// BITFIELD / BITFIELD_RO -- one parsed operation list, one owner lookup, one replacement write.
// Textually included by xshard.cc to keep the latency-sensitive GET/SET translation unit stable.
// Bitmap growth still enters the exact exported string replacement/accounting funnel as SETBIT.

namespace tomo {
namespace {

inline constexpr uint64_t kBitfieldMaxBulkLen = 512ull * 1024 * 1024;

bool bitfield_parse_i64(Slice input, int64_t& output) {
    if (input.n == 0 || input.n >= 32) return false;
    uint32_t position = 0;
    bool negative = false;
    if (input.p[position] == '-') {
        negative = true;
        if (++position == input.n) return false;
    }
    if (input.p[position] == '0') {
        if (negative || position + 1 != input.n) return false;
        output = 0;
        return true;
    }
    if (input.p[position] < '1' || input.p[position] > '9') return false;
    uint64_t value = 0;
    const uint64_t limit = negative ? uint64_t{INT64_MAX} + 1 : uint64_t{INT64_MAX};
    for (; position < input.n; position++) {
        const unsigned char byte = static_cast<unsigned char>(input.p[position]);
        if (byte < '0' || byte > '9') return false;
        const uint32_t digit = byte - '0';
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (negative) {
        output = value == uint64_t{INT64_MAX} + 1
            ? INT64_MIN : -static_cast<int64_t>(value);
    } else {
        output = static_cast<int64_t>(value);
    }
    return true;
}

Slice bitfield_string_bytes(const KvObj* object, char (&integer)[24],
                            KvObjRawReadBuffer& raw) {
    if (!object->is_int()) return kvobj_string_value(object, raw);
    return Slice(integer, i64_to_dec(integer, object->int_value()));
}

void reply_bitfield_store_error(Op& op, XshardStringStoreResult result) {
    op.clear_reply();
    if (result == XshardStringStoreResult::Maxmemory) reply_maxmemory_oom(op);
    else if (result == XshardStringStoreResult::Oom)
        reply_err(op.sink(), "ERR out of memory");
    else reply_err(op.sink(), "ERR keyspace insert failed");
}

enum class BitfieldOpcode : uint8_t { Get, Set, Incrby };
enum class BitfieldOverflow : uint8_t { Wrap, Sat, Fail };

struct BitfieldOperation {
    uint64_t offset = 0;
    int64_t argument = 0;
    BitfieldOpcode opcode = BitfieldOpcode::Get;
    BitfieldOverflow overflow = BitfieldOverflow::Wrap;
    uint8_t bits = 0;
    bool is_signed = false;
};

class BitfieldOperations {
public:
    bool push(const BitfieldOperation& operation) {
        if (size_ < kInline) {
            inline_[size_++] = operation;
            return true;
        }
        try {
            spill_.push_back(operation);
        } catch (const std::bad_alloc&) {
            return false;
        }
        size_++;
        return true;
    }
    uint32_t size() const { return size_; }
    const BitfieldOperation& operator[](uint32_t index) const {
        return index < kInline ? inline_[index] : spill_[index - kInline];
    }

private:
    static constexpr uint32_t kInline = 16;
    BitfieldOperation inline_[kInline]{};
    std::vector<BitfieldOperation> spill_;
    uint32_t size_ = 0;
};

bool parse_bitfield_type(Op& op, Slice argument, bool& is_signed, uint8_t& bits) {
    if (argument.n < 2 || (argument.p[0] != 'i' && argument.p[0] != 'u')) {
        reply_err(op.sink(),
                  "ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.");
        return false;
    }
    int64_t parsed = 0;
    if (!bitfield_parse_i64(Slice(argument.p + 1, argument.n - 1), parsed) || parsed < 1 ||
        (argument.p[0] == 'i' ? parsed > 64 : parsed > 63)) {
        reply_err(op.sink(),
                  "ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.");
        return false;
    }
    is_signed = argument.p[0] == 'i';
    bits = static_cast<uint8_t>(parsed);
    return true;
}

bool parse_bitfield_offset(Op& op, Slice argument, uint8_t bits, uint64_t& offset) {
    const bool indexed = argument.n && argument.p[0] == '#';
    const Slice number(argument.p + indexed, argument.n - indexed);
    int64_t parsed = 0;
    if (!bitfield_parse_i64(number, parsed) || parsed < 0 ||
        (indexed && static_cast<uint64_t>(parsed) >
                        static_cast<uint64_t>(INT64_MAX) / bits)) {
        reply_err(op.sink(), "ERR bit offset is not an integer or out of range");
        return false;
    }
    uint64_t result = static_cast<uint64_t>(parsed);
    if (indexed) result *= bits;
    if ((result >> 3) >= kBitfieldMaxBulkLen) {
        reply_err(op.sink(), "ERR bit offset is not an integer or out of range");
        return false;
    }
    offset = result;
    return true;
}

uint64_t get_unsigned_bitfield(const uint8_t* bytes, uint64_t offset, uint8_t bits) {
    uint64_t value = 0;
    for (uint32_t index = 0; index < bits; index++, offset++) {
        const uint8_t bit = static_cast<uint8_t>(7 - (offset & 7));
        value = (value << 1) | ((bytes[offset >> 3] >> bit) & 1u);
    }
    return value;
}

int64_t unsigned_bits_to_signed(uint64_t value, uint8_t bits) {
    if (bits < 64 && (value & (uint64_t{1} << (bits - 1))))
        value |= UINT64_MAX << bits;
    int64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

int64_t get_signed_bitfield(const uint8_t* bytes, uint64_t offset, uint8_t bits) {
    return unsigned_bits_to_signed(get_unsigned_bitfield(bytes, offset, bits), bits);
}

void set_unsigned_bitfield(uint8_t* bytes, uint64_t offset, uint8_t bits, uint64_t value) {
    for (uint32_t index = 0; index < bits; index++, offset++) {
        const uint8_t bit = static_cast<uint8_t>(7 - (offset & 7));
        const uint8_t mask = static_cast<uint8_t>(1u << bit);
        const bool selected = value & (uint64_t{1} << (bits - 1 - index));
        uint8_t& byte = bytes[offset >> 3];
        byte = selected ? static_cast<uint8_t>(byte | mask)
                        : static_cast<uint8_t>(byte & ~mask);
    }
}

uint64_t read_unsigned_bitfield(Slice value, uint64_t offset, uint8_t bits) {
    uint8_t window[9]{};
    const uint64_t byte = offset >> 3;
    for (uint32_t index = 0; index < sizeof(window) && byte + index < value.n; index++)
        window[index] = static_cast<uint8_t>(value.p[byte + index]);
    return get_unsigned_bitfield(window, offset - byte * 8, bits);
}

struct UnsignedBitfieldResult {
    uint64_t value = 0;
    bool overflow = false;
};

UnsignedBitfieldResult unsigned_bitfield_result(uint64_t value, int64_t increment, uint8_t bits,
                                                BitfieldOverflow overflow) {
    const uint64_t maximum = (uint64_t{1} << bits) - 1;
    const __int128 sum = static_cast<__int128>(value) + increment;
    if (sum >= 0 && sum <= maximum) return {static_cast<uint64_t>(sum), false};
    if (overflow == BitfieldOverflow::Sat) return {sum < 0 ? 0 : maximum, true};
    return {static_cast<uint64_t>(sum) & maximum, true};
}

struct SignedBitfieldResult {
    int64_t value = 0;
    bool overflow = false;
};

SignedBitfieldResult signed_bitfield_result(int64_t value, int64_t increment, uint8_t bits,
                                            BitfieldOverflow overflow) {
    const __int128 maximum = bits == 64 ? INT64_MAX : (__int128{1} << (bits - 1)) - 1;
    const __int128 minimum = -maximum - 1;
    const __int128 sum = static_cast<__int128>(value) + increment;
    if (sum >= minimum && sum <= maximum) return {static_cast<int64_t>(sum), false};
    if (overflow == BitfieldOverflow::Sat)
        return {static_cast<int64_t>(sum < minimum ? minimum : maximum), true};
    uint64_t wrapped = static_cast<uint64_t>(sum);
    if (bits < 64) wrapped &= (uint64_t{1} << bits) - 1;
    return {unsigned_bits_to_signed(wrapped, bits), true};
}

bool parse_bitfield_operations(Op& op, BitfieldOperations& operations, bool& readonly,
                               uint64_t& highest_write_offset) {
    BitfieldOverflow overflow = BitfieldOverflow::Wrap;
    readonly = true;
    highest_write_offset = 0;
    for (uint32_t index = 2; index < op.argc();) {
        BitfieldOperation operation;
        operation.overflow = overflow;
        uint32_t width = 0;
        if (op.arg(index).eq_icase("get") && op.argc() - index >= 3) {
            operation.opcode = BitfieldOpcode::Get;
            width = 3;
        } else if (op.arg(index).eq_icase("set") && op.argc() - index >= 4) {
            operation.opcode = BitfieldOpcode::Set;
            width = 4;
        } else if (op.arg(index).eq_icase("incrby") && op.argc() - index >= 4) {
            operation.opcode = BitfieldOpcode::Incrby;
            width = 4;
        } else if (op.arg(index).eq_icase("overflow") && op.argc() - index >= 2) {
            if (op.arg(index + 1).eq_icase("wrap")) overflow = BitfieldOverflow::Wrap;
            else if (op.arg(index + 1).eq_icase("sat")) overflow = BitfieldOverflow::Sat;
            else if (op.arg(index + 1).eq_icase("fail")) overflow = BitfieldOverflow::Fail;
            else {
                reply_err(op.sink(), "ERR Invalid OVERFLOW type specified");
                return false;
            }
            index += 2;
            continue;
        } else {
            reply_syntax(op.sink());
            return false;
        }

        if (!parse_bitfield_type(op, op.arg(index + 1), operation.is_signed, operation.bits) ||
            !parse_bitfield_offset(op, op.arg(index + 2), operation.bits, operation.offset))
            return false;
        if (operation.opcode != BitfieldOpcode::Get) {
            if (!bitfield_parse_i64(op.arg(index + 3), operation.argument)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range");
                return false;
            }
            readonly = false;
            highest_write_offset = std::max<uint64_t>(
                highest_write_offset, operation.offset + operation.bits - 1);
        }
        if (!operations.push(operation)) {
            reply_err(op.sink(), "ERR out of memory");
            return false;
        }
        index += width;
    }
    return true;
}

template <bool kNotify>
void bitfield_generic(Shard& shard, Op& op, bool ro_variant) {
    BitfieldOperations operations;
    bool readonly = true;
    uint64_t highest_write_offset = 0;
    if (!parse_bitfield_operations(op, operations, readonly, highest_write_offset)) return;
    if (ro_variant && !readonly) {
        reply_err(op.sink(), "ERR BITFIELD_RO only supports the GET subcommand");
        return;
    }

    KvObj* object = readonly ? shard.store_find_read<kNotify>(op.hash, op.key())
                             : shard.store_find<kNotify>(op.hash, op.key());
    if (object && !obj_type_check(object, Type::String, op.sink())) return;
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice old = object ? bitfield_string_bytes(object, integer, raw) : Slice("", 0);

    if (readonly) {
        reply_array_header(op.sink(), operations.size());
        for (uint32_t index = 0; index < operations.size(); index++) {
            const BitfieldOperation& operation = operations[index];
            const uint64_t value = read_unsigned_bitfield(old, operation.offset, operation.bits);
            reply_int(op.sink(), operation.is_signed
                ? unsigned_bits_to_signed(value, operation.bits)
                : static_cast<int64_t>(value));
        }
        return;
    }

    const uint32_t new_length = static_cast<uint32_t>(
        std::max<uint64_t>(old.n, (highest_write_offset >> 3) + 1));
    uint8_t* image = static_cast<uint8_t*>(std::malloc(new_length));
    if (!image) { reply_err(op.sink(), "ERR out of memory"); return; }
    if (old.n) std::memcpy(image, old.p, old.n);
    if (new_length > old.n) std::memset(image + old.n, 0, new_length - old.n);

    reply_array_header(op.sink(), operations.size());
    uint32_t changes = 0;
    const bool grew = new_length != old.n;
    for (uint32_t index = 0; index < operations.size(); index++) {
        const BitfieldOperation& operation = operations[index];
        if (operation.opcode == BitfieldOpcode::Get) {
            const Slice current(reinterpret_cast<const char*>(image), new_length);
            const uint64_t raw = read_unsigned_bitfield(current, operation.offset, operation.bits);
            reply_int(op.sink(), operation.is_signed
                ? unsigned_bits_to_signed(raw, operation.bits)
                : static_cast<int64_t>(raw));
            continue;
        }

        if (operation.is_signed) {
            const int64_t old_value = get_signed_bitfield(
                image, operation.offset, operation.bits);
            const int64_t base = operation.opcode == BitfieldOpcode::Set
                ? operation.argument : old_value;
            const int64_t increment = operation.opcode == BitfieldOpcode::Set
                ? 0 : operation.argument;
            const SignedBitfieldResult result = signed_bitfield_result(
                base, increment, operation.bits, operation.overflow);
            if (result.overflow && operation.overflow == BitfieldOverflow::Fail) {
                reply_nil(op.sink());
                continue;
            }
            reply_int(op.sink(), operation.opcode == BitfieldOpcode::Set
                ? old_value : result.value);
            set_unsigned_bitfield(image, operation.offset, operation.bits,
                                  static_cast<uint64_t>(result.value));
            if (grew || old_value != result.value) changes++;
        } else {
            const uint64_t old_value = get_unsigned_bitfield(
                image, operation.offset, operation.bits);
            const uint64_t base = operation.opcode == BitfieldOpcode::Set
                ? static_cast<uint64_t>(operation.argument) : old_value;
            const int64_t increment = operation.opcode == BitfieldOpcode::Set
                ? 0 : operation.argument;
            const UnsignedBitfieldResult result = unsigned_bitfield_result(
                base, increment, operation.bits, operation.overflow);
            if (result.overflow && operation.overflow == BitfieldOverflow::Fail) {
                reply_nil(op.sink());
                continue;
            }
            reply_int(op.sink(), operation.opcode == BitfieldOpcode::Set
                ? static_cast<int64_t>(old_value) : static_cast<int64_t>(result.value));
            set_unsigned_bitfield(image, operation.offset, operation.bits, result.value);
            if (grew || old_value != result.value) changes++;
        }
    }

    const int64_t expire_at_ms = object ? shard.store().deadline(op.hash, object) : -1;
    const Slice replacement(reinterpret_cast<const char*>(image), new_length);
    const XshardStringStoreResult stored = kNotify
        ? xshard_store_string_notify(shard, op.key(), op.hash, replacement,
                                     expire_at_ms, false,
                                     object && object->has_ttl_slot())
        : xshard_store_string(shard, op.key(), op.hash, replacement, expire_at_ms, false,
                              object && object->has_ttl_slot());
    std::free(image);
    if (stored != XshardStringStoreResult::Stored) {
        reply_bitfield_store_error(op, stored);
        return;
    }
    if constexpr (kNotify) if (changes)
        notify_record(shard, op, NOTIFY_STRING, NotifyEventId::Setbit, op.key());
}

void cmd_bitfield_notify_body(Shard& shard, Op& op) { bitfield_generic<true>(shard, op, false); }
void cmd_bitfield_ro_notify_body(Shard& shard, Op& op) { bitfield_generic<true>(shard, op, true); }

}  // namespace

void cmd_bitfield(Shard& shard, Op& op) { bitfield_generic<false>(shard, op, false); }

void cmd_bitfield_notify(Shard& shard, Op& op) {
    notify_execute_handler(shard, op, cmd_bitfield_notify_body);
}

void cmd_bitfield_ro(Shard& shard, Op& op) { bitfield_generic<false>(shard, op, true); }

void cmd_bitfield_ro_notify(Shard& shard, Op& op) {
    notify_execute_handler(shard, op, cmd_bitfield_ro_notify_body);
}

}  // namespace tomo
