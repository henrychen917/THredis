// PFDEBUG -- owner-local inspection and conversion of Redis-compatible HLL string images.
//
// The command is deliberately isolated from the ordinary PFADD/PFCOUNT path. It is an internal,
// explicitly-invoked debugging surface: ENCODING and DECODE inspect one HLL, while GETREG and
// TODENSE may rewrite that same owner's sparse image as dense. No cross-shard coordination or
// connection state is involved.
#include "command.h"
#include "hll.h"
#include "xshard.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../store/kvobj.h"

#include <array>
#include <cstring>
#include <new>
#include <string>

namespace tomo {
namespace {

bool eq_icase(Slice value, const char* literal) {
    const size_t length = std::strlen(literal);
    if (value.n != length) return false;
    for (size_t i = 0; i < length; i++) {
        uint8_t a = static_cast<uint8_t>(value.p[i]);
        uint8_t b = static_cast<uint8_t>(literal[i]);
        if (a >= 'a' && a <= 'z') a = static_cast<uint8_t>(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

void reply_bad_header(Op& op) {
    reply_err(op.sink(), "WRONGTYPE Key is not a valid HyperLogLog string value.");
}

void reply_corrupt(Op& op) {
    reply_err(op.sink(), "INVALIDOBJ Corrupted HLL object detected");
}

template <bool kNotify>
bool load_hll(Shard& shard, Op& op, KvObj*& object, Slice& image) {
    object = shard.store_find<kNotify>(op.hash, op.arg(2));
    if (!object) {
        reply_err(op.sink(), "ERR The specified key does not exist");
        return false;
    }
    if (static_cast<Type>(object->type) != Type::String) {
        reply_wrongtype(op.sink());
        return false;
    }
    if (object->is_int()) {
        reply_bad_header(op);
        return false;
    }
    image = object->str_value();
    if (!hll::header_valid(image)) {
        reply_bad_header(op);
        return false;
    }
    return true;
}

template <bool kNotify>
bool store_dense(Shard& shard, Op& op, KvObj* object, const std::string& image) {
    const XshardStringStoreResult result = kNotify
        ? xshard_store_string_notify(shard, op.arg(2), op.hash,
                                     Slice(image.data(), static_cast<uint32_t>(image.size())),
                                     object->expire_at_ms(), false)
        : xshard_store_string(shard, op.arg(2), op.hash,
                             Slice(image.data(), static_cast<uint32_t>(image.size())),
                             object->expire_at_ms(), false);
    switch (result) {
        case XshardStringStoreResult::Stored:
            return true;
        case XshardStringStoreResult::Oom:
            reply_err(op.sink(), "ERR out of memory");
            return false;
        case XshardStringStoreResult::InsertFailed:
            reply_err(op.sink(), "ERR keyspace insert failed");
            return false;
        case XshardStringStoreResult::Maxmemory:
            reply_maxmemory_oom(op);
            return false;
    }
    reply_err(op.sink(), "ERR keyspace insert failed");
    return false;
}

template <bool kNotify>
void cmd_pfdebug_impl(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    Slice image;
    if (!load_hll<kNotify>(shard, op, object, image)) return;

    const Slice subcommand = op.arg(1);
    if (eq_icase(subcommand, "ENCODING")) {
        reply_simple(op.sink(), hll::is_dense(image) ? "dense" : "sparse");
        return;
    }
    if (eq_icase(subcommand, "DECODE")) {
        if (hll::is_dense(image)) {
            reply_err(op.sink(), "ERR HLL encoding is not sparse");
            return;
        }
        try {
            std::string decoded;
            if (!hll::decode_sparse(image, decoded)) {
                reply_corrupt(op);
                return;
            }
            reply_bulk(op.sink(), Slice(decoded.data(), static_cast<uint32_t>(decoded.size())));
        } catch (const std::bad_alloc&) {
            reply_err(op.sink(), "ERR out of memory");
        }
        return;
    }
    if (eq_icase(subcommand, "GETREG")) {
        try {
            std::string dense;
            Slice register_image = image;
            const bool convert = !hll::is_dense(image);
            if (convert) {
                dense.assign(image.p, image.n);
                if (!hll::make_dense(dense)) {
                    reply_corrupt(op);
                    return;
                }
                register_image = Slice(dense.data(), static_cast<uint32_t>(dense.size()));
            }
            std::array<uint8_t, hll::kRegisters> registers{};
            if (!hll::merge_registers(register_image, registers)) {
                reply_corrupt(op);
                return;
            }
            if (convert && !store_dense<kNotify>(shard, op, object, dense)) return;
            auto sink = op.sink();
            reply_array_header(sink, registers.size());
            for (uint8_t value : registers) reply_int(sink, value);
        } catch (const std::bad_alloc&) {
            reply_err(op.sink(), "ERR out of memory");
        }
        return;
    }
    if (eq_icase(subcommand, "TODENSE")) {
        if (hll::is_dense(image)) {
            reply_int(op.sink(), 0);
            return;
        }
        try {
            std::string dense(image.p, image.n);
            if (!hll::make_dense(dense)) {
                reply_corrupt(op);
                return;
            }
            if (!store_dense<kNotify>(shard, op, object, dense)) return;
            reply_int(op.sink(), 1);
        } catch (const std::bad_alloc&) {
            reply_err(op.sink(), "ERR out of memory");
        }
        return;
    }

    try {
        std::string message = "ERR Unknown PFDEBUG subcommand '";
        message.append(subcommand.p, subcommand.n);
        message.push_back('\'');
        reply_err(op.sink(), message.c_str());
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
    }
}

void cmd_pfdebug(Shard& shard, Op& op) { cmd_pfdebug_impl<false>(shard, op); }
void cmd_pfdebug_notify(Shard& shard, Op& op) { cmd_pfdebug_impl<true>(shard, op); }

static const CommandSpec kTable[] = {
    // name       min max flags                                      handler first last step notify
    {"PFDEBUG",   3,  3, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::Admin,
                                                    cmd_pfdebug, 2, 2, 1, cmd_pfdebug_notify},
};

}  // namespace

CommandTable pfdebug_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo
