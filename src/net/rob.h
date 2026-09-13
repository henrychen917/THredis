// rob.h — the reorder buffer. One per connection.
//
// THE PROBLEM IT SOLVES. A pipelined client sends GET a, GET b, GET c on one socket. Those keys can
// live on three different workers, which finish in any order. The protocol requires the replies in
// the order sent. So we need out-of-order execution with in-order retirement — a reorder buffer, and
// the name is not an analogy: dispatch_id is the issue pointer, flush_id is the commit pointer, and
// a slot retires only when every older slot has retired.
//
//   dispatch_id  next id to issue     — advanced at parse
//   flush_id     next id to retire    — advanced at retire (both by the connection's io thread)
//   slot(id)     id & mask
//   in_flight    dispatch_id - flush_id
//
//   dispatch_id == flush_id is the universal quiescence fence. Nothing is in flight, so the
//   connection can be closed, migrated, or have its buffers recycled. Every teardown path tests
//   exactly this and nothing else.
//
// ============================================================================================
// WHAT CROSSES THREADS, in pure 2s: the io thread owns BOTH counters (it parses and it retires).
// The executor only ever touches individual Op slots — reached through chunk pointers published
// with release/acquire, contents ordered by the Op's own state handshake (Issued/Done). The two
// counters stay atomics anyway: on x86 the same-thread case degrades to ordinary loads and stores
// (zero cost), and dispatch_id == flush_id is the quiescence fence a future connection MIGRATION
// between io threads must read from the other side — demoting them buys nothing and closes a door.
//
// READ-BUFFER PINNING STILL FALLS OUT. argv Slices point into the connection's read buffer. Because
// retirement is strictly in order, the oldest live op is always slot(flush_id) — so every byte
// before that op's rbuf_off is dead. No refcounting, no generation numbers.
//
// SLOTS ARE CHUNKED, NOT INLINE AND NOT SCATTERED. An ExecContext is ~328 bytes; held by value a
// 64-slot ROB is ~21KB resident per connection whether it ever pipelines or not. The first pointer
// version fixed that with one heap Op per slot — and measured a −3% SET p32 regression, because 64
// scattered allocations lost the sequential locality the drain used to get from the inline array
// for free. So: contexts materialize in CONTIGUOUS CHUNKS of eight, on the first touch of any slot
// in the chunk. A p1 connection holds one chunk (~2.6KB), a 32-deep pipeliner four, and the drain
// walks sequential memory within every chunk. Each context recycles in place forever ("flushed"
// logically at retire — state goes Free — never returned to the allocator until the connection
// dies); steady-state allocator traffic is zero, and jemalloc needs no pool in front of it.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include "../exec/op.h"

namespace tomo {

// THE ROB WINDOW: the maximum number of ops one connection may have in flight. It lives here,
// in the ROB's own header, because it is the ROB's bound and because the read-local write ring
// below is sized FROM it -- one number, so the ring can never drift out of step with the window
// its unreachability proof is written against. net/conn.h re-exports it by including this file.
inline constexpr uint32_t kRobWindow = 64;   // max in-flight ops per connection

// COLD-PATH COUNTERS FOR ARM-ON-DEMAND (DESIGN-RINGDIET.md). Three events, none of them on a hot
// path: a connection being armed, its RYOW sidecar being allocated, and one write being committed
// into the ring. They are the instrument the design is proved with -- "pure SET does zero ring
// bookkeeping" is exactly write_ring_records staying at zero -- so they are always on rather than
// diagnostic-only, which is affordable precisely because every increment sits behind a noinline
// call that the unarmed stream never makes. The ROB holds a POINTER to the owning IO thread's
// block (installed at adopt and re-pointed at the migration edge) so that net/ need not know what
// a ThreadCtx is; core/thread.h embeds one by value inside ReadLocalStats.
struct ReadLocalArmStats {
    uint64_t arms = 0;                  // connections a local read armed
    uint64_t sidecars = 0;              // RYOW write-ring sidecars allocated (never at accept)
    uint64_t write_ring_records = 0;    // writes committed into the RYOW write ring
};

// Allocated only for connections served by the boot-armed fused read-local lane. The connection's
// IO owner is the sole reader/writer; the ROB ids carried beside hashes are generations, not
// cross-thread publications. Retirement is therefore lazy: flush_id advancing past an entry is
// its removal fence, and stale entries may only cause an allowed false-positive conflict.
struct alignas(64) ReadLocalRobState {
    // SIZED TO THE ROB WINDOW, WHICH MAKES CAPACITY OVERFLOW UNREACHABLE. A ring entry is removed
    // by retirement, so every live entry names an op in [flush_id, dispatch_id) -- distinct ids in
    // a window that is at most kRobWindow wide. Both insert sites prune immediately before testing
    // capacity, and the write being inserted holds one of those ids itself, so after the prune
    // write_count <= in_flight - 1 <= kRobWindow - 1. Rob asserts kWriteRingCapacity >= Capacity,
    // which is the whole argument. Measured against the instrument that does not depend on it
    // (scratchpad/ringsize): a client pipelining 64 deep at 100% writes tops out at exactly 63
    // live writes, one short of the window, and 41% reads at depth 32 tops out at 19.
    static constexpr uint32_t kWriteRingCapacity = kRobWindow;
    // A POLICY BOUND, NOT THE RING'S. How many keys a single blind MSET may name and still take a
    // precise ring slot instead of a conservative generation: every probe that hits that slot walks
    // the op's argv, so this trades demotions against walk length and has nothing to do with how
    // many writes the ring can hold. Deliberately left where it was when the two were one number.
    static constexpr uint32_t kMaxPreciseKeysetKeys = 16;
    static_assert((kWriteRingCapacity & (kWriteRingCapacity - 1)) == 0);
    static_assert(kWriteRingCapacity <= 64, "keyset/valid/wide slot bitmap width");
    static_assert(kMaxPreciseKeysetKeys <= kWriteRingCapacity);

    static constexpr uint64_t keyset_filter(uint64_t hash) {
        return (uint64_t{1} << (hash & 63)) |
               (uint64_t{1} << ((hash >> 32) & 63));
    }

    enum class PendingWrite : uint8_t { None, Hash, Keyset, Overflow };

    struct WriteKey {
        uint64_t hash = 0;
        uint64_t op_id = 0;
    };

    // INLINE REJECTION FILTER, slot for slot with the ring below. tags[i] is the low 16 bits of the
    // hash in ring slot i; the Rob's valid_/wide_ bitmaps say which slots a probe must consider and
    // which of those tag equality cannot speak for. Equal hashes have equal tags, so a miss over
    // the valid non-wide slots PROVES no ring entry holds this hash -- the no-false-negative half
    // of the RYOW contract. Everything else is one-sided: a stale bit or a tag collision only sends
    // a probe down the exact path it used to take unconditionally.
    //
    // IT LIVES HERE RATHER THAN IN THE ROB because sixty-four 16-bit tags are 128 bytes and the Rob
    // is locked at 192 with sixteen spare. It leads the struct so the probe's dereference lands on
    // the first two lines of the allocation -- the lines an interleaved connection's own write
    // frames keep hot -- and so a tag miss never touches the ring itself.
    uint16_t write_tags[kWriteRingCapacity]{};

    uint64_t pending_hash = 0;
    uint64_t pending_op_id = 0;
    uint64_t overflow_through = 0;
    uint64_t write_keyset_slots = 0;
    uint8_t write_head = 0;
    uint8_t write_count = 0;
    PendingWrite pending_write = PendingWrite::None;
    bool overflow = false;
    WriteKey write_ring[kWriteRingCapacity]{};
};
static_assert(alignof(ReadLocalRobState) >= 4, "read-local sidecar pointer uses its two low bits");
// write_count is compared against the capacity and must be able to HOLD it, even though the
// structural argument above says it never reaches it.
static_assert(ReadLocalRobState::kWriteRingCapacity <=
                  static_cast<uint32_t>(UINT8_MAX),
              "write_head/write_count are uint8_t");
// THE PER-CONNECTION BILL, LOCKED LIKE EVERY OTHER LAYOUT IN THIS TREE. 1216 bytes, which jemalloc
// rounds to its 1280-byte class -- measured as +965 bytes of RSS per connection that owns one
// (scratchpad/ringsize section 3) against the sixteen-slot sidecar's 296/320. It is NO LONGER paid
// by every connection: arm-on-demand allocates it at the first write of an ARMED connection, so a
// pure-write connection, a pure-read connection and an idle connection each carry none of it
// (DESIGN-RINGDIET.md). Locked all the same, so that a later field cannot quietly add another size
// class to every read/write connection without someone re-measuring that number.
// The MGET latest-read fence used to live here and now lives in the Rob: it is a read-side fence
// with nothing to do with the write ring, and keeping it here would have forced a pure-MGET
// connection to allocate the whole sidecar to hold one id.
static_assert(sizeof(ReadLocalRobState) == 1216,
              "the armed read-local sidecar grew: re-measure RSS per armed connection");

// Superset summary of every key hash that one connection's still-pending local reads may touch.
// The armed parser ORs a read's keyset in when it marks the slot pending; the words are cleared
// only when the pending bitmap drains to zero and are rewritten only from a walk of the complete
// pending set. Membership is therefore never lost while a read is pending: a miss PROVES that no
// pending read shares the probed hash, a hit merely means "run the exact demotion plan". Same
// hash-conservative contract as the RYOW write ring (a real 64-bit collision may take the owner
// path). Two bits per key across four words: ~8 pending GETs give well under 1% false hits.
struct ReadLocalPendingFilter {
    static constexpr uint32_t kWords = 4;
    static_assert((kWords & (kWords - 1)) == 0);

    static constexpr uint32_t word_of(uint64_t hash) {
        return static_cast<uint32_t>((hash >> 6) & (kWords - 1));
    }
    void add(uint64_t hash) {
        words[word_of(hash)] |= ReadLocalRobState::keyset_filter(hash);
    }
    bool may_contain(uint64_t hash) const {
        const uint64_t bits = ReadLocalRobState::keyset_filter(hash);
        return (words[word_of(hash)] & bits) == bits;
    }
    void merge(const ReadLocalPendingFilter& other) {
        for (uint32_t i = 0; i < kWords; i++) words[i] |= other.words[i];
    }
    void clear() {
        for (uint32_t i = 0; i < kWords; i++) words[i] = 0;
    }

    uint64_t words[kWords] = {};
};
static_assert(sizeof(ReadLocalPendingFilter) == 32, "fills the Rob's spare flush_-line tail");

template <uint32_t Capacity>
class Rob {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(Capacity <= 64, "read-local slot accounting uses one footprint-free word");
    static constexpr uint32_t kMask = Capacity - 1;

public:
    // ---- producer side (the parser / io thread) ------------------------------------------------
    uint64_t dispatch_id() const { return dispatch_.load(std::memory_order_relaxed); }
    uint64_t flush_id()    const { return flush_.load(std::memory_order_acquire); }

    uint32_t in_flight() const { return static_cast<uint32_t>(dispatch_id() - flush_id()); }
    bool     full()      const { return in_flight() >= Capacity; }
    bool     quiesced()  const { return dispatch_id() == flush_id(); }

    // Claim the next slot to fill. Null when the window is full — the caller must stop parsing and
    // let the sender drain. That backpressure is what bounds memory under a client that pipelines
    // without reading, and dropping it is how a server OOMs on one misbehaving connection.
    // Materialization happens HERE and only here — the parser is the sole allocator, so workers
    // and the sender only ever dereference slots that a publish made real.
    // `Codes` GATES THE CODED REPLY BY THREAD MODE, at compile time. Both call sites (io_loop's
    // parse loop) sit inside an `if constexpr (Fused)` arm, so this is a constant store of 1 or 0,
    // not a branch.
    //
    // WHY 2s IS OFF. Coded replies are a measured win in fused (SET -2.4%, DEL -3.2% cycles/op on
    // the quiet box) and a measured LOSS on the split GET cell (-1.4%, reproduced on both runs of
    // an ABBA pair), which is a main command and therefore blocks a both-modes merge under the
    // zero-regression rule. With Codes=false no 2s op is ever armed, Sink::code() always declines,
    // op.reply_code_ stays 0 for the life of every 2s op, and the split path executes exactly the
    // bytes it executed before -- the retire guards are all `if (op.reply_code_)` and none can be
    // taken. The coded blocks remain in the shared machine code (WbEngine is not templated on
    // thread mode); they are unreachable in 2s rather than absent.
    template <bool Codes = false>
    Op* acquire(uint8_t route_flags = 0) {
        if (full()) return nullptr;
        Op* op = slot(static_cast<uint32_t>(dispatch_id()) & kMask, true);
        op->reset<Codes>(route_flags);
        // Not `= Codes ? 1 : 0`: in split that is still a store per op, for a field that is
        // already zero and that nothing in that mode ever reads as anything else.
        if constexpr (Codes) op->reply_code_ok_ = 1;
        return op;
    }

    // The armed coarse parser owns the local-read slot bitmap and write generations. Resolve the
    // preceding candidate before recycling a position; ordinary acquire() above stays baseline.
    //
    // SHAPE, not policy: this body performs exactly what the single-function version performed, in
    // the same order. The write-generation resolution moved behind a noinline call because with it
    // inline the function reached 917 bytes and GCC declined to inline it into the (very large)
    // parse loop -- so every armed frame paid a call and a register-allocation barrier to discover
    // that no write generation was live. acquire() next door, which is the same shape minus that
    // machinery, inlines at every one of its call sites.
    //
    // THE GATE IS "a write is staged", NOT "a write generation is live". Only mark_current_write()
    // ever stages a candidate and only read_local_resolve_pending() ever clears one, so the STAGED
    // tag bit on the sidecar pointer is exactly the predicate "resolve_read_local_write() has work
    // to do". Gating on armedness instead made every frame of an interleaved connection call out of
    // line, deref the heap sidecar and run the deactivation test only to find PendingWrite::None --
    // a read frame between two writes can never have a candidate of its own to commit. The frame
    // that stages is still the frame BEFORE the resolve, so the commit point ("dispatch advanced =>
    // it published") and the ring's age order are bit-for-bit what they were.
    __attribute__((always_inline)) Op* acquire_read_local(uint8_t route_flags = 0) {
        if (__builtin_expect(read_local_state_staged(), false)) resolve_read_local_write();
        // One dispatch load serves the window test and the slot index. Only this connection's io
        // thread stores dispatch_ (see the header note), and nothing between the two uses stores
        // it, so the second load the previous shape performed could only return the same value.
        const uint64_t dispatch = dispatch_.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(dispatch - flush_id()) >= Capacity) return nullptr;
        const uint32_t index = static_cast<uint32_t>(dispatch) & kMask;
        const uint64_t keep = ~(uint64_t{1} << index);
        read_local_pending_slots_ &= keep;
        read_local_owner_slots_ &= keep;
        Op* op = slot(index, true);
        op->reset_read_local(route_flags);
        // Read-local is a FUSED-only path, so this arm is unconditional for the same reason
        // acquire<true>() is: only 1s reaches here.
        op->reply_code_ok_ = 1;
        return op;
    }

    // Cold half of acquire_read_local: the previous frame staged a write candidate, so it must be
    // resolved into the ring (or into an overflow generation) before this position is recycled.
    __attribute__((noinline)) void resolve_read_local_write() {
        ReadLocalRobState& state = read_local_state_required();
        read_local_resolve_pending(state);
        read_local_try_deactivate(state);
    }

    // Publish the slot claimed by acquire(). RELEASE: everything written into the slot must be
    // visible to the consumer before it can observe the new dispatch_id. Separate from acquire()
    // because the parser may abandon a half-built op without advancing the ROB.
    void publish() { dispatch_.store(dispatch_id() + 1, std::memory_order_release); }

    // Undo the last publish(). Only legal while the op is still un-dispatched, i.e. no worker can
    // have marked it Done -- which is exactly the refused-dispatch path. Safe even if a sender has
    // already observed the higher dispatch_: it can only have seen the slot as not-Done and stopped,
    // because retirement never touches an op that is not Done.
    void unpublish() { dispatch_.store(dispatch_id() - 1, std::memory_order_release); }

    // The enabled parser marks a slot while its Task still belongs to the local lane. The fused
    // executor clears it only after the read has completed locally or has entered an ordinary
    // owner queue. Thus a later write can distinguish work that still needs venue demotion from an
    // already-executed read without adding any hook to ordinary publish or retirement.
    // `keys` must cover every hash read_local_command_touches_hash() can report for this op: the
    // demotion planner's pre-check treats a filter miss as proof that no pending read overlaps.
    // `op_id` must be the current dispatch id -- the caller has just loaded it to name the op it
    // is about to publish (io_loop.h, the read-local accept arm), so the load is not repeated here.
    __attribute__((always_inline)) void mark_current_read_local(
            uint64_t op_id, const ReadLocalPendingFilter& keys) {
        mark_current_read_local_slot(op_id);
        read_local_pending_filter_.merge(keys);
    }

    // Point form. A marked read that is not an MGET reports exactly one hash to every predicate
    // that consults this filter: read_local_command_touches_hash() takes its non-MGET/non-precise-
    // MSET branch for such an op and compares op.hash alone, and the keyset overlap predicates use
    // read.hash alone. Merging a four-word summary whose other three words the caller had just
    // zeroed set the identical bits at three times the traffic.
    __attribute__((always_inline)) void mark_current_read_local_hash(
            uint64_t op_id, uint64_t hash) {
        mark_current_read_local_slot(op_id);
        read_local_pending_filter_.add(hash);
    }

    // False proves that no still-pending local read may touch `hash`; true only means "walk".
    bool read_local_pending_may_touch(uint64_t hash) const {
        return read_local_pending_filter_.may_contain(hash);
    }
    const ReadLocalPendingFilter& read_local_pending_filter() const {
        return read_local_pending_filter_;
    }
    // Legal only with a summary just computed over EVERY currently pending read (the planner's
    // complete walk); it restores exactness after false hits without touching the ordinary path.
    void reset_read_local_pending_filter(const ReadLocalPendingFilter& exact) {
        read_local_pending_filter_ = exact;
    }

    // A local MGET is one command-wide latest-read fence: no younger frame may receive a read cut
    // until this id either completes locally or is irrevocably transferred to the owner path. It
    // lives on the Rob's own producer line rather than in the write-ring sidecar because it is a
    // READ-side fence -- a connection that only ever sends MGETs must not have to allocate 1216
    // bytes of write ring to hold one id (DESIGN-RINGDIET.md).
    void arm_current_local_mget_fence() {
        if (local_mget_fence_id_ != UINT64_MAX) std::abort();
        local_mget_fence_id_ = dispatch_id();
    }

    bool local_mget_fence_pending() {
        if (local_mget_fence_id_ == UINT64_MAX) return false;
        if (read_local_id_active(local_mget_fence_id_, dispatch_id(), flush_id())) return true;
        local_mget_fence_id_ = UINT64_MAX;
        return false;
    }

    bool pending_read_local(uint64_t op_id) const {
        const uint64_t dispatch = dispatch_id();
        const uint64_t flush = flush_id();
        return read_local_id_active(op_id, dispatch, flush) &&
               (read_local_pending_slots_ &
                (uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask))) != 0;
    }

    bool has_pending_read_local() const { return read_local_pending_slots_ != 0; }
    // Lane slots this connection holds right now: local reads published and not yet executed (or
    // demoted). The parser's fair-share admission (P128.md) compares THIS, not in_flight(): a
    // reply that PHASE 2's bounded serve has not flushed yet holds no lane slot and must not
    // count against the connection's share (measured: it halved ops per rotation).
    uint32_t pending_read_local_count() const {
        return static_cast<uint32_t>(__builtin_popcountll(read_local_pending_slots_));
    }

    // Chunk forms for the fused local drain. One dispatch/flush snapshot serves a whole same-client
    // chunk: flush can only pass Done ops and no lane op is Done before this thread publishes it,
    // while dispatch only grows and every lane op published below it, so a snapshot taken once per
    // chunk classifies each entry exactly as a fresh pair would. The mask clear retires the chunk's
    // completed prefix with one store; the ids of in-flight ops are distinct modulo the window.
    bool pending_read_local(uint64_t op_id, uint64_t dispatch, uint64_t flush) const {
        return read_local_id_active(op_id, dispatch, flush) &&
               (read_local_pending_slots_ &
                (uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask))) != 0;
    }
    static uint64_t read_local_slot_bit(uint64_t op_id) {
        return uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask);
    }
    // Retires through the same clear-on-empty rule as the per-op form: when the drained chunk was
    // the whole pending set the summary restarts from zero (the contract stated in fcab80884; the
    // chunk form had dropped it, so the filter only ever grew in read-heavy streams and the first
    // write of every batch paid the exact walk — AUDIT-MARKDIET.md section 4).
    void complete_pending_read_local_mask(uint64_t bits) {
        if ((read_local_pending_slots_ & bits) != bits) std::abort();
        read_local_retire_pending_bit(bits);
    }
    // Owner-map emptiness is chunk-stable inside the drain: only this thread's parser and
    // demotion add owner slots and neither runs while a chunk executes.
    bool has_read_local_owner() const { return read_local_owner_slots_ != 0; }

    uint32_t collect_pending_read_local(uint64_t hash, bool hash_only, uint64_t* ids,
                                        uint32_t capacity) const {
        if (!read_local_pending_slots_) return 0;
        const uint64_t flush = flush_id();
        const uint32_t begin = static_cast<uint32_t>(flush) & kMask;
        const uint64_t generation = flush & ~uint64_t{kMask};
        uint64_t high = read_local_pending_slots_ & (UINT64_MAX << begin);
        const uint64_t low_mask = begin ? (uint64_t{1} << begin) - 1 : 0;
        uint64_t low = read_local_pending_slots_ & low_mask;
        uint32_t count = 0;
        // Enumerate set bits in logical ROB order across a possible slot-zero wrap.
        auto append = [&](uint64_t bits, uint64_t base) {
            while (bits) {
                const uint32_t index = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint64_t id = base | index;
                if (hash_only && at(id).hash != hash) continue;
                if (count == capacity) std::abort();
                ids[count++] = id;
            }
        };
        append(high, generation);
        append(low, generation + Capacity);
        return count;
    }

    void complete_pending_read_local(uint64_t op_id) {
        const uint64_t bit = uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask);
        if (!(read_local_pending_slots_ & bit)) std::abort();
        read_local_retire_pending_bit(bit);
        read_local_clear_mget_fence(op_id);
    }

    void publish_pending_read_local_to_owner(uint64_t op_id) {
        const uint64_t bit = uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask);
        if (!(read_local_pending_slots_ & bit) || (read_local_owner_slots_ & bit))
            std::abort();
        read_local_retire_pending_bit(bit);
        read_local_owner_slots_ |= bit;
        read_local_clear_mget_fence(op_id);
    }

    // Test only unfinished owner work that is older than `before_id`. Parser callers pass the
    // current dispatch id; EX callers pass the local task's id so a younger demotion cannot fence
    // an older read. The predicate supplies GET/MGET keyset overlap without teaching the ROB how
    // commands encode keys.
    // NOT split the way acquire_read_local and read_local_write_conflicts are. This one is
    // instantiated once per predicate lambda -- about a dozen times across the parser and the fused
    // drain, most of them cold -- so forcing its shell inline duplicated it at every site: +2613
    // bytes of fused parse and +1367 of fused drain, for 0.13 points MORE instructions per read.
    // Measured, not assumed: scratchpad/robdiet/bisect.csv, arm D against arm C.
    template <typename Match>
    bool read_local_owner_conflicts_before(uint64_t before_id, Match&& match) {
        if (!read_local_owner_slots_) return false;
        // Use one dispatch/flush snapshot both to prune the bitmap and to reconstruct wrapped IDs.
        // The sender may advance flush concurrently; mixing two snapshots could otherwise interpret
        // a just-retired low slot as belonging to the next ROB generation.
        const uint64_t dispatch = dispatch_id();
        const uint64_t flush = flush_id();
        read_local_owner_slots_ &= active_slots_mask(dispatch, flush);
        uint64_t active = read_local_owner_slots_;
        if (!active) return false;
        const uint64_t before_distance = before_id - flush;
        if (before_distance > dispatch - flush) std::abort();
        const uint32_t begin = static_cast<uint32_t>(flush) & kMask;
        const uint64_t generation = flush & ~uint64_t{kMask};
        uint64_t high = active & (UINT64_MAX << begin);
        const uint64_t low_mask = begin ? (uint64_t{1} << begin) - 1 : 0;
        uint64_t low = active & low_mask;
        bool conflict = false;
        auto inspect = [&](uint64_t bits, uint64_t base) {
            while (bits) {
                const uint32_t index = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint64_t id = base | index;
                const Op& op = at(id);
                if (op.state.load(std::memory_order_acquire) == OpState::Done) {
                    read_local_owner_slots_ &= ~(uint64_t{1} << index);
                    continue;
                }
                if (id - flush < before_distance && match(op)) conflict = true;
            }
        };
        inspect(high, generation);
        inspect(low, generation + Capacity);
        return conflict;
    }

    // Mark an owner-routed read (or the precise point operation that forced its demotion). Younger
    // reads consult this slot only when their keysets overlap; ROB retirement alone orders replies.
    void extend_current_read_local_owner() {
        read_local_owner_slots_ |=
            uint64_t{1} << (static_cast<uint32_t>(dispatch_id()) & kMask);
    }

    // Stage a conservative write record before the parser enters ACL/special lowering. The next
    // armed acquire is the commit point: an advanced dispatch id means this op published, while an
    // unchanged id means dispatch was abandoned (including publish + refused-post + unpublish).
    // Every write starts conservative. After arity and routing are known, ordinary point writes
    // and bounded blind keysets may refine it; all other special/multi-key writes leave it broad.
    void mark_current_write() {
        // ARM ON DEMAND, THE WRITE HALF (DESIGN-RINGDIET.md). Until a local read has armed this
        // connection the ring records NOTHING: no sidecar, no prune, no descriptor, no Staged tag
        // -- and so no resolve on the next frame either. The entire bookkeeping is one store of
        // this write's id, into a word on the producer's own cache line that dispatch_ has already
        // dirtied. A pure-write connection therefore pays a predicted-taken test and that store,
        // which is what the sixteen-slot ring used to pay for by GIVING UP after an overflow.
        if (__builtin_expect(read_local_arm_state_ == kReadLocalUnarmed, true)) {
            read_local_unarmed_write_id_ = dispatch_id();
            return;
        }
        // FIRST WRITE OF AN ARMED CONNECTION: this is where the sidecar is born. Read-only and
        // idle connections never reach it, which is the whole point of not allocating at accept.
        if (__builtin_expect(read_local_state_ == 0, false) && !prepare_read_local()) {
            // Out of memory. Fall back to the unarmed contract, which is always safe: it records
            // nothing and the next read that arrives while this write is live is demoted to the
            // owner path. The next quiescent read re-arms and the next write retries the
            // allocation, so the connection heals itself without a sticky failure flag.
            read_local_arm_state_ = kReadLocalUnarmed;
            read_local_unarmed_write_id_ = dispatch_id();
            return;
        }
        read_local_state_activate();
        ReadLocalRobState& state = read_local_state_required();
        if (state.pending_write != ReadLocalRobState::PendingWrite::None) std::abort();
        read_local_prune(state);
        state.pending_op_id = dispatch_id();
        state.pending_hash = 0;
        state.pending_write = ReadLocalRobState::PendingWrite::Overflow;
        read_local_state_ |= kReadLocalStateStaged;
    }

    bool refine_current_write_hash(uint64_t hash) {
        // Nothing was staged on an unarmed connection, so there is nothing to refine. TRUE is the
        // answer an unfilled ring gives, and it is the right one: the caller uses this only to
        // decide whether the OP may claim exact-write status downstream (the eviction guard in
        // ex_loop), a question about the command's keys that ring capacity never had a say in.
        if (__builtin_expect(read_local_arm_state_ == kReadLocalUnarmed, true)) return true;
        ReadLocalRobState& state = read_local_state_required();
        if (state.pending_write != ReadLocalRobState::PendingWrite::Overflow ||
            state.pending_op_id != dispatch_id())
            std::abort();
        // Once overflowed, every subsequently published write extends the conservative generation
        // until that whole run drains. Do not start tracking precise hashes again in its middle.
        //
        // THE SECOND TEST IS A KEPT FALLBACK, NOT A LIVE PATH. mark_current_write() pruned the ring
        // to the ops still in flight one statement ago, and this frame holds a window position of
        // its own, so write_count is at most Capacity-1 against a ring of Capacity slots (the
        // static_assert in Rob). It is kept because "conservative" is the only safe answer if that
        // reasoning is ever wrong, and deleting a correct fallback to celebrate a proof is how a
        // proof gets to be wrong in silence. Conservative generations remain ordinary traffic by
        // the OTHER door: any write that never refines -- a wide multi-key write, or a point write
        // under an evicting maxmemory policy -- is one, and the ring's overflow machinery below
        // serves them exactly as before.
        if (state.overflow ||
            state.write_count == ReadLocalRobState::kWriteRingCapacity)
            return false;
        state.pending_hash = hash;
        state.pending_write = ReadLocalRobState::PendingWrite::Hash;
        return true;
    }

    // A bounded precise multi-key write occupies one ring slot, just like a point write. Its live
    // ROB op owns the immutable key argv used by the conflict predicate, while the stored filter
    // rejects almost every disjoint probe without rescanning argv. Wider keysets stay Overflow.
    bool refine_current_write_keyset(uint64_t filter, uint32_t key_count) {
        // The policy bound is a property of the COMMAND, so it is answered before the ring is
        // consulted and its answer is the same armed or not.
        if (!key_count || key_count > ReadLocalRobState::kMaxPreciseKeysetKeys || !filter)
            return false;
        // Same reasoning as refine_current_write_hash: unarmed stages nothing and answers the way
        // an unfilled ring answers, so the op's precise-write marking is bit-for-bit unchanged.
        if (__builtin_expect(read_local_arm_state_ == kReadLocalUnarmed, true)) return true;
        ReadLocalRobState& state = read_local_state_required();
        if (state.pending_write != ReadLocalRobState::PendingWrite::Overflow ||
            state.pending_op_id != dispatch_id())
            std::abort();
        // Same kept fallback as refine_current_write_hash: unreachable by the window argument,
        // retained because conservative is the safe answer if the argument is ever wrong.
        if (state.overflow ||
            state.write_count == ReadLocalRobState::kWriteRingCapacity)
            return false;
        state.pending_hash = filter;
        state.pending_write = ReadLocalRobState::PendingWrite::Keyset;
        return true;
    }

    // Hash collisions deliberately conflict. Empty is the pure-GET fast path: it avoids even the
    // flush frontier load -- and that emptiness test is this shell, which stays inline. It was
    // already the merged function's first line, but the merged function was 612 bytes and out of
    // line, so a pure GET paid a call to be told that nothing was pending.
    //
    // THE SECOND TEST IS FOR THE INTERLEAVED CONNECTION. A connection that carries writes keeps a
    // write generation live permanently -- the previous batch's writes are still in flight while
    // this batch's reads parse -- so the armedness test above stops rejecting anything and every
    // read paid the full exact path: a call, the retired-prefix prune and a walk of every live
    // descriptor, to learn that its key is not one of them. The tag filter answers that same
    // question from two words on this line plus, only if they say a precise write really is live,
    // a branchless sweep of the sidecar's tag mirror -- and only a tag hit (or a keyset/overflow/
    // staged slot, which tag equality cannot speak for) pays the walk.
    template <typename KeysetTouchesHash>
    __attribute__((always_inline)) bool read_local_write_conflicts(
            uint64_t hash, KeysetTouchesHash&& keyset_touches_hash) {
        // ARM ON DEMAND, THE READ HALF (DESIGN-RINGDIET.md). ONE predictable test, on a word this
        // frame's acquire_read_local has already pulled into L1 with dispatch_, and nothing is
        // evaluated behind it: a connection in steady state (armed, no arming generation left in
        // flight) reads a zero and falls straight through to the unchanged probe below. The word
        // is non-zero for exactly two states, both of which end for good after the connection's
        // first read: not yet armed, and armed with pre-arming writes still in flight.
        if (__builtin_expect(read_local_arm_state_ != 0, false))
            if (read_local_arm()) return true;
        // One load of the sidecar word answers both hazards it names. A staged candidate is
        // consulted directly by the exact walk and no ring slot describes it, so it is read off
        // the same word rather than costing the write frame a store into the filter.
        const uintptr_t state_word = read_local_state_;
        if (state_word == 0 || (state_word & kReadLocalStateInactive) != 0) return false;
        if ((state_word & kReadLocalStateStaged) == 0 &&
            !read_local_write_tag_may_match(state_word, hash)) return false;
        return read_local_write_conflicts_pending(
            hash, static_cast<KeysetTouchesHash&&>(keyset_touches_hash));
    }

    // False PROVES that no live write descriptor holds `hash`; true means "run the exact walk".
    // Both words that can answer without touching the heap are read first: a live conservative
    // generation forces the walk outright, and an empty valid_ ends the probe on the Rob's own
    // cache line. Only a connection that really is carrying live precise writes reaches the tag
    // mirror in the sidecar, and for such a connection those two lines are hot -- its own write
    // frames wrote them.
    __attribute__((always_inline)) bool read_local_write_tag_may_match(
            uintptr_t state_word, uint64_t hash) const {
        if (read_local_write_force_) return true;
        const uint64_t live = read_local_write_valid_;
        if (!live) return false;
        // A live keyset slot stores a key filter, not a hash: no tag can reject it, and there is
        // nothing to gain by finding out which one it is.
        if (read_local_write_wide_ & live) return true;
        const uint16_t* tags = reinterpret_cast<const ReadLocalRobState*>(
            state_word & ~kReadLocalStateTagBits)->write_tags;
        const uint16_t tag = static_cast<uint16_t>(hash);
        // SIXTEEN LANES AT A TIME, AND ONLY OVER THE GROUPS THAT HOLD SOMETHING.
        //
        // THE LANE WIDTH IS A CODEGEN FACT, NOT A STYLE CHOICE. The straight
        // `for (i < 64) hits |= (tags[i] == tag) << i` that mirrored the old sixteen-slot filter
        // one-for-one does not vectorise: GCC 13.3 leaves it a scalar eight-instruction loop run
        // sixty-four times, and it measures 599 instructions and 145 cycles on the reject path a
        // disjoint read takes every time (scratchpad/ringsize/probe_cost.sh). Sixteen lanes is the
        // group size GCC turns into a vpcmpeqw against a constant bit-weight vector and an
        // OR-reduction. A first draft of this lane shipped the flat shape into a rate A/B and read
        // -8% at 61% reads before anyone disassembled it.
        //
        // THE GROUP WALK IS DRIVEN BY live, NOT BY THE CAPACITY. The lowest set bit names the first
        // group holding a live slot, and clearing that group's whole mask walks straight to the
        // next: an empty group costs nothing at all, so a connection carrying nine live writes pays
        // for one group whether the ring has sixteen slots or sixty-four. That is what makes the
        // bigger ring affordable for the shallow connection, which is the common one -- the
        // measured mean is six at 61% reads and nine at 41%. Measured per rejected probe against
        // the sixteen-slot filter this replaces (probe_cost.sh, one connection, 20M probes):
        //
        //     live writes      1..15        19          40        63
        //     sixteen-slot   32 instr   88 instr*      n/a       n/a     * conservative generation:
        //     this sweep     49 instr   73 instr   97 instr  121 instr     the ring is FULL, and
        //     sixteen-slot    5.1 cyc   15.1 cyc       n/a       n/a       every one of those 88
        //     this sweep      8.1 cyc   11.7 cyc  16.2 cyc  19.5 cyc       instructions ends in a
        //                                                                  false conflict.
        // Seventeen instructions and three cycles more while the old ring could still hold the run,
        // and fifteen fewer once it could not -- while returning the right answer instead of
        // fencing every read. That is the whole trade, and it is why the walk is bounded by live.
        uint64_t rest = live;
        do {
            const uint32_t g =
                static_cast<uint32_t>(__builtin_ctzll(rest)) / kTagLanes;
            const uint32_t shift = g * kTagLanes;
            const uint32_t live_g = static_cast<uint32_t>(rest >> shift) & 0xFFFFu;
            uint32_t hits = 0;
            for (uint32_t i = 0; i < kTagLanes; i++)
                hits |= static_cast<uint32_t>(tags[shift + i] == tag) << i;
            if (hits & live_g) return true;
            rest &= ~(uint64_t{0xFFFF} << shift);
        } while (rest);
        return false;
    }

    // A write generation is live: prune the retired FIFO prefix, then scan the live write
    // descriptors -- at most one per ROB window position, and measured at nineteen for the worst
    // pipelined mix (scratchpad/ringsize). Hash entries compare directly; exact keyset entries ask
    // the caller to inspect their still-live ROB argv. Overflow remains conservative until every
    // write published during that overflow generation has retired. Unchanged from the merged form.
    template <typename KeysetTouchesHash>
    __attribute__((noinline)) bool read_local_write_conflicts_pending(
            uint64_t hash, KeysetTouchesHash&& keyset_touches_hash) {
        ReadLocalRobState& state = read_local_state_required();
        const bool has_keysets =
            state.pending_write == ReadLocalRobState::PendingWrite::Keyset ||
            state.write_keyset_slots != 0;
        const uint64_t probe_filter = has_keysets
            ? ReadLocalRobState::keyset_filter(hash) : 0;
        // acquire_read_local normally resolves this first. Preserve the no-false-negative contract
        // if a future armed caller probes without acquiring through that door.
        if (state.pending_write == ReadLocalRobState::PendingWrite::Overflow) return true;
        if (state.pending_write == ReadLocalRobState::PendingWrite::Hash &&
            state.pending_hash == hash)
            return true;
        if (state.pending_write == ReadLocalRobState::PendingWrite::Keyset) {
            if ((state.pending_hash & probe_filter) == probe_filter &&
                keyset_touches_hash(at(state.pending_op_id), hash))
                return true;
        }
        if (!state.overflow && state.write_count == 0) {
            read_local_try_deactivate(state);
            return false;
        }
        read_local_prune(state);
        if (state.overflow) return true;
        if (state.write_count == 0) {
            read_local_try_deactivate(state);
            return false;
        }
        for (uint32_t i = 0; i < state.write_count; i++) {
            const uint32_t at =
                (static_cast<uint32_t>(state.write_head) + i) &
                (ReadLocalRobState::kWriteRingCapacity - 1);
            if (state.write_keyset_slots & (uint64_t{1} << at)) {
                if ((state.write_ring[at].hash & probe_filter) == probe_filter &&
                    keyset_touches_hash(this->at(state.write_ring[at].op_id), hash))
                    return true;
            } else if (state.write_ring[at].hash == hash) {
                return true;
            }
        }
        return false;
    }

    // The RYOW write ring's sidecar. Called from the connection's own IO thread on the first
    // write of an ARMED connection -- never at accept (DESIGN-RINGDIET.md) -- and directly by the
    // unit test, which drives the ROB without a server. Returns false only on allocation failure;
    // every caller has a safe unarmed fallback for that.
    bool prepare_read_local() {
        if (read_local_state_) return true;
        ReadLocalRobState* state = new (std::nothrow) ReadLocalRobState;
        if (!state) return false;
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(state);
        if (ptr & kReadLocalStateTagBits) std::abort();
        read_local_state_ = ptr | kReadLocalStateInactive;
        if (read_local_arm_stats_) read_local_arm_stats_->sidecars++;
        return true;
    }

    // Installed by the IO thread that owns this connection (adopt_client) and re-pointed at the
    // migration ownership edge, so every increment below is a plain store by the owning thread.
    void set_read_local_arm_stats(ReadLocalArmStats* stats) { read_local_arm_stats_ = stats; }

    // True while reads on this connection are being demoted by the ARMING TRANSIENT rather than by
    // an explicit key conflict. The parser reads it only on the demote path, to attribute the
    // fallback; it is never consulted by a read that is being served.
    bool read_local_arm_fence_live() const {
        return read_local_arm_state_ == kReadLocalArmFence;
    }

    // Test hook: the unarmed/armed/transient state, so the unit test can assert the transition
    // rather than infer it. 1 = unarmed, 2 = armed with pre-arming writes still in flight, 0 =
    // armed and recording.
    uint32_t read_local_arm_state() const { return read_local_arm_state_; }

    // ---- consumer side (whichever stage sends) -------------------------------------------------
    // Retire every completed op from the head, in order, handing each reply to `sink`. Stops at the
    // first op still running — a later op finishing early must wait, which is the whole point.
    template <typename Sink>
    uint32_t drain(Sink&& sink) {
        const uint64_t d = dispatch_.load(std::memory_order_acquire);
        uint64_t f = flush_.load(std::memory_order_relaxed);
        uint32_t n = 0;
        while (f != d) {
            Op& op = *slot(static_cast<uint32_t>(f) & kMask, false);
            if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
            sink(op);                                   // the acquire above orders the reply bytes
            if (op.oversized()) op.shrink();            // bounded retention: bursts do not pin heap
            op.state.store(OpState::Free, std::memory_order_relaxed);
            f++;
            n++;
        }
        // One release store for the whole batch rather than one per op: the producer only needs to
        // learn the final position, and each store would otherwise bounce the line.
        if (n) flush_.store(f, std::memory_order_release);
        return n;
    }

    // Slot access for the worker side, which addresses ops by id rather than by pointer so a stale
    // pointer can never outlive a recycle.
    Op& at(uint64_t id) { return *slot(static_cast<uint32_t>(id) & kMask, false); }
    const Op& at(uint64_t id) const { return const_cast<Rob*>(this)->at(id); }

    ~Rob() {
        for (uint32_t i = 0; i < kChunks; i++) delete[] chunks_[i];
        delete read_local_state_ptr();
    }

private:
    static constexpr uint32_t kChunkOps = 8;
    static constexpr uint32_t kChunks   = Capacity / kChunkOps;
    static_assert(Capacity % kChunkOps == 0);

    // may_grow is true only from acquire() — the parser. Everyone else dereferences ground the
    // parser already materialized.
    __attribute__((always_inline)) Op* slot(uint32_t idx, bool may_grow) {
        Op*& ch = chunks_[idx / kChunkOps];
        if (!ch && may_grow) ch = new Op[kChunkOps];
        return &ch[idx % kChunkOps];
    }

    static constexpr uint64_t capacity_slots_mask() {
        if constexpr (Capacity == 64) return UINT64_MAX;
        else return (uint64_t{1} << Capacity) - 1;
    }

    static uint64_t active_slots_mask(uint64_t dispatch, uint64_t flush) {
        const uint32_t count = static_cast<uint32_t>(dispatch - flush);
        if (!count) return 0;
        if (count >= Capacity) return capacity_slots_mask();
        const uint32_t begin = static_cast<uint32_t>(flush) & kMask;
        const uint64_t run = (uint64_t{1} << count) - 1;
        uint64_t mask = run << begin;
        if (begin + count > Capacity) mask |= run >> (Capacity - begin);
        return mask & capacity_slots_mask();
    }

    // THE STRUCTURAL BOUND THE RING IS SIZED BY. A ring entry lives exactly while its op is in
    // flight, so the live entries name distinct ids inside a window at most Capacity wide, and both
    // insert sites prune to that set immediately before testing capacity. With a slot per window
    // position the capacity test can therefore never fire -- see refine_current_write_hash and
    // read_local_resolve_pending_body, where the conservative fallback is kept anyway.
    static_assert(ReadLocalRobState::kWriteRingCapacity >= Capacity,
                  "the RYOW write ring must cover the whole ROB window");
    static_assert(ReadLocalRobState::kWriteRingCapacity <= 64,
                  "valid_/wide_ slot bitmap width");
    // The tag sweep's vector width. Sixteen because that is the group size GCC turns into a
    // vpcmpeqw plus a bit-weight OR-reduction; see read_local_write_tag_may_match.
    static constexpr uint32_t kTagLanes = 16;
    static_assert(ReadLocalRobState::kWriteRingCapacity % kTagLanes == 0);

    // Two tag bits on the sidecar pointer, both owned by the connection's io thread.
    //   Inactive  no write generation is live at all: pure-GET traffic never dereferences the heap.
    //             (It is read only by read_local_write_conflicts' inline shell, which tests the
    //             whole word at once; the predicate accessor that used to wrap it had no callers
    //             left once the MGET fence moved onto the Rob, so it is gone rather than dormant.)
    //   Staged    mark_current_write() has parked a candidate that no resolve has committed yet.
    // Staged is a strict subset of "active": staging activates first, and deactivation is refused
    // while a candidate is parked (read_local_try_deactivate tests pending_write == None).
    static constexpr uintptr_t kReadLocalStateInactive = 1;
    static constexpr uintptr_t kReadLocalStateStaged   = 2;
    static constexpr uintptr_t kReadLocalStateTagBits =
        kReadLocalStateInactive | kReadLocalStateStaged;

    // ARM-ON-DEMAND STATE (DESIGN-RINGDIET.md), one word on the producer line, zero in the state
    // every connection ends up in. Two non-zero values, and a connection passes through each of
    // them at most once:
    //   Unarmed   no local read has been seen. Writes record nothing but their newest id.
    //   ArmFence  a read has armed the connection, but writes PUBLISHED BEFORE that arming are
    //             still in flight and no descriptor describes them. Every read is demoted until
    //             the newest of them retires -- which, retirement being in order, retires all of
    //             them. This is the transient, and it is bounded by one ROB drain.
    // It is deliberately NOT the sidecar's overflow generation, which would have been the obvious
    // reuse: an overflow generation is EXTENDED by every write published while it is live, so on
    // an interleaved connection it would never end and no read would ever be served locally again.
    // This fence is fixed at arming and is never extended; writes published after arming take
    // ordinary ring slots underneath it and are exact the moment it lifts.
    static constexpr uint32_t kReadLocalUnarmed  = 1;
    static constexpr uint32_t kReadLocalArmFence = 2;

    // Cold half of the probe's arming gate. Returns true when this read must take the OWNER path:
    // the connection is carrying writes that no descriptor describes. The owner path preserves
    // per-key order by construction (a read and a write of one key share one owner queue), so
    // demoting here is exactly as strong as the ring would have been -- it just costs a task.
    __attribute__((noinline)) bool read_local_arm() {
        const uint64_t dispatch = dispatch_.load(std::memory_order_relaxed);
        const uint64_t flush = flush_id();
        if (read_local_arm_state_ == kReadLocalUnarmed) {
            if (read_local_arm_stats_) read_local_arm_stats_->arms++;
            if (read_local_id_active(read_local_unarmed_write_id_, dispatch, flush)) {
                // Writes are in flight and unrecorded: freeze the newest of them as the fence.
                read_local_arm_state_ = kReadLocalArmFence;
                return true;
            }
            read_local_unarmed_write_id_ = UINT64_MAX;
            read_local_arm_state_ = 0;
            return false;
        }
        if (read_local_id_active(read_local_unarmed_write_id_, dispatch, flush)) return true;
        read_local_unarmed_write_id_ = UINT64_MAX;
        read_local_arm_state_ = 0;
        return false;
    }

    ReadLocalRobState* read_local_state_ptr() const {
        return reinterpret_cast<ReadLocalRobState*>(
            read_local_state_ & ~kReadLocalStateTagBits);
    }
    // "resolve_read_local_write() has work": exactly mark_current_write()'s parked candidate.
    bool read_local_state_staged() const {
        return (read_local_state_ & kReadLocalStateStaged) != 0;
    }
    void read_local_state_activate() {
        if (!read_local_state_) std::abort();
        read_local_state_ &= ~kReadLocalStateInactive;
    }
    ReadLocalRobState& read_local_state_required() {
        ReadLocalRobState* state = read_local_state_ptr();
        if (!state) std::abort();
        return *state;
    }
    const ReadLocalRobState& read_local_state_required() const {
        const ReadLocalRobState* state = read_local_state_ptr();
        if (!state) std::abort();
        return *state;
    }
    // The Inactive tag means exactly "no WRITE hazard is live": the MGET fence moved to its own
    // Rob word and is tested by local_mget_fence_pending() without ever consulting the sidecar,
    // so it no longer has to hold the whole write-ring state active to be seen.
    void read_local_try_deactivate(const ReadLocalRobState& state) {
        if (!state.overflow && state.write_count == 0 &&
            state.pending_write == ReadLocalRobState::PendingWrite::None)
            read_local_state_ |= kReadLocalStateInactive;
    }

    // Slot half shared by both mark_current_read_local forms.
    __attribute__((always_inline)) void mark_current_read_local_slot(uint64_t op_id) {
        const uint64_t bit = uint64_t{1} << (static_cast<uint32_t>(op_id) & kMask);
        if (read_local_owner_slots_ & bit) std::abort();
        read_local_pending_slots_ |= bit;
    }

    void read_local_clear_mget_fence(uint64_t op_id) {
        if (local_mget_fence_id_ == op_id) local_mget_fence_id_ = UINT64_MAX;
    }

    static bool read_local_id_active(uint64_t op_id, uint64_t dispatch, uint64_t flush) {
        return op_id - flush < dispatch - flush;
    }

    void read_local_prune(ReadLocalRobState& state) {
        if (!state.overflow && state.write_count == 0) return;
        read_local_prune(state, dispatch_id(), flush_id());
    }

    // Retirement is the ring's only removal fence, and it is also the filter's: a slot leaves
    // valid_ in the same statement that leaves the FIFO, so the filter can never claim absence
    // for a descriptor the exact walk would still consult.
    void read_local_prune(ReadLocalRobState& state, uint64_t dispatch, uint64_t flush) {
        if (state.overflow) {
            if (read_local_id_active(state.overflow_through, dispatch, flush)) return;
            state.overflow = false;
            state.write_head = 0;
            state.write_count = 0;
            state.write_keyset_slots = 0;
            read_local_write_leave_overflow();
            return;
        }
        uint64_t retired_slots = 0;
        while (state.write_count &&
               !read_local_id_active(
                   state.write_ring[state.write_head].op_id, dispatch, flush)) {
            state.write_keyset_slots &= ~(uint64_t{1} << state.write_head);
            retired_slots |= uint64_t{1} << state.write_head;
            state.write_head = static_cast<uint8_t>(
                (state.write_head + 1) & (ReadLocalRobState::kWriteRingCapacity - 1));
            state.write_count--;
        }
        if (!state.write_count) {
            state.write_head = 0;
            state.write_keyset_slots = 0;
            read_local_write_reset_slots();      // overflow is false here, so force_ is already 0
            return;
        }
        // One pair of stores for the whole retired prefix rather than a pair per slot: the loop
        // above is the only producer of these bits and it pops a contiguous run.
        if (retired_slots) {
            read_local_write_valid_ &= ~retired_slots;
            read_local_write_wide_ &= ~retired_slots;
        }
    }

    // Drop every per-slot bit. The force flag is not a slot; the two helpers below own it, and it
    // deliberately keeps its own word so that clearing the slots can never disturb it.
    void read_local_write_reset_slots() {
        read_local_write_valid_ = 0;
        read_local_write_wide_ = 0;
    }

    // THE FILTER'S COMPLETENESS RULE: the force bit is set exactly while a conservative generation
    // is live. That generation is the one hazard with no ring slot and therefore no tag that could
    // reject it -- its member hashes were discarded when the ring overflowed. (The other slotless
    // hazard, a staged candidate, is read straight off the sidecar word by the probe shell, so it
    // never touches this bit.) The bit therefore moves ONLY where state.overflow itself moves: the
    // two entry points below and the one exit in read_local_prune, and nowhere else. An earlier
    // draft cleared it speculatively at the top of every resolve and re-set it per branch, which
    // let an abandoned write cancel a still-live generation and hand a read a false "no conflict"
    // (tests/read_local_write_ring_unit.cc case 8).
    void read_local_write_enter_overflow() {
        read_local_write_reset_slots();
        read_local_write_force_ = 1;
    }
    void read_local_write_leave_overflow() {
        read_local_write_reset_slots();
        read_local_write_force_ = 0;
    }

    // The ONLY consumer of a staged candidate, and therefore the only place the Staged tag is
    // cleared. Every early return below leaves pending_write == None, so tag and field stay in
    // lockstep on all paths (the invariant acquire_read_local's gate rests on).
    void read_local_resolve_pending(ReadLocalRobState& state) {
        read_local_state_ &= ~kReadLocalStateStaged;
        read_local_resolve_pending_body(state);
    }

    void read_local_resolve_pending_body(ReadLocalRobState& state) {
        using PendingWrite = ReadLocalRobState::PendingWrite;
        if (state.pending_write == PendingWrite::None) return;
        const uint64_t dispatch = dispatch_id();
        const uint64_t flush = flush_id();
        const uint64_t op_id = state.pending_op_id;
        const PendingWrite pending = state.pending_write;
        const uint64_t hash = state.pending_hash;
        state.pending_write = PendingWrite::None;

        // The op either never published / was unpublished, or published and already retired before
        // another frame on this connection could be considered. Neither can constrain that frame.
        if (!read_local_id_active(op_id, dispatch, flush)) return;

        read_local_prune(state, dispatch, flush);
        if (state.overflow || pending == PendingWrite::Overflow) {
            const bool entering = !state.overflow;
            state.overflow = true;
            state.overflow_through = op_id;
            state.write_head = 0;
            state.write_count = 0;
            state.write_keyset_slots = 0;
            if (entering) read_local_write_enter_overflow();
            return;
        }

        // The commit-side half of the same kept fallback. read_local_prune ran one statement ago
        // and the op being committed holds a window position that no ring entry can, so a full ring
        // is unreachable; a connection that somehow reached it still becomes a conservative
        // generation here and still fences every later read until that generation drains.
        if (state.write_count == ReadLocalRobState::kWriteRingCapacity) {
            state.overflow = true;
            state.overflow_through = op_id;
            state.write_head = 0;
            state.write_count = 0;
            state.write_keyset_slots = 0;
            read_local_write_enter_overflow();
            return;
        }
        const uint32_t tail =
            (static_cast<uint32_t>(state.write_head) + state.write_count) &
            (ReadLocalRobState::kWriteRingCapacity - 1);
        state.write_ring[tail] = ReadLocalRobState::WriteKey{hash, op_id};
        // THE bookkeeping this lane exists to remove from unarmed streams. Counted here, at the
        // single commit point, so "pure SET does zero ring bookkeeping" is a number in INFO and
        // not an argument about which branches ran.
        if (read_local_arm_stats_) read_local_arm_stats_->write_ring_records++;
        // The filter slot is written in the same breath as the ring slot. A Keyset descriptor
        // stores a key FILTER, not a hash, so tag equality cannot speak for it: it joins wide_
        // and every probe walks it, exactly as before this change.
        state.write_tags[tail] = static_cast<uint16_t>(hash);
        read_local_write_valid_ |= uint64_t{1} << tail;
        if (pending == PendingWrite::Keyset) {
            state.write_keyset_slots |= uint64_t{1} << tail;
            read_local_write_wide_ |= uint64_t{1} << tail;
        } else {
            state.write_keyset_slots &= ~(uint64_t{1} << tail);
            read_local_write_wide_ &= ~(uint64_t{1} << tail);
        }
        state.write_count++;
    }

    Op* chunks_[kChunks] = {};
    // Separate cache lines: the producer writes dispatch_ while the consumer writes flush_, and
    // sharing a line would make every publish invalidate the consumer's copy and vice versa.
    alignas(64) std::atomic<uint64_t> dispatch_{0};
    // CONTROL WORDS for the armed RYOW write ring; the tag mirror they select over lives in the
    // sidecar (ReadLocalRobState::write_tags), because a slot per ROB window position is 128 bytes
    // and this line has sixteen to spare. valid_ bit i says ring slot i holds a descriptor a probe
    // must consider; wide_ bit i says slot i cannot be rejected by tag equality (a precise-keyset
    // entry, whose stored word is a key filter, not a hash). force_ is not a slot: it forces the
    // exact path outright while a conservative generation is live, the one hazard with no ring slot
    // and therefore no tag that could reject it. All three stay in the padding dispatch_ already
    // owned, so the 192-byte lock is untouched and the parser that probes them owns that line
    // either way -- and a connection with nothing live is still answered from this line alone.
    uint64_t read_local_write_valid_ = 0;
    uint64_t read_local_write_wide_ = 0;
    uint32_t read_local_write_force_ = 0;
    // ARM-ON-DEMAND, and the MGET latest-read fence, in the same trailing padding. All four are
    // parser-owned, all four are read or written by the frame that has just stored dispatch_, and
    // together they fill this line exactly to its 64-byte boundary -- so the 192-byte Rob lock
    // below still holds and no connection grew a byte for any of it.
    uint32_t read_local_arm_state_ = kReadLocalUnarmed;
    // While unarmed: the newest write this connection has published, and the only thing recorded
    // about it. At arming it becomes the transient's fence and stops being written. UINT64_MAX is
    // "none", the same never-active sentinel the MGET fence uses.
    uint64_t read_local_unarmed_write_id_ = UINT64_MAX;
    uint64_t local_mget_fence_id_ = UINT64_MAX;
    ReadLocalArmStats* read_local_arm_stats_ = nullptr;
    alignas(64) std::atomic<uint64_t> flush_{0};
    // Venue-pending and owner-tail bitmaps distinguish work that a write must still demote from
    // work already sequenced on ordinary owner queues. The sidecar pointer's low alignment bit
    // means no write generation is active, so pure GETs do not dereference heap state. All three
    // words remain inside flush_'s established trailing padding.
    uint64_t read_local_pending_slots_ = 0;
    uint64_t read_local_owner_slots_ = 0;
    uintptr_t read_local_state_ = 0;
    // Superset of the pending reads' key hashes (see ReadLocalPendingFilter). It completes flush_'s
    // cache line: the parser that marks/probes it already owns that line for the bitmaps above.
    ReadLocalPendingFilter read_local_pending_filter_;

    // Removing a pending read never shrinks the filter (superset stays valid); an empty pending set
    // is the one point where the summary can be reset for free.
    void read_local_retire_pending_bit(uint64_t bit) {
        read_local_pending_slots_ &= ~bit;
        if (!read_local_pending_slots_) read_local_pending_filter_.clear();
    }
};

// The bitmaps, tagged sidecar pointer and pending-key filter deliberately occupy the pre-existing
// tail of flush_'s cache line. Locking the complete ROB keeps future accounting from silently
// growing every client.
static_assert(sizeof(Rob<64>) == 192, "Rob<64> layout changed");

}  // namespace tomo
