#include "aof.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>

#include "../cmd/command.h"
#include "../core/config.h"
#include "../core/server.h"
#include "../core/shard.h"
#include "../core/thread.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../net/uring.h"
#include "../snapshot/snapshot.h"
#include "../store/flatstore.h"

namespace tomo {
namespace {

constexpr uint8_t kFileMagic[8] = {'T','O','M','O','A','O','F','\0'};
constexpr uint32_t kFileVersion = 1;
constexpr uint32_t kFileHeaderBytes = 80;
constexpr uint32_t kFrameHeaderBytes = 40;
constexpr uint32_t kFrameTag = 0x4d524641;       // "AFRM"
constexpr uint32_t kRecordHeaderBytes = 40;
constexpr uint32_t kRecordTag = 0x43524f41;      // "AORC"
constexpr uint32_t kWriterFramesPerPass = 16;
constexpr uint32_t kAofAlwaysMaxChains = 8;

enum AofIoRole : uint8_t { AofIoWrite = 1, AofIoSync = 2 };

struct AofControlWait {
    bool done = false;
    int result = 0;
};

struct AofIoRequest {
    AofIoRole role = AofIoWrite;
    int fd = -1;
    uint64_t offset = 0;
    uint64_t safe_offset = 0;
    size_t remaining = 0;
    iovec vectors[2]{};
    uint32_t vector_count = 0;
    uint32_t vector_index = 0;
    uint8_t header[kFileHeaderBytes]{};
    std::vector<std::array<uint8_t, kFrameHeaderBytes>> frame_headers;
    std::vector<std::unique_ptr<AofChunk>> chunks;
    std::vector<iovec> batch_vectors;
    AofIoRequest* batch_sync = nullptr;
    AofControlWait* waiter = nullptr;
    uint64_t sync_target = 0;
    std::vector<uint64_t> sync_sequences;
    bool contains_group_commit = false;
    bool everysec = false;
    bool had_short = false;
};

void consume_aof_iovecs(AofIoRequest& request, size_t bytes) {
    iovec* vectors = request.batch_vectors.empty()
        ? request.vectors : request.batch_vectors.data();
    while (request.vector_count && bytes >= vectors[request.vector_index].iov_len) {
        bytes -= vectors[request.vector_index].iov_len;
        request.vector_index++;
        request.vector_count--;
    }
    if (bytes && request.vector_count) {
        iovec& vector = vectors[request.vector_index];
        vector.iov_base = static_cast<uint8_t*>(vector.iov_base) + bytes;
        vector.iov_len -= bytes;
    }
}

io_uring_sqe* queue_aof_write(Ring& ring, AofIoRequest& request) {
    if (!request.vector_count) return nullptr;
    io_uring_sqe* sqe = ring.sqe();
    if (!sqe) return nullptr;
    iovec* vectors = request.batch_vectors.empty()
        ? request.vectors : request.batch_vectors.data();
    io_uring_prep_writev(sqe, request.fd, vectors + request.vector_index,
                         static_cast<unsigned>(request.vector_count), request.offset);
    sqe->user_data = ur_tag(UrKind::AofIo, &request);
    ring.note_pending();
    return sqe;
}

io_uring_sqe* queue_aof_sync(Ring& ring, AofIoRequest& request, bool drain) {
    io_uring_sqe* sqe = ring.sqe();
    if (!sqe) return nullptr;
    io_uring_prep_fsync(sqe, request.fd, IORING_FSYNC_DATASYNC);
    // Never use IOSQE_IO_DRAIN on this shared ring: multishot accepts are deliberately permanent
    // earlier requests, so a drain can never issue. Ordering comes from the linked data write; a
    // tail sync without a current write targets only the already-completed written frontier.
    (void)drain;
    sqe->user_data = ur_tag(UrKind::AofIo, &request);
    ring.note_pending();
    return sqe;
}

struct AofManifestData {
    std::string base_name;
    uint64_t base_sequence = 0;
    uint64_t base_epoch = 0;
    uint64_t base_commit = 0;
    uint64_t base_size = 0;
    uint64_t rewrite_base_size = 0;
    std::vector<std::pair<uint64_t, std::string>> increments;
    std::vector<std::vector<uint32_t>> increment_starts;
};

bool parse_sequence_starts(const std::string& text, std::vector<uint32_t>& starts) {
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = text.find(',', begin);
        const std::string field = text.substr(begin, end == std::string::npos
                                                       ? std::string::npos : end - begin);
        if (field.empty()) return false;
        char* tail = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(field.c_str(), &tail, 10);
        if (errno || !tail || *tail || value > UINT32_MAX) return false;
        starts.push_back(static_cast<uint32_t>(value));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !starts.empty();
}

std::string aof_segment_name(const std::string& basename, uint64_t sequence) {
    return basename + "." + std::to_string(sequence) + ".incr.tomo";
}

std::string aof_base_name(const std::string& basename, uint64_t sequence) {
    return basename + "." + std::to_string(sequence) + ".base.tomo";
}

int64_t realtime_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t monotonic_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool plain_name(const char* value) {
    return value && *value && !std::strchr(value, '/');
}

bool write_counted(int fd, const uint8_t* bytes, size_t length, uint64_t& offset) {
    while (length) {
        const ssize_t n = ::write(fd, bytes, length);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        bytes += n;
        length -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

bool write_frame_counted(int fd, const uint8_t* header, size_t header_length,
                         const uint8_t* payload, size_t payload_length, uint64_t& offset) {
    iovec vectors[2] = {
        {const_cast<uint8_t*>(header), header_length},
        {const_cast<uint8_t*>(payload), payload_length},
    };
    size_t first = 0;
    while (first != 2) {
        const ssize_t n = ::writev(fd, vectors + first, static_cast<int>(2 - first));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        offset += static_cast<uint64_t>(n);
        size_t consumed = static_cast<size_t>(n);
        while (first != 2 && consumed >= vectors[first].iov_len) {
            consumed -= vectors[first].iov_len;
            first++;
        }
        if (first != 2 && consumed) {
            vectors[first].iov_base = static_cast<uint8_t*>(vectors[first].iov_base) + consumed;
            vectors[first].iov_len -= consumed;
        }
    }
    return true;
}

bool read_file(int fd, std::vector<uint8_t>& out, std::string& error) {
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        error = "could not stat AOF";
        return false;
    }
    try {
        out.resize(static_cast<size_t>(st.st_size));
    } catch (const std::bad_alloc&) {
        error = "out of memory reading AOF";
        return false;
    }
    size_t offset = 0;
    while (offset < out.size()) {
        const ssize_t n = ::pread(fd, out.data() + offset, out.size() - offset,
                                  static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = "short AOF read";
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool read_manifest(const std::string& path, bool& exists, AofManifestData& manifest,
                   std::string& error) {
    exists = false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        error = "could not open AOF manifest";
        return false;
    }
    exists = true;
    std::vector<uint8_t> bytes;
    if (!read_file(fd, bytes, error)) { ::close(fd); return false; }
    ::close(fd);
    std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != "TOMOAOF-MANIFEST 1") {
        error = "invalid AOF manifest header";
        return false;
    }
    bool saw_base = false;
    bool saw_rewrite_size = false;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream fields(line);
        std::string word, name, seq_word, type_word, type, extra;
        if (!(fields >> word)) continue;
        if (word == "file") {
            uint64_t sequence = 0;
            if (!(fields >> name >> seq_word >> sequence >> type_word >> type) ||
                seq_word != "seq" || type_word != "type" || !plain_name(name.c_str())) {
                error = "invalid AOF manifest file entry";
                return false;
            }
            if (type == "b") {
                std::string epoch_word, commit_word, size_word;
                uint64_t epoch = 0, commit = 0, size = 0;
                if (saw_base || sequence == 0 ||
                    !(fields >> epoch_word >> epoch >> commit_word >> commit
                                          >> size_word >> size) ||
                    epoch_word != "epoch" || commit_word != "commit" || size_word != "size" ||
                    (fields >> extra)) {
                    error = "invalid AOF manifest base entry";
                    return false;
                }
                saw_base = true;
                manifest.base_name = name;
                manifest.base_sequence = sequence;
                manifest.base_epoch = epoch;
                manifest.base_commit = commit;
                manifest.base_size = size;
            } else if (type == "i") {
                std::string start_word, starts_text;
                std::vector<uint32_t> starts;
                if (!(fields >> start_word >> starts_text) || start_word != "start" ||
                    !parse_sequence_starts(starts_text, starts) || (fields >> extra) ||
                    sequence == 0) {
                    error = "invalid AOF manifest increment entry";
                    return false;
                }
                manifest.increments.emplace_back(sequence, name);
                manifest.increment_starts.push_back(std::move(starts));
            } else {
                error = "invalid AOF manifest file type";
                return false;
            }
        } else if (word == "rewrite-base-size") {
            uint64_t size = 0;
            if (saw_rewrite_size || !(fields >> size) || (fields >> extra)) {
                error = "invalid AOF manifest rewrite size";
                return false;
            }
            saw_rewrite_size = true;
            manifest.rewrite_base_size = size;
        } else {
            error = "unknown AOF manifest entry";
            return false;
        }
    }
    if (!saw_rewrite_size) {
        error = "AOF manifest has no rewrite base size";
        return false;
    }
    if (manifest.increments.empty()) {
        error = "AOF manifest has no increment file";
        return false;
    }
    std::unordered_set<std::string> names;
    if (!manifest.base_name.empty()) names.insert(manifest.base_name);
    for (const auto& increment : manifest.increments) {
        if (!names.insert(increment.second).second) {
            error = "AOF manifest repeats a file name";
            return false;
        }
    }
    for (size_t i = 1; i < manifest.increments.size(); i++) {
        if (manifest.increments[i].first != manifest.increments[i - 1].first + 1) {
            error = "AOF manifest increment sequences are not contiguous";
            return false;
        }
    }
    return true;
}

const char* hook_error(SnapshotHookStatus status) {
    switch (status) {
        case SnapshotHookStatus::Unsupported: return "AOF contains an unsupported value type";
        case SnapshotHookStatus::Corrupt: return "corrupt AOF type payload";
        case SnapshotHookStatus::Oom: return "out of memory loading AOF";
        case SnapshotHookStatus::Ok: break;
    }
    return "AOF type hook failed";
}

bool parse_record_bounds(const std::vector<uint8_t>& section, size_t pos,
                         AofRecordKind& kind, uint8_t& type, uint8_t& encoding,
                         uint32_t& key_len, uint64_t& payload_len, int64_t& expire,
                         uint64_t& group, size_t& next, std::string& error) {
    if (section.size() - pos < kRecordHeaderBytes) {
        error = "truncated AOF record";
        return false;
    }
    const uint8_t* h = section.data() + pos;
    if (snapshot_get_u32(h) != kRecordTag || h[7] != 1 ||
        snapshot_get_u32(h + 12) != kRecordHeaderBytes) {
        error = "invalid AOF record header";
        return false;
    }
    kind = static_cast<AofRecordKind>(h[4]);
    type = h[5];
    encoding = h[6];
    key_len = snapshot_get_u32(h + 8);
    payload_len = snapshot_get_u64(h + 16);
    expire = static_cast<int64_t>(snapshot_get_u64(h + 24));
    group = snapshot_get_u64(h + 32);
    const uint64_t remaining = static_cast<uint64_t>(section.size() - pos - kRecordHeaderBytes);
    if (payload_len > UINT32_MAX || remaining < static_cast<uint64_t>(key_len) + payload_len) {
        error = "invalid AOF record lengths";
        return false;
    }
    next = pos + kRecordHeaderBytes + key_len + static_cast<size_t>(payload_len);
    return true;
}

}  // namespace

std::string aof_directory_path(const Config& config) {
    const char* dir = config.dir && *config.dir ? config.dir : ".";
    const char* appenddir = plain_name(config.appenddirname) ? config.appenddirname : "appendonlydir";
    return std::string(dir) + "/" + appenddir;
}

std::string aof_file_path(const Config& config) {
    const char* filename = plain_name(config.appendfilename) ? config.appendfilename : "appendonly.aof";
    return aof_directory_path(config) + "/" + aof_segment_name(filename, 1);
}

void AofProducer::init(AofManager* manager, int32_t sid, uint32_t next_sequence) {
    manager_ = manager;
    sid_ = sid;
    sequence_ = next_sequence;
}

bool AofProducer::enabled() const { return manager_ && manager_->recording(); }

std::unique_ptr<AofChunk> AofProducer::make_chunk(uint32_t flags) {
    try {
        auto chunk = std::make_unique<AofChunk>();
        chunk->sid = sid_;
        chunk->flags = flags;
        chunk->bytes.reserve(kAofChunkBytes);
        return chunk;
    } catch (const std::bad_alloc&) {
        if (manager_) manager_->fail("out of memory allocating AOF chunk");
        return nullptr;
    }
}

std::unique_ptr<AofChunk> AofProducer::make_group_chunk(uint32_t flags) {
    try {
        auto chunk = std::make_unique<AofChunk>();
        chunk->sid = sid_;
        chunk->flags = flags;
        chunk->bytes.reserve(kAofChunkBytes);
        return chunk;
    } catch (const std::bad_alloc&) {
        if (manager_) manager_->fail("out of memory allocating AOF group chunk");
        return nullptr;
    }
}

bool AofProducer::group_seal(PendingGroup& group, uint32_t flags) {
    if (!group.build) return true;
    group.build->flags |= flags;
    try { group.chunks.push_back(std::move(group.build)); }
    catch (const std::bad_alloc&) {
        if (manager_) manager_->fail("out of memory retaining AOF group chunk");
        return false;
    }
    return true;
}

bool AofProducer::group_emit(PendingGroup& group, const uint8_t* bytes, uint64_t length,
                             bool large) {
    while (length) {
        if (!group.build) {
            group.build = make_group_chunk(large && group.chunks.empty()
                                           ? static_cast<uint32_t>(AofFrameLargeBegin) : 0);
            if (!group.build) return false;
        }
        const size_t room = kAofChunkBytes - group.build->bytes.size();
        if (!room) {
            if (!group_seal(group, 0)) return false;
            continue;
        }
        const size_t take = static_cast<size_t>(std::min<uint64_t>(length, room));
        try { group.build->bytes.insert(group.build->bytes.end(), bytes, bytes + take); }
        catch (const std::bad_alloc&) {
            if (manager_) manager_->fail("out of memory staging AOF group bytes");
            return false;
        }
        bytes += take;
        length -= take;
        if (large && group.build->bytes.size() == kAofChunkBytes && length)
            if (!group_seal(group, 0)) return false;
    }
    return true;
}

bool AofProducer::record_group_bytes(PendingGroup& group, AofRecordKind kind, uint8_t type,
                                     uint8_t encoding, Slice key, int64_t expire_at_ms,
                                     const SnapshotTypeHooks* hooks,
                                     SnapshotSaveCursor* cursor) {
    const uint64_t payload_len = cursor ? cursor->total : 0;
    if (payload_len > UINT32_MAX || payload_len > UINT64_MAX - kRecordHeaderBytes - key.n) {
        if (manager_) manager_->fail("AOF group record is too large");
        return false;
    }
    const uint64_t total = kRecordHeaderBytes + static_cast<uint64_t>(key.n) + payload_len;
    const bool large = total > kAofChunkBytes;
    if (large) {
        if (!group_seal(group, 0)) return false;
    } else if (group.build && group.build->bytes.size() + total > kAofChunkBytes) {
        if (!group_seal(group, 0)) return false;
    }
    if (!group.build) {
        group.build = make_group_chunk(
            large ? static_cast<uint32_t>(AofFrameLargeBegin) : 0);
        if (!group.build) return false;
    }
    // The fixed header always begins in one frame, so the owner can patch the final live ticket
    // after the visibility publish without touching another owner's staging memory.
    if (kAofChunkBytes - group.build->bytes.size() < kRecordHeaderBytes) {
        if (!group_seal(group, 0)) return false;
        group.build = make_group_chunk(
            large ? static_cast<uint32_t>(AofFrameLargeBegin) : 0);
        if (!group.build) return false;
    }
    const uint32_t ticket_offset = static_cast<uint32_t>(group.build->bytes.size() + 32);
    try { group.build->group_ticket_offsets.push_back(ticket_offset); }
    catch (const std::bad_alloc&) {
        if (manager_) manager_->fail("out of memory staging AOF group ticket");
        return false;
    }

    uint8_t header[kRecordHeaderBytes] = {};
    snapshot_put_u32(header, kRecordTag);
    header[4] = static_cast<uint8_t>(kind);
    header[5] = type;
    header[6] = encoding;
    header[7] = 1;
    snapshot_put_u32(header + 8, key.n);
    snapshot_put_u32(header + 12, kRecordHeaderBytes);
    snapshot_put_u64(header + 16, payload_len);
    snapshot_put_u64(header + 24, static_cast<uint64_t>(expire_at_ms));
    if (!group_emit(group, header, sizeof(header), large) ||
        !group_emit(group, reinterpret_cast<const uint8_t*>(key.p), key.n, large)) return false;

    if (hooks && cursor) {
        while (cursor->offset < cursor->total) {
            if (!group.build) {
                group.build = make_group_chunk();
                if (!group.build) return false;
            }
            size_t room = kAofChunkBytes - group.build->bytes.size();
            if (!room) {
                if (!group_seal(group, 0)) return false;
                continue;
            }
            const size_t capacity = static_cast<size_t>(std::min<uint64_t>(
                room, cursor->total - cursor->offset));
            const size_t old_size = group.build->bytes.size();
            try { group.build->bytes.resize(old_size + capacity); }
            catch (const std::bad_alloc&) {
                if (manager_) manager_->fail("out of memory staging AOF group value");
                return false;
            }
            size_t written = 0;
            const SnapshotHookStatus status = hooks->read_save(
                *cursor, group.build->bytes.data() + old_size, capacity, written);
            if (status != SnapshotHookStatus::Ok || written > capacity ||
                (written == 0 && cursor->offset < cursor->total)) {
                group.build->bytes.resize(old_size);
                if (manager_) manager_->fail("AOF group serialization failed");
                return false;
            }
            group.build->bytes.resize(old_size + written);
        }
    }
    if (large) {
        if (!group.build) return false;
        group.build->records = 1;
        return group_seal(group, AofFrameLargeEnd);
    }
    group.build->records++;
    return true;
}

bool AofProducer::begin_group(const std::shared_ptr<AofGroupDecision>& decision) {
    if (!enabled() || !decision) return !manager_ || !manager_->failed();
    if (active_group_) {
        manager_->fail("nested AOF group staging");
        return false;
    }
    if (build_ && !seal(0, nullptr)) return false;
    try {
        staged_.emplace_back();
        active_group_ = &staged_.back().group;
        active_group_->decision = decision;
        return true;
    } catch (const std::bad_alloc&) {
        manager_->fail("out of memory starting AOF group");
        return false;
    }
}

bool AofProducer::record_group_post_image_impl(FlatStore& store, uint64_t hash, Slice key,
                                               bool physical) {
    if (!active_group_) return false;
    PendingGroup& group = *active_group_;
    KvObj* object = physical ? store.aof_physical(hash, key) : store.find(hash, key);
    if (!object) return record_group_bytes(group, AofRecordKind::GroupDel,
                                           0, 0, key, -1, nullptr, nullptr);
    const SnapshotTypeHooks& hooks = snapshot_type_hooks(static_cast<Type>(object->type));
    SnapshotSaveCursor cursor;
    uint8_t encoding = 0;
    if (!hooks.begin_save || !hooks.read_save ||
        hooks.begin_save(*object, cursor, encoding) != SnapshotHookStatus::Ok) {
        manager_->fail("AOF group type serialization failed");
        return false;
    }
    return record_group_bytes(group, AofRecordKind::GroupPut,
                              object->type, encoding, object->key(),
                              object->expire_at_ms(), &hooks, &cursor);
}

bool AofProducer::record_group_post_image(FlatStore& store, uint64_t hash, Slice key) {
    return record_group_post_image_impl(store, hash, key, true);
}

bool AofProducer::record_group_visible_post_image(FlatStore& store, uint64_t hash, Slice key) {
    return record_group_post_image_impl(store, hash, key, false);
}

bool AofProducer::finish_group() {
    if (!active_group_) return false;
    PendingGroup& group = *active_group_;
    bool ok = group_seal(group, 0);
    if (group.chunks.empty()) {
        if (manager_) manager_->fail("AOF group has no fragments");
        ok = false;
    } else {
        group.chunks.back()->group_fragment_last = true;
    }
    active_group_ = nullptr;
    return ok;
}

bool AofProducer::make_ready(std::unique_ptr<AofChunk> chunk) {
    if (!chunk) return true;
    chunk->sid = sid_;
    chunk->sequence = sequence_++;
    try {
        ready_.push_back(std::move(chunk));
        return true;
    } catch (const std::bad_alloc&) {
        manager_->fail("out of memory publishing AOF chunk");
        return false;
    }
}

bool AofProducer::resolve_groups() {
    if (active_group_) return false;
    while (!staged_.empty()) {
        StagedItem& item = staged_.front();
        if (item.plain) {
            if (!make_ready(std::move(item.plain))) return false;
            staged_.pop_front();
            continue;
        }
        PendingGroup& group = item.group;
        if (group.decision->aborted.load(std::memory_order_acquire)) {
            staged_.pop_front();
            continue;
        }
        const uint64_t ticket = group.decision->ticket.load(std::memory_order_acquire);
        if (!ticket) break;
        for (auto& chunk : group.chunks) {
            chunk->group = group.decision;
            for (uint32_t offset : chunk->group_ticket_offsets) {
                if (offset + sizeof(uint64_t) > chunk->bytes.size()) {
                    manager_->fail("invalid AOF group ticket offset");
                    return false;
                }
                snapshot_put_u64(chunk->bytes.data() + offset, ticket);
            }
            if (!make_ready(std::move(chunk))) return false;
        }
        staged_.pop_front();
    }
    return true;
}

bool AofProducer::post_ready(AofOwnerContext& context) {
    if (!manager_ || !context.ring || !context.signals) return false;
    while (!ready_.empty()) {
        std::unique_ptr<AofChunk>& chunk = ready_.front();
        if (!manager_->post_chunk(context.producer, chunk, *context.ring, *context.signals))
            return false;
        ready_.erase(ready_.begin());
    }
    return true;
}

bool AofProducer::seal(uint32_t flags, AofOwnerContext* context) {
    if (!build_) return true;
    build_->flags |= flags;
    try {
        if (staged_.empty()) {
            if (!make_ready(std::move(build_))) return false;
        } else {
            staged_.emplace_back();
            staged_.back().plain = std::move(build_);
        }
    } catch (const std::bad_alloc&) {
        manager_->fail("out of memory retaining ordered AOF chunk");
        return false;
    }
    if (!context) return true;
    while (!post_ready(*context)) {
        if (!manager_ || manager_->failed()) return false;
        context->ring->submit_and_reap();
        std::this_thread::yield();
    }
    return true;
}

bool AofProducer::emit(const uint8_t* bytes, uint64_t length, bool large,
                       AofOwnerContext* context) {
    while (length) {
        if (!build_) {
            build_ = make_chunk(
                large ? static_cast<uint32_t>(AofFrameLargeBegin) : 0);
            if (!build_) return false;
        }
        const size_t room = kAofChunkBytes - build_->bytes.size();
        if (!room) {
            if (!seal(0, context)) return false;
            build_ = make_chunk(0);
            if (!build_) return false;
            continue;
        }
        const size_t take = static_cast<size_t>(std::min<uint64_t>(length, room));
        try {
            build_->bytes.insert(build_->bytes.end(), bytes, bytes + take);
        } catch (const std::bad_alloc&) {
            if (manager_) manager_->fail("out of memory staging AOF bytes");
            return false;
        }
        bytes += take;
        length -= take;
        if (large && build_->bytes.size() == kAofChunkBytes && length)
            if (!seal(0, context)) return false;
    }
    return true;
}

bool AofProducer::maybe_timestamp(int64_t now_ms, AofOwnerContext* context) {
    if (!manager_ || !manager_->timestamp_enabled()) return true;
    const int64_t second = now_ms / 1000;
    if (second == timestamp_second_) return true;
    timestamp_second_ = second;
    return record_bytes(AofRecordKind::Timestamp, 0, 0, Slice(), -1,
                        static_cast<uint64_t>(second), nullptr, 0, nullptr, nullptr, context);
}

bool AofProducer::record_bytes(AofRecordKind kind, uint8_t type, uint8_t encoding, Slice key,
                               int64_t expire_at_ms, uint64_t group, const uint8_t* payload,
                               uint64_t payload_len, const SnapshotTypeHooks* hooks,
                               SnapshotSaveCursor* cursor, AofOwnerContext* context) {
    if (!manager_ || manager_->failed()) return false;
    if (kind != AofRecordKind::Timestamp && manager_->timestamp_enabled() &&
        !maybe_timestamp(realtime_ms(), context)) return false;
    if (payload_len > UINT32_MAX ||
        payload_len > UINT64_MAX - kRecordHeaderBytes - key.n) {
        manager_->fail("AOF record is too large");
        return false;
    }
    const uint64_t total = kRecordHeaderBytes + static_cast<uint64_t>(key.n) + payload_len;
    const bool large = total > kAofChunkBytes;
    if (large) {
        if (!seal(0, context)) return false;
    } else if (build_ && build_->bytes.size() + total > kAofChunkBytes) {
        if (!seal(0, context)) return false;
    }
    if (!build_) {
        build_ = make_chunk(
            large ? static_cast<uint32_t>(AofFrameLargeBegin) : 0);
        if (!build_) return false;
    }

    uint8_t header[kRecordHeaderBytes] = {};
    snapshot_put_u32(header, kRecordTag);
    header[4] = static_cast<uint8_t>(kind);
    header[5] = type;
    header[6] = encoding;
    header[7] = 1;
    snapshot_put_u32(header + 8, key.n);
    snapshot_put_u32(header + 12, kRecordHeaderBytes);
    snapshot_put_u64(header + 16, payload_len);
    snapshot_put_u64(header + 24, static_cast<uint64_t>(expire_at_ms));
    snapshot_put_u64(header + 32, group);
    if (!emit(header, sizeof(header), large, context) ||
        !emit(reinterpret_cast<const uint8_t*>(key.p), key.n, large, context)) return false;

    if (hooks && cursor) {
        while (cursor->offset < cursor->total) {
            if (!build_) {
                build_ = make_chunk(large ? 0 : 0);
                if (!build_) return false;
            }
            size_t room = kAofChunkBytes - build_->bytes.size();
            if (!room) {
                if (!seal(0, context)) return false;
                build_ = make_chunk(0);
                if (!build_) return false;
                room = kAofChunkBytes;
            }
            const size_t capacity = static_cast<size_t>(std::min<uint64_t>(
                room, cursor->total - cursor->offset));
            const size_t old_size = build_->bytes.size();
            try {
                build_->bytes.resize(old_size + capacity);
            } catch (const std::bad_alloc&) {
                manager_->fail("out of memory staging AOF value");
                return false;
            }
            size_t written = 0;
            const SnapshotHookStatus status = hooks->read_save(
                *cursor, build_->bytes.data() + old_size, capacity, written);
            if (status != SnapshotHookStatus::Ok || written > capacity ||
                (written == 0 && cursor->offset < cursor->total)) {
                build_->bytes.resize(old_size);
                manager_->fail("AOF type serialization failed");
                return false;
            }
            build_->bytes.resize(old_size + written);
        }
    } else if (payload_len && !emit(payload, payload_len, large, context)) {
        return false;
    }

    if (large) {
        if (!build_) return false;
        build_->records = 1;
        return seal(AofFrameLargeEnd, context);
    }
    build_->records++;
    return true;
}

bool AofProducer::record_post_image_impl(FlatStore& store, uint64_t hash, Slice key,
                                         AofOwnerContext* context, uint64_t group) {
    if (!enabled()) return true;
    KvObj* object = store.find(hash, key);
    if (!object) {
        const AofRecordKind kind = group ? AofRecordKind::GroupDel : AofRecordKind::Del;
        return record_bytes(kind, 0, 0, key, -1, group, nullptr, 0,
                            nullptr, nullptr, context);
    }
    const SnapshotTypeHooks& hooks = snapshot_type_hooks(static_cast<Type>(object->type));
    SnapshotSaveCursor cursor;
    uint8_t encoding = 0;
    if (!hooks.begin_save || !hooks.read_save ||
        hooks.begin_save(*object, cursor, encoding) != SnapshotHookStatus::Ok) {
        manager_->fail("AOF type serialization failed");
        return false;
    }
    const AofRecordKind kind = group ? AofRecordKind::GroupPut : AofRecordKind::Put;
    return record_bytes(kind, object->type, encoding, object->key(), object->expire_at_ms(),
                        group, nullptr, cursor.total, &hooks, &cursor, context);
}

bool AofProducer::record_post_image(FlatStore& store, uint64_t hash, Slice key,
                                    AofOwnerContext& context, uint64_t group) {
    return record_post_image_impl(store, hash, key, &context, group);
}

bool AofProducer::record_post_image_buffered(FlatStore& store, uint64_t hash, Slice key,
                                             uint64_t group) {
    return record_post_image_impl(store, hash, key, nullptr, group);
}

bool AofProducer::record_delete(Slice key, uint64_t group) {
    if (!enabled()) return true;
    const AofRecordKind kind = group ? AofRecordKind::GroupDel : AofRecordKind::Del;
    return record_bytes(kind, 0, 0, key, -1, group, nullptr, 0, nullptr, nullptr, nullptr);
}

bool AofProducer::record_flush() {
    if (!enabled()) return true;
    return record_bytes(AofRecordKind::Flush, 0, 0, Slice(), -1, 0,
                        nullptr, 0, nullptr, nullptr, nullptr);
}

bool AofProducer::flush(AofOwnerContext& context) {
    if (!manager_) return true;
    if (build_ && !seal(0, nullptr)) return false;
    if (!resolve_groups()) return false;
    return post_ready(context);
}

AofManager::~AofManager() {
    if (fd_ >= 0) ::close(fd_);
    discard_chunks();
}

void AofManager::init(Server& server, const Config& config, uint32_t nthreads, uint32_t nshards,
                      uint32_t writer_tid, const AofReplayPlan* replay) {
    server_ = &server;
    configured_ = config.appendonly;
    engine_ = config.persist_io;
    fsync_policy_.store(config.appendfsync, std::memory_order_relaxed);
    auto_rewrite_percentage_.store(config.auto_aof_rewrite_percentage,
                                   std::memory_order_relaxed);
    auto_rewrite_min_size_.store(config.auto_aof_rewrite_min_size,
                                 std::memory_order_relaxed);
    if (!configured_) return;
    nthreads_ = nthreads;
    nshards_ = nshards;
    writer_tid_ = writer_tid;
    directory_path_ = aof_directory_path(config);
    appendfilename_ = plain_name(config.appendfilename) ? config.appendfilename : "appendonly.aof";
    manifest_path_ = directory_path_ + "/" + appendfilename_ + ".manifest";
    AofManifestData manifest;
    bool manifest_exists = false;
    std::string manifest_error;
    if (!read_manifest(manifest_path_, manifest_exists, manifest, manifest_error)) {
        fail(manifest_error.c_str());
        return;
    }
    if (manifest_exists) {
        base_name_ = manifest.base_name;
        base_sequence_ = manifest.base_sequence;
        base_epoch_ = manifest.base_epoch;
        base_commit_ = manifest.base_commit;
        base_size_.store(manifest.base_size, std::memory_order_relaxed);
        rewrite_base_size_.store(manifest.rewrite_base_size, std::memory_order_relaxed);
        increments_ = manifest.increments;
        increment_starts_ = manifest.increment_starts;
        active_incr_sequence_ = increments_.back().first;
        file_path_ = directory_path_ + "/" + increments_.back().second;
    } else {
        active_incr_sequence_ = 1;
        increments_.emplace_back(active_incr_sequence_,
                                 aof_segment_name(appendfilename_, active_incr_sequence_));
        increment_starts_.push_back(std::vector<uint32_t>(nshards_, 0));
        file_path_ = directory_path_ + "/" + increments_.back().second;
    }
    timestamp_enabled_.store(config.aof_timestamp_enabled, std::memory_order_relaxed);
    chunk_in_ = std::make_unique<ChunkChan[]>(nthreads_);
    next_sequence_.assign(nshards_, 0);
    if (replay && replay->next_sequence.size() == nshards_)
        next_sequence_ = replay->next_sequence;
    if (replay) {
        replayed_records_.store(replay->replayed_records, std::memory_order_relaxed);
        groups_skipped_.store(replay->groups_skipped, std::memory_order_relaxed);
        groups_committed_.store(replay->committed_groups.size(), std::memory_order_relaxed);
    }
}

std::shared_ptr<AofGroupDecision> aof_create_group(
        AofManager& manager, const std::vector<uint32_t>& participants) {
    if (!manager.recording() || participants.empty()) return nullptr;
    try {
        auto group = std::make_shared<AofGroupDecision>();
        group->participants = participants;
        std::sort(group->participants.begin(), group->participants.end());
        group->participants.erase(
            std::unique(group->participants.begin(), group->participants.end()),
            group->participants.end());
        group->dependencies.resize(group->participants.size());
        group->dependency_seen.assign(group->participants.size(), 0);
        for (size_t i = 0; i < group->participants.size(); i++)
            group->dependencies[i].sid = group->participants[i];
        return group;
    } catch (const std::bad_alloc&) {
        manager.fail("out of memory allocating AOF group decision");
        return nullptr;
    }
}

void aof_abort_group(const std::shared_ptr<AofGroupDecision>& group) {
    if (group) group->aborted.store(true, std::memory_order_release);
}

bool aof_commit_group(AofManager& manager, const std::shared_ptr<AofGroupDecision>& group,
                      uint64_t ticket, AofOwnerContext& context) {
    if (!group) return true;
    if (!ticket || group->aborted.load(std::memory_order_acquire)) return false;
    group->ticket.store(ticket, std::memory_order_release);
    bool expected = false;
    if (!group->commit_enqueued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return true;
    std::unique_ptr<AofChunk> chunk;
    try {
        chunk = std::make_unique<AofChunk>();
        chunk->sid = -1;
        chunk->group = group;
        chunk->group_commit = true;
    } catch (const std::bad_alloc&) {
        manager.fail("out of memory allocating AOF GCMT");
        return false;
    }
    while (!manager.post_chunk(context.producer, chunk, *context.ring, *context.signals)) {
        if (manager.failed()) return false;
        context.ring->submit_and_reap();
        std::this_thread::yield();
    }
    return true;
}

void AofManager::fail(const char* message) {
    last_error_ = message ? message : "AOF failure";
    failed_.store(true, std::memory_order_release);
    recording_.store(false, std::memory_order_release);
    std::fprintf(stderr, "AOF error: %s\n", last_error_.c_str());
}

bool AofManager::write_header_normal() {
    uint8_t header[kFileHeaderBytes] = {};
    std::memcpy(header, kFileMagic, sizeof(kFileMagic));
    snapshot_put_u32(header + 8, kFileVersion);
    snapshot_put_u32(header + 12, kFileHeaderBytes);
    snapshot_put_u32(header + 16, nshards_);
    snapshot_put_u32(header + 20, static_cast<uint32_t>(g_hash_kind));
    snapshot_put_u64(header + 24, static_cast<uint64_t>(realtime_ms()));
    snapshot_put_u64(header + 32, g_hash_seed);
    snapshot_put_u64(header + 40, g_sip_k0);
    snapshot_put_u64(header + 48, g_sip_k1);
    snapshot_put_u64(header + 64, snapshot_checksum(header, 64));
    return write_counted(fd_, header, sizeof(header), file_offset_);
}

bool AofManager::wait_control_write(ThreadCtx& writer, Ring& ring, int fd,
                                    const uint8_t* bytes, size_t length, uint64_t& offset) {
    if (length > kFileHeaderBytes) return false;
    auto* request = new (std::nothrow) AofIoRequest();
    if (!request) return false;
    AofControlWait wait;
    request->fd = fd;
    request->offset = offset;
    request->remaining = length;
    request->waiter = &wait;
    std::memcpy(request->header, bytes, length);
    request->vectors[0] = {request->header, length};
    request->vector_count = 1;
    if (!queue_aof_write(ring, *request)) { delete request; return false; }
    io_inflight_++;
    while (!wait.done) {
        ring.submit_and_wait(1);
        pump_io_completions(writer, ring);
    }
    if (wait.result < 0) return false;
    offset += length;
    return true;
}

bool AofManager::write_header_uring(ThreadCtx& writer, Ring& ring, int fd, uint64_t& offset) {
    uint8_t header[kFileHeaderBytes] = {};
    std::memcpy(header, kFileMagic, sizeof(kFileMagic));
    snapshot_put_u32(header + 8, kFileVersion);
    snapshot_put_u32(header + 12, kFileHeaderBytes);
    snapshot_put_u32(header + 16, nshards_);
    snapshot_put_u32(header + 20, static_cast<uint32_t>(g_hash_kind));
    snapshot_put_u64(header + 24, static_cast<uint64_t>(realtime_ms()));
    snapshot_put_u64(header + 32, g_hash_seed);
    snapshot_put_u64(header + 40, g_sip_k0);
    snapshot_put_u64(header + 48, g_sip_k1);
    snapshot_put_u64(header + 64, snapshot_checksum(header, 64));
    return wait_control_write(writer, ring, fd, header, sizeof(header), offset);
}

bool AofManager::wait_control_sync(ThreadCtx& writer, Ring& ring, int fd) {
    auto* request = new (std::nothrow) AofIoRequest();
    if (!request) return false;
    AofControlWait wait;
    request->role = AofIoSync;
    request->fd = fd;
    request->waiter = &wait;
    if (!queue_aof_sync(ring, *request, true)) { delete request; return false; }
    io_inflight_++;
    fsync_inflight_++;
    while (!wait.done) {
        ring.submit_and_wait(1);
        pump_io_completions(writer, ring);
    }
    return wait.result >= 0;
}

bool AofManager::bind_writer(ThreadCtx& writer, Ring& ring, std::string& error) {
    if (!configured_) return true;
    if (writer.id() != writer_tid_) {
        while (!writer_ready_.load(std::memory_order_acquire) &&
               !failed_.load(std::memory_order_acquire)) std::this_thread::yield();
        if (failed()) error = last_error_;
        return !failed();
    }
    if (::mkdir(directory_path_.c_str(), 0755) != 0 && errno != EEXIST) {
        error = "could not create appenddirname";
        fail(error.c_str());
        return false;
    }
    struct stat directory{};
    if (::stat(directory_path_.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode)) {
        error = "appenddirname is not a directory";
        fail(error.c_str());
        return false;
    }
    cleanup_unreferenced_files();
    const int append_flag = engine_ == PersistIoEngine::Normal ? O_APPEND : 0;
    fd_ = ::open(file_path_.c_str(), O_CREAT | O_RDWR | append_flag | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        error = "could not open AOF file";
        fail(error.c_str());
        return false;
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0 || st.st_size < 0) {
        error = "could not stat AOF file";
        fail(error.c_str());
        return false;
    }
    file_offset_ = static_cast<uint64_t>(st.st_size);
    const bool header_ok = file_offset_ != 0 ||
        (engine_ == PersistIoEngine::Normal ? write_header_normal()
                                            : write_header_uring(writer, ring, fd_, file_offset_));
    if (!header_ok) {
        error = "could not write AOF header";
        fail(error.c_str());
        return false;
    }
    last_good_offset_ = file_offset_;
    const uint64_t current = base_size() + file_offset_;
    current_size_.store(current, std::memory_order_relaxed);
    if (rewrite_base_size() == 0)
        rewrite_base_size_.store(current, std::memory_order_relaxed);
    writer_ring_.store(&ring, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
    writer_ready_.store(true, std::memory_order_release);
    return true;
}

void AofManager::cleanup_unreferenced_files() {
    DIR* directory = ::opendir(directory_path_.c_str());
    if (!directory) return;
    std::unordered_set<std::string> keep;
    if (!base_name_.empty()) keep.insert(base_name_);
    for (const auto& increment : increments_) keep.insert(increment.second);
    uint64_t removed = 0;
    while (dirent* entry = ::readdir(directory)) {
        const std::string name(entry->d_name);
        if (keep.count(name) || name.rfind(appendfilename_ + ".", 0) != 0) continue;
        const bool segment = name.find(".base.tomo") != std::string::npos ||
                             name.find(".incr.tomo") != std::string::npos;
        if (!segment) continue;
        if (::unlink((directory_path_ + "/" + name).c_str()) == 0) removed++;
    }
    (void)::closedir(directory);
    if (removed) {
        history_unlinks_.fetch_add(removed, std::memory_order_relaxed);
        const int directory_fd = ::open(directory_path_.c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd >= 0) {
            (void)::fsync(directory_fd);
            (void)::close(directory_fd);
        }
    }
}

bool AofManager::persist_manifest(
        const std::string& base_name, uint64_t base_sequence, uint64_t base_epoch,
        uint64_t base_commit, uint64_t persisted_base_size,
        uint64_t persisted_rewrite_base_size,
        const std::vector<std::pair<uint64_t, std::string>>& increments,
        const std::vector<std::vector<uint32_t>>& increment_starts,
        std::string& error) {
    if (increments.size() != increment_starts.size()) {
        error = "AOF manifest increment metadata is incomplete";
        return false;
    }
    std::string contents = "TOMOAOF-MANIFEST 1\n";
    if (!base_name.empty()) {
        contents += "file " + base_name + " seq " + std::to_string(base_sequence) +
                    " type b epoch " + std::to_string(base_epoch) + " commit " +
                    std::to_string(base_commit) + " size " +
                    std::to_string(persisted_base_size) + "\n";
    }
    for (size_t index = 0; index < increments.size(); index++) {
        const auto& increment = increments[index];
        contents += "file " + increment.second + " seq " +
                    std::to_string(increment.first) + " type i start ";
        for (size_t sid = 0; sid < increment_starts[index].size(); sid++) {
            if (sid) contents.push_back(',');
            contents += std::to_string(increment_starts[index][sid]);
        }
        contents.push_back('\n');
    }
    contents += "rewrite-base-size " + std::to_string(persisted_rewrite_base_size) + "\n";

    const std::string temp = manifest_path_ + ".tmp." + std::to_string(::getpid()) + "." +
                             std::to_string(rewrite_target_sequence_);
    const int manifest_fd = ::open(temp.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (manifest_fd < 0) { error = "could not create AOF manifest temporary file"; return false; }
    uint64_t offset = 0;
    bool ok = write_counted(manifest_fd,
                            reinterpret_cast<const uint8_t*>(contents.data()),
                            contents.size(), offset) && ::fsync(manifest_fd) == 0;
    if (::close(manifest_fd) != 0) ok = false;
    if (ok) ok = ::rename(temp.c_str(), manifest_path_.c_str()) == 0;
    if (ok) {
        const int directory_fd = ::open(directory_path_.c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0) ok = false;
        else {
            ok = ::fsync(directory_fd) == 0;
            if (::close(directory_fd) != 0) ok = false;
        }
    }
    if (!ok) {
        (void)::unlink(temp.c_str());
        error = "could not persist AOF manifest";
    }
    return ok;
}

bool AofManager::schedule_rewrite(bool automatic) {
    if (!recording() || failed() || rewrite_in_progress()) return false;
    bool expected = false;
    if (!rewrite_requested_.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) return false;
    rewrite_requests_.fetch_add(1, std::memory_order_relaxed);
    if (automatic) auto_rewrite_triggers_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool AofManager::request_rewrite() {
    return schedule_rewrite(false);
}

void AofManager::maybe_schedule_auto_rewrite() {
    const uint32_t percentage = auto_rewrite_percentage();
    // This branch is deliberately before every size load/arithmetic operation: zero is the exact
    // no-auto-work setting promised by the Redis-compatible knob.
    if (percentage == 0) return;
    if (failed() || rewrite_in_progress() || rewrite_scheduled()) return;
    const int64_t now = monotonic_ms();
    if (consecutive_rewrite_failures() >= 3 &&
        now < next_rewrite_retry_ms_.load(std::memory_order_relaxed)) {
        if (!backoff_reported_) {
            auto_rewrite_backoff_skips_.fetch_add(1, std::memory_order_relaxed);
            backoff_reported_ = true;
        }
        return;
    }
    backoff_reported_ = false;
    const uint64_t current = current_size();
    if (current <= auto_rewrite_min_size()) return;
    const uint64_t baseline = rewrite_base_size();
    if (current <= baseline) return;
    const unsigned __int128 growth =
        static_cast<unsigned __int128>(current - baseline) * 100;
    const unsigned __int128 required =
        static_cast<unsigned __int128>(baseline) * percentage;
    if (growth < required) return;
    (void)schedule_rewrite(true);
}

void AofManager::maybe_pause_rewrite(AofRewriteDebugStage stage) {
    if (debug_rewrite_pause_.load(std::memory_order_acquire) != stage) return;
    const std::string marker = directory_path_ + "/debug-aof-rewrite-stage";
    const int marker_fd = ::open(marker.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (marker_fd >= 0) {
        const char* text = stage == AofRewriteDebugStage::BeforeMark ? "before-mark\n" :
                           stage == AofRewriteDebugStage::BeforeManifest ? "before-manifest\n" :
                           "after-manifest\n";
        uint64_t offset = 0;
        (void)write_counted(marker_fd, reinterpret_cast<const uint8_t*>(text),
                            std::strlen(text), offset);
        (void)::fsync(marker_fd);
        (void)::close(marker_fd);
    }
    while (debug_rewrite_pause_.load(std::memory_order_acquire) == stage && server_ &&
           !server_->shutting_down().load(std::memory_order_relaxed)) {
        if (::access(marker.c_str(), F_OK) != 0) {
            debug_rewrite_pause_.store(AofRewriteDebugStage::None, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
    (void)::unlink(marker.c_str());
}

void AofManager::maybe_start_rewrite(ThreadCtx& writer, Ring& ring) {
    if (!rewrite_requested_.load(std::memory_order_acquire) || rewrite_in_progress() ||
        !server_ || server_->snapshot().in_progress() || server_->atomic_inflight() != 0) return;
    bool expected = false;
    if (!rewrite_in_progress_.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel)) return;
    rewrite_requested_.store(false, std::memory_order_release);
    rewrite_target_sequence_ = active_incr_sequence_ + 1;
    rewrite_base_name_ = aof_base_name(appendfilename_, rewrite_target_sequence_);
    maybe_pause_rewrite(AofRewriteDebugStage::BeforeMark);
    std::string error;
    const SnapshotManager::StartResult result = server_->snapshot().start(
        *server_, writer, ring, false, error, this, directory_path_.c_str(),
        rewrite_base_name_.c_str());
    if (result != SnapshotManager::StartResult::Started) {
        rewrite_abort();
        if (!error.empty()) std::fprintf(stderr, "AOF rewrite error: %s\n", error.c_str());
    }
}

bool AofManager::rewrite_mark(ThreadCtx& writer, Ring& ring, uint64_t snapshot_epoch,
                              int64_t, std::string& error) {
    if (!rewrite_in_progress() || writer.id() != writer_tid_ || fd_ < 0 || !server_) {
        error = "invalid AOF rewrite mark owner";
        return false;
    }
    for (uint32_t pass = 0; pending_chunks() && pass < 100000; pass++) {
        const uint32_t work = writer_pass(writer, ring, true);
        if (engine_ == PersistIoEngine::Uring) {
            ring.submit_and_reap();
            pump_io_completions(writer, ring);
        }
        if (!work) std::this_thread::yield();
    }
    if (pending_chunks()) { error = "timed out draining AOF at rewrite mark"; return false; }
    const bool old_synced = engine_ == PersistIoEngine::Normal
        ? ::fdatasync(fd_) == 0 : wait_control_sync(writer, ring, fd_);
    if (!old_synced) { error = "could not sync old AOF increment"; return false; }

    const uint64_t new_sequence = rewrite_target_sequence_;
    const std::string new_name = aof_segment_name(appendfilename_, new_sequence);
    const std::string new_path = directory_path_ + "/" + new_name;
    const int append_flag = engine_ == PersistIoEngine::Normal ? O_APPEND : 0;
    const int new_fd = ::open(new_path.c_str(),
                              O_CREAT | O_EXCL | O_RDWR | append_flag | O_CLOEXEC, 0600);
    if (new_fd < 0) { error = "could not create new AOF increment"; return false; }

    const int old_fd = fd_;
    const uint64_t old_offset = file_offset_;
    const uint64_t old_last_good = last_good_offset_;
    fd_ = new_fd;
    file_offset_ = 0;
    last_good_offset_ = 0;
    const bool header_ok = engine_ == PersistIoEngine::Normal
        ? write_header_normal() : write_header_uring(writer, ring, new_fd, file_offset_);
    const bool new_ok = header_ok && (engine_ == PersistIoEngine::Normal
        ? ::fdatasync(new_fd) == 0 : wait_control_sync(writer, ring, new_fd));
    const uint64_t new_offset = file_offset_;
    fd_ = old_fd;
    file_offset_ = old_offset;
    last_good_offset_ = old_last_good;
    if (!new_ok) {
        (void)::close(new_fd);
        (void)::unlink(new_path.c_str());
        error = "could not initialize new AOF increment";
        return false;
    }

    std::vector<std::pair<uint64_t, std::string>> staged = increments_;
    std::vector<std::vector<uint32_t>> staged_starts = increment_starts_;
    staged.emplace_back(new_sequence, new_name);
    staged_starts.push_back(next_sequence_);
    if (!persist_manifest(base_name_, base_sequence_, base_epoch_, base_commit_, base_size(),
                          rewrite_base_size(), staged, staged_starts, error)) {
        (void)::close(new_fd);
        (void)::unlink(new_path.c_str());
        return false;
    }

    rewrite_history_.clear();
    if (!base_name_.empty()) rewrite_history_.push_back(base_name_);
    for (const auto& increment : increments_) rewrite_history_.push_back(increment.second);
    increments_ = std::move(staged);
    increment_starts_ = std::move(staged_starts);
    (void)::close(old_fd);
    fd_ = new_fd;
    file_path_ = new_path;
    active_incr_sequence_ = new_sequence;
    file_offset_ = new_offset;
    last_good_offset_ = new_offset;
    large_record_offset_ = 0;
    locked_producer_ = UINT32_MAX;
    durable_sequence_.store(written_sequence_.load(std::memory_order_acquire),
                            std::memory_order_release);
    current_size_.store(base_size() + new_offset, std::memory_order_relaxed);
    last_fsync_ms_ = monotonic_ms();
    base_epoch_ = snapshot_epoch;
    base_commit_ = server_->atomic_snapshot();
    return true;
}

bool AofManager::rewrite_complete(const std::string& base_path, uint64_t snapshot_epoch) {
    if (!rewrite_in_progress() || snapshot_epoch != base_epoch_) return false;
    struct stat base_stat{};
    if (::stat(base_path.c_str(), &base_stat) != 0 || base_stat.st_size < 0) {
        std::fprintf(stderr, "AOF rewrite error: could not stat new base\n");
        return false;
    }
    const uint64_t size = static_cast<uint64_t>(base_stat.st_size);
    const uint64_t rewrite_baseline = size + file_offset_;
    std::vector<std::pair<uint64_t, std::string>> active{
        {active_incr_sequence_, increments_.back().second}};
    std::vector<std::vector<uint32_t>> active_starts{increment_starts_.back()};
    std::string error;
    maybe_pause_rewrite(AofRewriteDebugStage::BeforeManifest);
    if (!persist_manifest(rewrite_base_name_, rewrite_target_sequence_, snapshot_epoch,
                          base_commit_, size, rewrite_baseline,
                          active, active_starts, error)) {
        std::fprintf(stderr, "AOF rewrite error: %s\n", error.c_str());
        return false;
    }
    maybe_pause_rewrite(AofRewriteDebugStage::AfterManifest);

    uint64_t removed = 0;
    for (const std::string& name : rewrite_history_) {
        if (name == rewrite_base_name_ || name == active.front().second) continue;
        const std::string path = directory_path_ + "/" + name;
        if (::unlink(path.c_str()) == 0 || errno == ENOENT) removed++;
    }
    const int directory_fd = ::open(directory_path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        (void)::fsync(directory_fd);
        (void)::close(directory_fd);
    }
    base_name_ = rewrite_base_name_;
    base_sequence_ = rewrite_target_sequence_;
    base_size_.store(size, std::memory_order_relaxed);
    rewrite_base_size_.store(rewrite_baseline, std::memory_order_relaxed);
    increments_ = std::move(active);
    increment_starts_ = std::move(active_starts);
    rewrite_history_.clear();
    history_unlinks_.fetch_add(removed, std::memory_order_relaxed);
    rewrite_completions_.fetch_add(1, std::memory_order_relaxed);
    consecutive_rewrite_failures_.store(0, std::memory_order_relaxed);
    next_rewrite_retry_ms_.store(0, std::memory_order_relaxed);
    backoff_reported_ = false;
    last_rewrite_ok_.store(true, std::memory_order_relaxed);
    rewrite_in_progress_.store(false, std::memory_order_release);
    struct stat increment_stat{};
    const uint64_t increment_size = ::fstat(fd_, &increment_stat) == 0 && increment_stat.st_size >= 0
        ? static_cast<uint64_t>(increment_stat.st_size) : file_offset_;
    current_size_.store(size + increment_size, std::memory_order_relaxed);
    return true;
}

void AofManager::rewrite_abort() {
    if (!rewrite_in_progress_.exchange(false, std::memory_order_acq_rel)) return;
    last_rewrite_ok_.store(false, std::memory_order_relaxed);
    rewrite_failures_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t failures =
        consecutive_rewrite_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures >= 3) {
        const uint32_t shift = std::min<uint32_t>(failures - 3, 6);
        const uint32_t minutes = std::min<uint32_t>(1u << shift, 60u);
        next_rewrite_retry_ms_.store(monotonic_ms() + static_cast<int64_t>(minutes) * 60000,
                                     std::memory_order_relaxed);
        backoff_reported_ = false;
    }
    rewrite_history_.clear();
}

bool AofManager::post_chunk(uint32_t producer, std::unique_ptr<AofChunk>& chunk,
                            Ring& producer_ring, LoopSignals& signals) {
    if (!recording() || !chunk || producer >= nthreads_) return false;
    if (!chunk->post_sequence)
        chunk->post_sequence = posted_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    AofChunk* raw = chunk.get();
    if (!chunk_in_[producer].push(raw, signals)) return false;
    chunk.release();
    pending_chunks_.fetch_add(1, std::memory_order_release);
    if (chunk_notify_.set(producer)) {
        Ring* target = writer_ring_.load(std::memory_order_acquire);
        chunk_in_[producer].wake(producer_ring, signals, target);
    }
    return true;
}

bool AofManager::reply_gate_ready(uint64_t target) const {
    if (!recording() || target == 0 || fsync_policy() == AppendFsyncPolicy::No) return true;
    const uint64_t frontier = fsync_policy() == AppendFsyncPolicy::Always
        ? durable_sequence_.load(std::memory_order_acquire)
        : written_sequence_.load(std::memory_order_acquire);
    return frontier >= target;
}

void AofManager::register_send_gate_wait(uint32_t tid) {
    if (tid >= nthreads_) return;
    if (gate_waiters_.set(tid)) send_gate_waits_.fetch_add(1, std::memory_order_relaxed);
}

bool AofManager::mark_post_written(uint64_t sequence) {
    if (!sequence) return false;
    uint64_t frontier = written_sequence_.load(std::memory_order_relaxed);
    if (sequence <= frontier) return false;
    if (sequence != frontier + 1) {
        try { return written_out_of_order_.insert(sequence).second; }
        catch (const std::bad_alloc&) { return false; }
    }
    frontier = sequence;
    while (written_out_of_order_.erase(frontier + 1)) frontier++;
    written_sequence_.store(frontier, std::memory_order_release);
    return true;
}

bool AofManager::mark_post_submitted(uint64_t sequence) {
    if (!sequence) return false;
    uint64_t frontier = submitted_sequence_.load(std::memory_order_relaxed);
    if (sequence <= frontier) return false;
    if (sequence != frontier + 1) {
        try { return submitted_out_of_order_.insert(sequence).second; }
        catch (const std::bad_alloc&) { return false; }
    }
    frontier = sequence;
    while (submitted_out_of_order_.erase(frontier + 1)) frontier++;
    submitted_sequence_.store(frontier, std::memory_order_release);
    return true;
}

bool AofManager::mark_post_durable(uint64_t sequence) {
    if (!sequence) return false;
    uint64_t frontier = durable_sequence_.load(std::memory_order_relaxed);
    if (sequence <= frontier) return true;
    if (sequence != frontier + 1) {
        try { return durable_out_of_order_.insert(sequence).second; }
        catch (const std::bad_alloc&) { return false; }
    }
    frontier = sequence;
    while (durable_out_of_order_.erase(frontier + 1)) frontier++;
    durable_sequence_.store(frontier, std::memory_order_release);
    return true;
}

void AofManager::wake_gate_waiters(ThreadCtx& writer, Ring& ring) {
    if (!server_) return;
    for (uint32_t word = 0; word < NotifyMask::kWords; word++) {
        uint64_t bits = gate_waiters_.take(word);
        while (bits) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t tid = word * 64 + bit;
            if (tid >= nthreads_ || server_->thread(tid).role() != Role::Ifid) continue;
            Ring* target = server_->thread(tid).ring();
            if (!target || !ring.msg_to(*target, ur_tag(UrKind::Wake, nullptr))) {
                gate_waiters_.set(tid);
                continue;
            }
            writer.sig().wakes_sent++;
        }
    }
}

uint32_t AofManager::maybe_sync(ThreadCtx& writer, Ring& ring, io_uring_sqe* last_write) {
    const AppendFsyncPolicy policy = fsync_policy();
    AofIoRequest* write = static_cast<AofIoRequest*>(current_uring_write_);
    if (write) {
        try {
            write->batch_vectors.reserve(write->chunks.size() * 2);
            for (size_t index = 0; index < write->chunks.size(); index++) {
                write->batch_vectors.push_back(
                    {write->frame_headers[index].data(), kFrameHeaderBytes});
                AofChunk& chunk = *write->chunks[index];
                write->batch_vectors.push_back({chunk.bytes.data(), chunk.bytes.size()});
            }
        } catch (const std::bad_alloc&) {
            for (size_t index = 0; index < write->chunks.size(); index++)
                pending_chunks_.fetch_sub(1, std::memory_order_release);
            delete write;
            current_uring_write_ = nullptr;
            fail("out of memory submitting AOF write batch");
            return 0;
        }
        write->vector_count = static_cast<uint32_t>(write->batch_vectors.size());
        ring.ensure_sq_space(2);
        last_write = queue_aof_write(ring, *write);
        if (!last_write) {
            failed_write_offset_ = std::min(failed_write_offset_, write->safe_offset);
            for (size_t index = 0; index < write->chunks.size(); index++)
                pending_chunks_.fetch_sub(1, std::memory_order_release);
            delete write;
            current_uring_write_ = nullptr;
            if (io_inflight_ == 0) {
                const int truncate_result =
                    ::ftruncate(fd_, static_cast<off_t>(failed_write_offset_));
                (void)truncate_result;
                file_offset_ = failed_write_offset_;
                failed_write_offset_ = UINT64_MAX;
            }
            fail("AOF frame ordering or write failed");
            return 0;
        }
        io_inflight_++;
        current_uring_write_ = nullptr;
    }
    const bool always_batch = policy == AppendFsyncPolicy::Always && write;
    if ((policy == AppendFsyncPolicy::No && !always_batch) || fd_ < 0) return 0;
    const uint64_t target = engine_ == PersistIoEngine::Normal ||
                            (policy == AppendFsyncPolicy::Everysec && !last_write)
        ? written_sequence_.load(std::memory_order_acquire)
        : submitted_sequence_.load(std::memory_order_acquire);
    const uint64_t durable = durable_sequence_.load(std::memory_order_acquire);
    if (target <= durable && !always_batch) return 0;
    const int64_t now = monotonic_ms();
    if (policy == AppendFsyncPolicy::Everysec && now - last_fsync_ms_ < 1000) return 0;

    if (engine_ == PersistIoEngine::Normal) {
        if (::fdatasync(fd_) != 0) {
            fail("AOF data-sync failed");
            wake_gate_waiters(writer, ring);
            return 1;
        }
        durable_sequence_.store(target, std::memory_order_release);
        fsyncs_.fetch_add(1, std::memory_order_relaxed);
        last_fsync_ms_ = now;
        wake_gate_waiters(writer, ring);
        return 1;
    }

    if (policy == AppendFsyncPolicy::Everysec && everysec_fsync_inflight_) {
        return 0;
    }
    if (policy == AppendFsyncPolicy::Always && !always_batch &&
        fsync_inflight_ && !short_sync_needed_) return 0;
    auto* request = new (std::nothrow) AofIoRequest();
    if (!request) { fail("out of memory submitting AOF data-sync"); return 0; }
    request->role = AofIoSync;
    request->fd = fd_;
    if (always_batch) {
        try {
            request->sync_sequences.reserve(write->chunks.size());
            for (const auto& chunk : write->chunks)
                request->sync_sequences.push_back(chunk->post_sequence);
        } catch (const std::bad_alloc&) {
            delete request;
            fail("out of memory retaining AOF sync batch");
            return 0;
        }
    } else {
        request->sync_target = target;
        if (policy == AppendFsyncPolicy::Always) short_sync_needed_ = false;
    }
    request->everysec = policy == AppendFsyncPolicy::Everysec;
    if (last_write) last_write->flags |= IOSQE_IO_LINK;
    io_uring_sqe* sqe = queue_aof_sync(ring, *request, true);
    if (!sqe) {
        if (last_write) last_write->flags &= ~IOSQE_IO_LINK;
        delete request;
        fail("AOF data-sync submission failed");
        return 0;
    }
    if (always_batch) write->batch_sync = request;
    io_inflight_++;
    fsync_inflight_++;
    if (request->everysec) everysec_fsync_inflight_ = true;
    last_fsync_ms_ = now;
    return 1;
}

void AofManager::on_io_complete(ThreadCtx& writer, Ring& ring, void* opaque, int result) {
    std::unique_ptr<AofIoRequest> request(static_cast<AofIoRequest*>(opaque));
    if (!request) return;
    auto truncate_failed_write_if_idle = [&]() {
        if (failed_write_offset_ == UINT64_MAX || io_inflight_ != 0) return;
        const int truncate_result =
            ::ftruncate(fd_, static_cast<off_t>(failed_write_offset_));
        (void)truncate_result;
        file_offset_ = failed_write_offset_;
        failed_write_offset_ = UINT64_MAX;
    };
    if (request->role == AofIoWrite) {
        AofIoRequest* batch_sync = request->batch_sync;
        request->batch_sync = nullptr;
        if (result <= 0 || static_cast<size_t>(result) > request->remaining) {
            if (io_inflight_) io_inflight_--;
            if (request->waiter) {
                request->waiter->result = result < 0 ? result : -EIO;
                request->waiter->done = true;
            } else if (!request->chunks.empty()) {
                failed_write_offset_ = std::min(failed_write_offset_, request->safe_offset);
                pending_chunks_.fetch_sub(request->chunks.size(), std::memory_order_release);
                fail(request->contains_group_commit ? "AOF GCMT write failed"
                                                    : "AOF frame ordering or write failed");
                wake_gate_waiters(writer, ring);
            }
            truncate_failed_write_if_idle();
            return;
        }

        request->remaining -= static_cast<size_t>(result);
        request->offset += static_cast<uint64_t>(result);
        consume_aof_iovecs(*request, static_cast<size_t>(result));
        if (request->remaining) {
            if (batch_sync) batch_sync->had_short = true;
            AofIoRequest* retry = request.release();
            retry->had_short = true;
            io_uring_sqe* write_sqe = queue_aof_write(ring, *retry);
            if (!write_sqe) {
                request.reset(retry);
                if (io_inflight_) io_inflight_--;
                if (retry->waiter) {
                    retry->waiter->result = -EIO;
                    retry->waiter->done = true;
                } else {
                    failed_write_offset_ = std::min(failed_write_offset_, retry->safe_offset);
                    pending_chunks_.fetch_sub(retry->chunks.size(), std::memory_order_release);
                    fail("AOF frame ordering or write failed");
                }
                truncate_failed_write_if_idle();
                return;
            }
            return;
        }

        if (io_inflight_) io_inflight_--;
        truncate_failed_write_if_idle();
        if (request->waiter) {
            request->waiter->result = 0;
            request->waiter->done = true;
            return;
        }
        if (request->chunks.empty()) return;
        for (const auto& chunk : request->chunks) {
            if (!chunk->group_commit) note_group_fragment(*chunk);
            if (!mark_post_written(chunk->post_sequence)) {
                fail("AOF post frontier did not advance");
                break;
            }
            records_written_.fetch_add(chunk->group_commit ? 1 : chunk->records,
                                       std::memory_order_relaxed);
            if (chunk->group_commit)
                groups_committed_.fetch_add(1, std::memory_order_relaxed);
        }
        current_size_.store(base_size() + file_offset_, std::memory_order_relaxed);
        pending_chunks_.fetch_sub(request->chunks.size(), std::memory_order_release);
        if (request->had_short) short_sync_needed_ = true;
        wake_gate_waiters(writer, ring);
        return;
    }

    if (io_inflight_) io_inflight_--;
    if (fsync_inflight_) fsync_inflight_--;
    if (request->everysec) everysec_fsync_inflight_ = false;
    truncate_failed_write_if_idle();
    if (request->waiter) {
        request->waiter->result = result < 0 ? result : 0;
        request->waiter->done = true;
        return;
    }
    if (result < 0) {
        fail("AOF data-sync failed");
        wake_gate_waiters(writer, ring);
        return;
    }
    fsyncs_.fetch_add(1, std::memory_order_relaxed);
    if (!request->sync_sequences.empty()) {
        if (request->had_short) {
            short_sync_needed_ = true;
        } else {
            for (uint64_t sequence : request->sync_sequences) {
                if (!mark_post_durable(sequence)) {
                    fail("AOF durable frontier did not advance");
                    break;
                }
            }
        }
    }
    if (request->sync_target) {
        const uint64_t written = written_sequence_.load(std::memory_order_acquire);
        if (written >= request->sync_target) {
            uint64_t durable = durable_sequence_.load(std::memory_order_relaxed);
            if (request->sync_target > durable) durable = request->sync_target;
            for (auto it = durable_out_of_order_.begin(); it != durable_out_of_order_.end();) {
                if (*it <= durable) it = durable_out_of_order_.erase(it);
                else ++it;
            }
            while (durable_out_of_order_.erase(durable + 1)) durable++;
            durable_sequence_.store(durable, std::memory_order_release);
        } else {
            // A positive short write lets the linked sync run after only the prefix. The remainder
            // is already resubmitted; force an immediate conservative sync after its CQE.
            last_fsync_ms_ = 0;
        }
    }
    wake_gate_waiters(writer, ring);
}

uint32_t AofManager::pump_io_completions(ThreadCtx& writer, Ring& ring) {
    return ring.for_each_cqe_filtered([&](io_uring_cqe* cqe) {
        if (ur_kind(cqe->user_data) != UrKind::AofIo) return false;
        on_io_complete(writer, ring, ur_ptr<void>(cqe->user_data), cqe->res);
        return true;
    });
}

bool AofManager::write_frame_normal(const AofChunk& chunk) {
    if (chunk.sid < 0 || static_cast<uint32_t>(chunk.sid) >= nshards_) return false;
    const uint32_t sid = static_cast<uint32_t>(chunk.sid);
    if (chunk.sequence != next_sequence_[sid]++) return false;
    uint8_t header[kFrameHeaderBytes] = {};
    snapshot_put_u32(header, kFrameTag);
    snapshot_put_u32(header + 4, sid);
    snapshot_put_u32(header + 8, chunk.sequence);
    snapshot_put_u32(header + 12, chunk.flags);
    snapshot_put_u32(header + 16, static_cast<uint32_t>(chunk.bytes.size()));
    snapshot_put_u32(header + 20, kFrameHeaderBytes);
    snapshot_put_u64(header + 24, snapshot_checksum(chunk.bytes.data(), chunk.bytes.size()));
    snapshot_put_u64(header + 32, snapshot_checksum(header, 32));
    const uint64_t frame_begin = file_offset_;
    if (!write_frame_counted(fd_, header, sizeof(header),
                             chunk.bytes.data(), chunk.bytes.size(), file_offset_)) {
        const int truncate_result = ::ftruncate(fd_, static_cast<off_t>(last_good_offset_));
        (void)truncate_result;
        file_offset_ = last_good_offset_;
        return false;
    }
    if (chunk.flags & AofFrameLargeBegin) large_record_offset_ = frame_begin;
    if (chunk.flags & AofFrameLargeEnd) last_good_offset_ = file_offset_;
    else if (locked_producer_ == UINT32_MAX) last_good_offset_ = file_offset_;
    records_written_.fetch_add(chunk.records, std::memory_order_relaxed);
    current_size_.store(base_size() + file_offset_, std::memory_order_relaxed);
    return true;
}

void AofManager::note_group_fragment(const AofChunk& chunk) {
    if (!chunk.group || !chunk.group_fragment_last || chunk.sid < 0) return;
    AofGroupDecision& group = *chunk.group;
    for (size_t index = 0; index < group.participants.size(); index++) {
        if (group.participants[index] != static_cast<uint32_t>(chunk.sid)) continue;
        group.dependencies[index].sequence = chunk.sequence;
        group.dependency_seen[index] = 1;
        uint64_t remaining = debug_stop_after_fragments_.load(std::memory_order_acquire);
        while (remaining && !debug_stop_after_fragments_.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_acq_rel,
                   std::memory_order_acquire)) {}
        if (remaining == 1) {
            ::raise(SIGKILL);
            ::_exit(86);
        }
        return;
    }
    fail("AOF group fragment names a non-participant shard");
}

bool AofManager::group_dependencies_ready(const AofGroupDecision& group) const {
    if (group.dependencies.size() != group.participants.size() ||
        group.dependency_seen.size() != group.participants.size()) return false;
    for (uint8_t seen : group.dependency_seen) if (!seen) return false;
    return true;
}

bool AofManager::prepare_group_commit(AofChunk& chunk, uint8_t* header) {
    if (!chunk.group || !group_dependencies_ready(*chunk.group)) return false;
    const uint64_t ticket = chunk.group->ticket.load(std::memory_order_acquire);
    if (!ticket || chunk.group->aborted.load(std::memory_order_acquire)) return false;
    const uint64_t payload_len = 4 + chunk.group->dependencies.size() * 8;
    try { chunk.bytes.assign(kRecordHeaderBytes + payload_len, 0); }
    catch (const std::bad_alloc&) { return false; }
    uint8_t* record = chunk.bytes.data();
    snapshot_put_u32(record, kRecordTag);
    record[4] = static_cast<uint8_t>(AofRecordKind::GroupCommit);
    record[7] = 1;
    snapshot_put_u32(record + 12, kRecordHeaderBytes);
    snapshot_put_u64(record + 16, payload_len);
    snapshot_put_u64(record + 24, static_cast<uint64_t>(-1));
    snapshot_put_u64(record + 32, ticket);
    snapshot_put_u32(record + kRecordHeaderBytes,
                     static_cast<uint32_t>(chunk.group->dependencies.size()));
    size_t pos = kRecordHeaderBytes + 4;
    for (const AofGroupDependency& dependency : chunk.group->dependencies) {
        snapshot_put_u32(record + pos, dependency.sid);
        snapshot_put_u32(record + pos + 4, dependency.sequence);
        pos += 8;
    }

    snapshot_put_u32(header, kFrameTag);
    snapshot_put_u32(header + 4, UINT32_MAX);  // physical control stream
    snapshot_put_u32(header + 8, 0);           // control frames need no logical shard sequence
    snapshot_put_u32(header + 16, static_cast<uint32_t>(chunk.bytes.size()));
    snapshot_put_u32(header + 20, kFrameHeaderBytes);
    snapshot_put_u64(header + 24, snapshot_checksum(chunk.bytes.data(), chunk.bytes.size()));
    snapshot_put_u64(header + 32, snapshot_checksum(header, 32));
    return true;
}

bool AofManager::write_group_commit_normal(AofChunk& chunk) {
    uint8_t header[kFrameHeaderBytes] = {};
    if (!prepare_group_commit(chunk, header)) return false;
    if (!write_frame_counted(fd_, header, sizeof(header),
                             chunk.bytes.data(), chunk.bytes.size(), file_offset_)) {
        const int truncate_result = ::ftruncate(fd_, static_cast<off_t>(last_good_offset_));
        (void)truncate_result;
        file_offset_ = last_good_offset_;
        return false;
    }
    last_good_offset_ = file_offset_;
    current_size_.store(base_size() + file_offset_, std::memory_order_relaxed);
    records_written_.fetch_add(1, std::memory_order_relaxed);
    groups_committed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool AofManager::submit_frame_uring(std::unique_ptr<AofChunk> chunk, bool group_commit,
                                    Ring& ring, io_uring_sqe*& last_write) {
    (void)ring;
    (void)last_write;
    auto* request = static_cast<AofIoRequest*>(current_uring_write_);
    if (!request) {
        request = new (std::nothrow) AofIoRequest();
        if (!request) return false;
        request->fd = fd_;
        request->offset = file_offset_;
        request->safe_offset = last_good_offset_;
        try {
            request->frame_headers.reserve(256);
            request->chunks.reserve(256);
        } catch (const std::bad_alloc&) {
            delete request;
            return false;
        }
        current_uring_write_ = request;
    }
    std::array<uint8_t, kFrameHeaderBytes> header{};
    if (group_commit) {
        if (!prepare_group_commit(*chunk, header.data())) return false;
    } else {
        if (chunk->sid < 0 || static_cast<uint32_t>(chunk->sid) >= nshards_) {
            return false;
        }
        const uint32_t sid = static_cast<uint32_t>(chunk->sid);
        if (chunk->sequence != next_sequence_[sid]++) return false;
        snapshot_put_u32(header.data(), kFrameTag);
        snapshot_put_u32(header.data() + 4, sid);
        snapshot_put_u32(header.data() + 8, chunk->sequence);
        snapshot_put_u32(header.data() + 12, chunk->flags);
        snapshot_put_u32(header.data() + 16, static_cast<uint32_t>(chunk->bytes.size()));
        snapshot_put_u32(header.data() + 20, kFrameHeaderBytes);
        snapshot_put_u64(header.data() + 24,
                         snapshot_checksum(chunk->bytes.data(), chunk->bytes.size()));
        snapshot_put_u64(header.data() + 32, snapshot_checksum(header.data(), 32));
    }

    const uint32_t flags = chunk->flags;
    const uint64_t post_sequence = chunk->post_sequence;
    const size_t frame_bytes = kFrameHeaderBytes + chunk->bytes.size();
    try {
        request->frame_headers.push_back(header);
        request->chunks.push_back(std::move(chunk));
    } catch (const std::bad_alloc&) {
        if (request->frame_headers.size() > request->chunks.size())
            request->frame_headers.pop_back();
        return false;
    }
    request->remaining += frame_bytes;
    request->contains_group_commit |= group_commit;

    const uint64_t frame_begin = file_offset_;
    file_offset_ += frame_bytes;
    if (flags & AofFrameLargeBegin) large_record_offset_ = frame_begin;
    if (flags & AofFrameLargeEnd) last_good_offset_ = file_offset_;
    else if (locked_producer_ == UINT32_MAX) last_good_offset_ = file_offset_;
    if (group_commit) last_good_offset_ = file_offset_;
    if (!mark_post_submitted(post_sequence)) {
        fail("AOF submitted frontier did not advance");
        return true;
    }
    return true;
}

uint32_t AofManager::drain_pending_commits(uint32_t& budget, Ring& ring,
                                           io_uring_sqe*& last_write) {
    // A control frame must never land between the begin and end frames of a large record. Every
    // recovery path -- the writer's own short-write rollback to last_good_offset_, the shutdown
    // ftruncate to large_record_offset_, and the loader's rewind to the large record's first frame
    // -- discards the file from the large record's first byte onward. Anything written inside that
    // byte range is discarded with it, and a GCMT is exactly the record that must not be
    // discardable: it is what makes an already-durable, already-acknowledged group visible on
    // replay. So the lock a large record takes on the physical stream covers control frames too.
    // The deferral is bounded: the writer keeps draining the locked producer, which is the only
    // producer that can close the record.
    if (locked_producer_ != UINT32_MAX) {
        if (!pending_commits_.empty()) {
            for (const std::unique_ptr<AofChunk>& held : pending_commits_) {
                if (held->group && group_dependencies_ready(*held->group)) {
                    control_defers_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        return 0;
    }
    uint32_t written = 0;
    for (size_t index = 0; index < pending_commits_.size() && budget;) {
        AofChunk& chunk = *pending_commits_[index];
        if (!group_dependencies_ready(*chunk.group)) { index++; continue; }
        const bool ok = engine_ == PersistIoEngine::Normal
            ? write_group_commit_normal(chunk)
            : submit_frame_uring(std::move(pending_commits_[index]), true, ring, last_write);
        if (!ok) {
            fail("AOF GCMT write failed");
            return written;
        }
        if (engine_ == PersistIoEngine::Normal) {
            if (!mark_post_written(chunk.post_sequence)) {
                fail("AOF post frontier did not advance");
                return written;
            }
            pending_chunks_.fetch_sub(1, std::memory_order_release);
        }
        pending_commits_.erase(pending_commits_.begin() + index);
        budget--;
        written++;
    }
    return written;
}

bool AofManager::drain_producer(uint32_t producer, uint32_t& budget, uint32_t& consumed,
                                Ring& ring, io_uring_sqe*& last_write) {
    AofChunk* chunk = nullptr;
    while (budget && chunk_in_[producer].recv(chunk)) {
        if (chunk->group_commit) {
            try { pending_commits_.emplace_back(chunk); }
            catch (const std::bad_alloc&) {
                delete chunk;
                chunk_in_[producer].retire();
                pending_chunks_.fetch_sub(1, std::memory_order_release);
                fail("out of memory retaining AOF GCMT");
                return false;
            }
            chunk_in_[producer].retire();
            budget--;
            consumed++;
            continue;
        }
        const uint32_t flags = chunk->flags;
        bool valid = true;
        if (locked_producer_ == UINT32_MAX) {
            if (flags & AofFrameLargeEnd) valid = false;
            if (flags & AofFrameLargeBegin) locked_producer_ = producer;
        } else {
            if (locked_producer_ != producer || (flags & AofFrameLargeBegin)) valid = false;
        }
        if (valid && engine_ == PersistIoEngine::Normal) valid = write_frame_normal(*chunk);
        if (valid && engine_ == PersistIoEngine::Normal) note_group_fragment(*chunk);
        if (valid && engine_ == PersistIoEngine::Normal)
            valid = mark_post_written(chunk->post_sequence);
        if (valid && engine_ == PersistIoEngine::Uring) {
            std::unique_ptr<AofChunk> owned(chunk);
            valid = submit_frame_uring(std::move(owned), false, ring, last_write);
            chunk = nullptr;
        }
        if (valid && (flags & AofFrameLargeEnd)) locked_producer_ = UINT32_MAX;
        if (engine_ == PersistIoEngine::Normal) delete chunk;
        chunk_in_[producer].retire();
        if (engine_ == PersistIoEngine::Normal || !valid)
            pending_chunks_.fetch_sub(1, std::memory_order_release);
        budget--;
        consumed++;
        if (!valid) {
            fail("AOF frame ordering or write failed");
            return false;
        }
    }
    if (chunk_in_[producer].depth()) chunk_notify_.set(producer);
    return true;
}

uint32_t AofManager::writer_pass(ThreadCtx& writer, Ring& ring, bool drain_all) {
    if (!configured_ || writer.id() != writer_tid_ || fd_ < 0 || failed()) return 0;
    if (engine_ == PersistIoEngine::Uring &&
        fsync_policy() == AppendFsyncPolicy::Always &&
        fsync_inflight_ >= kAofAlwaysMaxChains) return 0;
    if (engine_ == PersistIoEngine::Uring &&
        fsync_policy() == AppendFsyncPolicy::Everysec && io_inflight_ &&
        monotonic_ms() - last_fsync_ms_ >= 1000) return 0;
    maybe_schedule_auto_rewrite();
    maybe_start_rewrite(writer, ring);
    const uint64_t written_before = written_sequence_.load(std::memory_order_relaxed);
    uint32_t budget = drain_all || engine_ == PersistIoEngine::Uring
        ? 256 : kWriterFramesPerPass;
    uint32_t consumed = 0;
    io_uring_sqe* last_write = nullptr;
    consumed += drain_pending_commits(budget, ring, last_write);
    if (locked_producer_ != UINT32_MAX) {
        drain_producer(locked_producer_, budget, consumed, ring, last_write);
        if (written_sequence_.load(std::memory_order_relaxed) != written_before)
            wake_gate_waiters(writer, ring);
        return consumed + maybe_sync(writer, ring, last_write);
    }
    for (uint32_t word = 0; word < NotifyMask::kWords && budget; word++) {
        uint64_t bits = chunk_notify_.take(word);
        while (bits && budget && locked_producer_ == UINT32_MAX) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t producer = word * 64 + bit;
            if (producer < nthreads_)
                drain_producer(producer, budget, consumed, ring, last_write);
        }
        while (bits) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            chunk_notify_.set(word * 64 + bit);
        }
    }
    if (drain_all && consumed == 0 && locked_producer_ == UINT32_MAX) {
        for (uint32_t visited = 0; visited < nthreads_ && budget; visited++) {
            const uint32_t producer = writer_cursor_++ % nthreads_;
            drain_producer(producer, budget, consumed, ring, last_write);
            if (locked_producer_ != UINT32_MAX) break;
        }
    }
    if (budget) consumed += drain_pending_commits(budget, ring, last_write);
    if (written_sequence_.load(std::memory_order_relaxed) != written_before)
        wake_gate_waiters(writer, ring);
    return consumed + maybe_sync(writer, ring, last_write);
}

bool AofManager::wait_until_drained(uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (pending_chunks() && std::chrono::steady_clock::now() < deadline) {
        if (Ring* ring = writer_ring_.load(std::memory_order_acquire)) {
            (void)ring;
        }
        std::this_thread::yield();
    }
    return pending_chunks() == 0;
}

void AofManager::writer_shutdown(ThreadCtx& writer, Ring& ring) {
    if (!configured_ || writer.id() != writer_tid_ || fd_ < 0) return;
    for (uint32_t pass = 0; pending_chunks() && pass < 100000; pass++) {
        const uint32_t n = writer_pass(writer, ring, true);
        if (engine_ == PersistIoEngine::Uring) {
            ring.submit_and_reap();
            pump_io_completions(writer, ring);
        }
        if (!n) std::this_thread::yield();
    }
    while (engine_ == PersistIoEngine::Uring && io_inflight_) {
        ring.submit_and_wait(1);
        pump_io_completions(writer, ring);
    }
    if (locked_producer_ != UINT32_MAX) {
        const int truncate_result = ::ftruncate(fd_, static_cast<off_t>(large_record_offset_));
        (void)truncate_result;
        file_offset_ = large_record_offset_;
    }
    if (engine_ == PersistIoEngine::Normal) (void)::fdatasync(fd_);
    else (void)wait_control_sync(writer, ring, fd_);
    (void)::close(fd_);
    fd_ = -1;
    recording_.store(false, std::memory_order_release);
    writer_ring_.store(nullptr, std::memory_order_release);
}

void AofManager::discard_chunks() {
    delete static_cast<AofIoRequest*>(current_uring_write_);
    current_uring_write_ = nullptr;
    if (!chunk_in_) return;
    for (uint32_t producer = 0; producer < nthreads_; producer++) {
        AofChunk* chunk = nullptr;
        while (chunk_in_[producer].recv(chunk)) {
            delete chunk;
            chunk_in_[producer].retire();
        }
    }
}

std::unique_ptr<AofReplayPlan> aof_read_plan(const char* path, uint32_t expected_shards,
                                             bool truncate_tail, bool& exists,
                                             std::string& warning, std::string& error,
                                             const std::vector<uint32_t>* initial_sequences) {
    exists = false;
    warning.clear();
    error.clear();
    const int fd = ::open(path, truncate_tail ? O_RDWR | O_CLOEXEC : O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return nullptr;
        error = "could not open AOF";
        return nullptr;
    }
    exists = true;
    std::vector<uint8_t> file;
    if (!read_file(fd, file, error)) { ::close(fd); return nullptr; }
    if (file.size() < kFileHeaderBytes ||
        std::memcmp(file.data(), kFileMagic, sizeof(kFileMagic)) != 0 ||
        snapshot_get_u32(file.data() + 8) != kFileVersion ||
        snapshot_get_u32(file.data() + 12) != kFileHeaderBytes ||
        snapshot_get_u64(file.data() + 64) != snapshot_checksum(file.data(), 64)) {
        error = "invalid AOF header";
        ::close(fd);
        return nullptr;
    }
    auto plan = std::make_unique<AofReplayPlan>();
    plan->shard_count = snapshot_get_u32(file.data() + 16);
    if (plan->shard_count != expected_shards) {
        error = "AOF shard count does not match --shards";
        ::close(fd);
        return nullptr;
    }
    plan->hash_kind = snapshot_get_u32(file.data() + 20);
    if (plan->hash_kind > static_cast<uint32_t>(HashKind::SipHash12)) {
        error = "AOF uses an unknown hash kind";
        ::close(fd);
        return nullptr;
    }
    plan->hash_seed = snapshot_get_u64(file.data() + 32);
    plan->sip_k0 = snapshot_get_u64(file.data() + 40);
    plan->sip_k1 = snapshot_get_u64(file.data() + 48);
    plan->sections.resize(plan->shard_count);
    if (initial_sequences && initial_sequences->size() == plan->shard_count)
        plan->next_sequence = *initial_sequences;
    else
        plan->next_sequence.assign(plan->shard_count, 0);

    size_t pos = kFileHeaderBytes;
    size_t valid = kFileHeaderBytes;
    bool active_large = false;
    uint32_t large_sid = UINT32_MAX;
    size_t large_file_pos = 0;
    size_t large_section_size = 0;
    uint32_t large_sequence = 0;
    bool torn = false;
    while (pos < file.size()) {
        if (file.size() - pos < kFrameHeaderBytes) { torn = true; break; }
        const uint8_t* h = file.data() + pos;
        if (snapshot_get_u32(h) != kFrameTag ||
            snapshot_get_u32(h + 20) != kFrameHeaderBytes ||
            snapshot_get_u64(h + 32) != snapshot_checksum(h, 32)) {
            error = "invalid AOF frame header";
            ::close(fd);
            return nullptr;
        }
        const uint32_t sid = snapshot_get_u32(h + 4);
        const bool control = sid == UINT32_MAX;
        const uint32_t sequence = snapshot_get_u32(h + 8);
        const uint32_t flags = snapshot_get_u32(h + 12);
        const uint32_t length = snapshot_get_u32(h + 16);
        const uint64_t checksum = snapshot_get_u64(h + 24);
        const size_t frame_pos = pos;
        pos += kFrameHeaderBytes;
        if ((!control && (sid >= plan->shard_count || sequence != plan->next_sequence[sid]++)) ||
            (control && (sequence != 0 || flags != 0))) {
            error = "invalid AOF shard frame sequence";
            ::close(fd);
            return nullptr;
        }
        if (file.size() - pos < length) {
            if (!control) plan->next_sequence[sid]--;
            torn = true;
            pos = frame_pos;
            break;
        }
        if (checksum != snapshot_checksum(file.data() + pos, length)) {
            error = "invalid AOF frame checksum";
            ::close(fd);
            return nullptr;
        }
        if (control && active_large) {
            error = "AOF control record interleaves a large record";
            ::close(fd);
            return nullptr;
        }
        if (!active_large) {
            if (flags & AofFrameLargeEnd) {
                error = "AOF large record ends without a begin";
                ::close(fd);
                return nullptr;
            }
            if (flags & AofFrameLargeBegin) {
                if (control) {
                    error = "AOF control record cannot be large";
                    ::close(fd);
                    return nullptr;
                }
                active_large = true;
                large_sid = sid;
                large_file_pos = frame_pos;
                large_section_size = plan->sections[sid].size();
                large_sequence = sequence;
            }
        } else if (sid != large_sid || (flags & AofFrameLargeBegin)) {
            error = "interleaved AOF large record";
            ::close(fd);
            return nullptr;
        }
        try {
            std::vector<uint8_t>& section = control ? plan->control_section : plan->sections[sid];
            section.insert(section.end(), file.data() + pos, file.data() + pos + length);
        } catch (const std::bad_alloc&) {
            error = "out of memory assembling AOF shard streams";
            ::close(fd);
            return nullptr;
        }
        pos += length;
        if (active_large && (flags & AofFrameLargeEnd)) {
            active_large = false;
            large_sid = UINT32_MAX;
        }
        if (!active_large) valid = pos;
    }
    if (active_large) {
        plan->sections[large_sid].resize(large_section_size);
        plan->next_sequence[large_sid] = large_sequence;
        valid = large_file_pos;
        torn = true;
    }
    if (torn) {
        if (!truncate_tail) {
            error = "truncated AOF tail";
            ::close(fd);
            return nullptr;
        }
        if (::ftruncate(fd, static_cast<off_t>(valid)) != 0) {
            error = "could not truncate torn AOF tail";
            ::close(fd);
            return nullptr;
        }
        warning = "truncated AOF tail from " + std::to_string(file.size()) + " to " +
                  std::to_string(valid) + " bytes";
    }
    ::close(fd);

    size_t control_pos = 0;
    while (control_pos < plan->control_section.size()) {
        AofRecordKind kind;
        uint8_t type = 0, encoding = 0;
        uint32_t key_len = 0;
        uint64_t payload_len = 0, group = 0;
        int64_t expire = -1;
        size_t next = 0;
        if (!parse_record_bounds(plan->control_section, control_pos, kind, type, encoding,
                                 key_len, payload_len, expire, group, next, error)) return nullptr;
        const size_t payload_pos = control_pos + kRecordHeaderBytes + key_len;
        if (kind != AofRecordKind::GroupCommit || key_len != 0 || !group || payload_len < 4) {
            error = "invalid AOF GCMT record";
            return nullptr;
        }
        const uint32_t count = snapshot_get_u32(plan->control_section.data() + payload_pos);
        if (!count || payload_len != 4 + static_cast<uint64_t>(count) * 8) {
            error = "invalid AOF GCMT participant vector";
            return nullptr;
        }
        for (uint32_t index = 0; index < count; index++) {
            const uint8_t* dependency = plan->control_section.data() + payload_pos + 4 + index * 8;
            const uint32_t sid = snapshot_get_u32(dependency);
            const uint32_t sequence = snapshot_get_u32(dependency + 4);
            if (sid >= plan->shard_count || sequence >= plan->next_sequence[sid]) {
                error = "AOF GCMT names a missing shard fragment";
                return nullptr;
            }
        }
        if (!plan->committed_groups.insert(group).second) {
            error = "duplicate AOF GCMT ticket";
            return nullptr;
        }
        plan->replayed_records++;
        control_pos = next;
    }

    for (uint32_t sid = 0; sid < plan->shard_count; sid++) {
        const auto& section = plan->sections[sid];
        size_t record_pos = 0;
        while (record_pos < section.size()) {
            AofRecordKind kind;
            uint8_t type = 0, encoding = 0;
            uint32_t key_len = 0;
            uint64_t payload_len = 0, group = 0;
            int64_t expire = -1;
            size_t next = 0;
            if (!parse_record_bounds(section, record_pos, kind, type, encoding, key_len,
                                     payload_len, expire, group, next, error)) return nullptr;
            (void)type; (void)encoding; (void)key_len; (void)payload_len; (void)expire;
            if (kind < AofRecordKind::Put || kind > AofRecordKind::GroupDel) {
                error = "unknown AOF record kind";
                return nullptr;
            }
            if ((kind == AofRecordKind::GroupPut || kind == AofRecordKind::GroupDel) && !group)
                { error = "AOF group fragment has no ticket"; return nullptr; }
            if ((kind == AofRecordKind::GroupPut || kind == AofRecordKind::GroupDel) &&
                !plan->committed_groups.count(group)) plan->groups_skipped++;
            plan->replayed_records++;
            record_pos = next;
        }
    }
    return plan;
}

bool aof_read_recovery(const Config& config, uint32_t expected_shards,
                       std::unique_ptr<SnapshotLoadPlan>& base,
                       std::vector<std::unique_ptr<AofReplayPlan>>& increments,
                       std::string& warning, std::string& error) {
    base.reset();
    increments.clear();
    warning.clear();
    error.clear();
    const std::string directory = aof_directory_path(config);
    const std::string basename = plain_name(config.appendfilename)
        ? config.appendfilename : "appendonly.aof";
    const std::string manifest_path = directory + "/" + basename + ".manifest";
    AofManifestData manifest;
    bool manifest_exists = false;
    if (!read_manifest(manifest_path, manifest_exists, manifest, error)) return false;
    if (!manifest_exists) {
        bool exists = false;
        std::string local_warning;
        auto plan = aof_read_plan(aof_file_path(config).c_str(), expected_shards, true,
                                  exists, local_warning, error);
        if (!error.empty()) return false;
        if (!local_warning.empty()) warning = local_warning;
        if (plan) increments.push_back(std::move(plan));
        return true;
    }

    if (!manifest.base_name.empty()) {
        const std::string base_path = directory + "/" + manifest.base_name;
        base = snapshot_read_plan(base_path.c_str(), expected_shards, error);
        if (!base) {
            if (error.empty()) error = "could not read AOF base snapshot";
            return false;
        }
        struct stat base_stat{};
        if (base->epoch != manifest.base_epoch || ::stat(base_path.c_str(), &base_stat) != 0 ||
            base_stat.st_size < 0 ||
            static_cast<uint64_t>(base_stat.st_size) != manifest.base_size) {
            error = "AOF base metadata does not match the manifest";
            return false;
        }
    }

    uint64_t replayed = 0;
    uint64_t skipped = 0;
    for (size_t index = 0; index < manifest.increments.size(); index++) {
        const auto& entry = manifest.increments[index];
        if (index >= manifest.increment_starts.size() ||
            manifest.increment_starts[index].size() != expected_shards) {
            error = "AOF manifest increment start vector has the wrong shard count";
            return false;
        }
        bool exists = false;
        std::string local_warning;
        const bool last = index + 1 == manifest.increments.size();
        const std::vector<uint32_t>* initial = &manifest.increment_starts[index];
        auto plan = aof_read_plan((directory + "/" + entry.second).c_str(), expected_shards,
                                  last, exists, local_warning, error, initial);
        if (!plan || !exists) {
            if (error.empty()) error = "AOF manifest increment file is missing";
            return false;
        }
        if (base && (plan->hash_kind != base->hash_kind || plan->hash_seed != base->hash_seed ||
                     plan->sip_k0 != base->sip_k0 || plan->sip_k1 != base->sip_k1)) {
            error = "AOF base and increment hash metadata differ";
            return false;
        }
        if (!increments.empty()) {
            const AofReplayPlan& first = *increments.front();
            if (plan->hash_kind != first.hash_kind || plan->hash_seed != first.hash_seed ||
                plan->sip_k0 != first.sip_k0 || plan->sip_k1 != first.sip_k1) {
                error = "AOF increment hash metadata differ";
                return false;
            }
        }
        replayed += plan->replayed_records;
        skipped += plan->groups_skipped;
        if (!local_warning.empty()) {
            if (!warning.empty()) warning += "; ";
            warning += entry.second + ": " + local_warning;
        }
        increments.push_back(std::move(plan));
    }
    increments.back()->replayed_records = replayed;
    increments.back()->groups_skipped = skipped;
    return true;
}

bool aof_load_shard(const AofReplayPlan& plan, Server& server, Shard& shard,
                    std::string& error) {
    const uint32_t sid = static_cast<uint32_t>(shard.id());
    if (sid >= plan.sections.size()) { error = "AOF shard section is missing"; return false; }
    const int64_t now = realtime_ms();
    shard.set_cached_now_ms(now);
    const std::vector<uint8_t>& section = plan.sections[sid];
    size_t pos = 0;
    while (pos < section.size()) {
        AofRecordKind kind;
        uint8_t type = 0, encoding = 0;
        uint32_t key_len = 0;
        uint64_t payload_len = 0, group = 0;
        int64_t expire = -1;
        size_t next = 0;
        if (!parse_record_bounds(section, pos, kind, type, encoding, key_len, payload_len,
                                 expire, group, next, error)) return false;
        const size_t data = pos + kRecordHeaderBytes;
        const Slice key(reinterpret_cast<const char*>(section.data() + data), key_len);
        const Slice payload(reinterpret_cast<const char*>(section.data() + data + key_len),
                            static_cast<uint32_t>(payload_len));
        pos = next;
        if (kind == AofRecordKind::Timestamp || kind == AofRecordKind::GroupCommit) continue;
        if (kind == AofRecordKind::Flush) {
            shard.store().clear();
            continue;
        }
        if ((kind == AofRecordKind::GroupPut || kind == AofRecordKind::GroupDel) &&
            !plan.committed_groups.count(group)) continue;
        const uint64_t hash = FlatStore::hash_key(key);
        if (server.router().shard_of(hash) != shard.id()) {
            error = "AOF key is in the wrong shard stream";
            return false;
        }
        if (kind == AofRecordKind::Del || kind == AofRecordKind::GroupDel ||
            (expire >= 0 && expire <= now)) {
            shard.store().erase(hash, key);
            continue;
        }
        if ((kind != AofRecordKind::Put && kind != AofRecordKind::GroupPut) ||
            type > static_cast<uint8_t>(Type::Stream)) {
            error = "invalid AOF value record";
            return false;
        }
        KvObj* object = nullptr;
        const SnapshotHookStatus status = snapshot_type_hooks(static_cast<Type>(type)).load(
            key, encoding, expire, payload, shard.type_limits(), object);
        if (status != SnapshotHookStatus::Ok || !object) {
            error = hook_error(status);
            return false;
        }
        if (shard.store().insert(hash, object) != FlatStore::InsertResult::Inserted) {
            kvobj_free(object);
            error = "could not insert AOF value";
            return false;
        }
        shard.store().note_loaded_object(hash, object);
    }
    shard.publish_size();
    return true;
}

bool aof_load_owned(const AofReplayPlan& plan, Server& server, ThreadCtx& owner,
                    std::string& error) {
    for (Shard* shard : owner.shards())
        if (!aof_load_shard(plan, server, *shard, error)) return false;
    return true;
}

bool aof_record_local_op(Shard& shard, Op& op, AofOwnerContext& context) {
    if (!(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) return true;
    if (op.spec->first_key <= 0) return true;
    uint32_t first = static_cast<uint32_t>(op.spec->first_key);
    uint32_t last = op.spec->last_key < 0 ? op.argc() - 1
                                          : static_cast<uint32_t>(op.spec->last_key);
    uint32_t step = static_cast<uint32_t>(op.spec->key_step);
    if (op.spec->flags & CmdFlags::ScriptRoute) {
        uint32_t count = 0;
        if (!command_script_key_range(op, first, count) || count == 0) return true;
        last = first + count - 1;
        step = 1;
    }
    if (!step || first >= op.argc()) return true;
    last = std::min(last, op.argc() - 1);
    bool ok = true;
    for (uint32_t arg = first; arg <= last; arg += step) {
        const Slice key = op.arg(arg);
        const uint64_t hash = arg == first && !op.local_xshard() ? op.hash
                                                                 : FlatStore::hash_key(key);
        ok &= shard.store().aof().record_post_image(shard.store(), hash, key, context);
        if (last - arg < step) break;
    }
    return ok;
}

}  // namespace tomo
