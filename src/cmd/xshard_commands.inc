// xshard_commands.inc — cross-shard classification, computation, and reply assembly.
//
// This file is textually included by xshard.cc. It deliberately remains part of that one
// translation unit and shares its anonymous namespace; there are no new external symbols or
// build-system inputs.

namespace tomo {
void reply_maxmemory_oom(Op& op);  // shared canonical spelling from t_string.cc
namespace {

bool name_is(const Op& op, const char* name) { return std::strcmp(op.spec->name, name) == 0; }

bool parse_u64(Slice s, uint64_t& value) {
    if (!s.n) return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        const uint8_t c = static_cast<uint8_t>(s.p[i]);
        if (c < '0' || c > '9') return false;
        if (v > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) return false;
        v = v * 10 + (c - '0');
    }
    value = v;
    return true;
}

bool parse_i64(Slice s, int64_t& out) {
    if (!s.n) return false;
    uint32_t pos = 0;
    bool negative = false;
    if (s.p[pos] == '-') {
        negative = true;
        if (++pos == s.n) return false;
    }
    // Canonical decimal, as redis's string2ll: no leading '+', no leading zeroes, no negative
    // zero. "SINTERCARD 05 key" and "ZINTERCARD 1 key LIMIT 05" were accepted before this.
    if (s.p[pos] == '0') {
        if (negative || pos + 1 != s.n) return false;
        out = 0;
        return true;
    }
    if (s.p[pos] < '1' || s.p[pos] > '9') return false;
    uint64_t value = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63)
                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    for (; pos < s.n; pos++) {
        const uint8_t c = static_cast<uint8_t>(s.p[pos]);
        if (c < '0' || c > '9') return false;
        const uint32_t digit = c - '0';
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? (value == (uint64_t{1} << 63)
                          ? std::numeric_limits<int64_t>::min()
                          : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

void reply_invalid_integer(Op& op);

// A WEIGHTS operand is a double VALUE, so it takes the getDoubleFromObject grammar: "0x10" is a
// weight of 16 and "1e309" is an error. See src/base/numeric.h.
bool parse_weight(Slice s, double& out) {
    return parse_double_strict(s, out);
}

bool parse_zset_operation(Op& op, Kind kind, uint32_t& count,
                          std::vector<double>& weights, ZAggregate& aggregate,
                          bool& withscores, uint64_t& limit) {
    const bool store = is_store_zsetop(kind);
    const uint32_t num_arg = store ? 2 : 1;
    int64_t parsed_count = 0;
    if (!parse_i64(op.arg(num_arg), parsed_count)) {
        reply_invalid_integer(op);
        return false;
    }
    if (parsed_count < 1) {
        if (kind == Kind::Zunion) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zunion' command");
        else if (kind == Kind::Zinter) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zinter' command");
        else if (kind == Kind::Zdiff) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zdiff' command");
        else if (kind == Kind::Zunionstore) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zunionstore' command");
        else if (kind == Kind::Zinterstore) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zinterstore' command");
        else if (kind == Kind::Zdiffstore) reply_err(op.sink(), "ERR at least 1 input key is needed for 'zdiffstore' command");
        else reply_err(op.sink(), "ERR at least 1 input key is needed for 'zintercard' command");
        return false;
    }
    if (static_cast<uint64_t>(parsed_count) > UINT32_MAX) {
        reply_syntax(op.sink());
        return false;
    }
    count = static_cast<uint32_t>(parsed_count);
    const uint64_t tail64 = static_cast<uint64_t>(num_arg) + 1 + count;
    if (tail64 > op.argc()) {
        reply_syntax(op.sink());
        return false;
    }
    uint32_t arg = static_cast<uint32_t>(tail64);
    aggregate = ZAggregate::Sum;
    withscores = false;
    limit = 0;
    try { weights.assign(count, 1.0); }
    catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return false; }

    if (kind == Kind::Zintercard) {
        if (arg == op.argc()) return true;
        int64_t parsed_limit = 0;
        if (arg + 2 != op.argc() || !op.arg(arg).eq_icase("limit") ||
            !parse_i64(op.arg(arg + 1), parsed_limit) || parsed_limit < 0) {
            if (arg < op.argc() && op.arg(arg).eq_icase("limit") && arg + 1 < op.argc())
                reply_err(op.sink(), "ERR LIMIT can't be negative");
            else
                reply_syntax(op.sink());
            return false;
        }
        limit = static_cast<uint64_t>(parsed_limit);
        return true;
    }

    const bool difference = kind == Kind::Zdiff || kind == Kind::Zdiffstore;
    while (arg < op.argc()) {
        if (!difference && op.arg(arg).eq_icase("weights") && arg + count < op.argc()) {
            for (uint32_t i = 0; i < count; i++) {
                if (!parse_weight(op.arg(arg + 1 + i), weights[i])) {
                    reply_err(op.sink(), "ERR weight value is not a float");
                    return false;
                }
            }
            arg += count + 1;
        } else if (!difference && op.arg(arg).eq_icase("aggregate") && arg + 1 < op.argc()) {
            const Slice mode = op.arg(arg + 1);
            if (mode.eq_icase("sum")) aggregate = ZAggregate::Sum;
            else if (mode.eq_icase("min")) aggregate = ZAggregate::Min;
            else if (mode.eq_icase("max")) aggregate = ZAggregate::Max;
            else { reply_syntax(op.sink()); return false; }
            arg += 2;
        } else if (!store && op.arg(arg).eq_icase("withscores")) {
            withscores = true;
            arg++;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    return true;
}

void reply_invalid_integer(Op& op) {
    reply_err(op.sink(), "ERR value is not an integer or out of range");
}

bool parse_mpop(Op& op, Kind kind, uint32_t& numkeys, bool& edge, uint64_t& count) {
    int64_t parsed_numkeys = 0;
    if (!parse_i64(op.arg(1), parsed_numkeys) || parsed_numkeys < 1) {
        reply_err(op.sink(), "ERR numkeys should be greater than 0");
        return false;
    }
    if (static_cast<uint64_t>(parsed_numkeys) > UINT32_MAX ||
        static_cast<uint64_t>(parsed_numkeys) + 2 >= op.argc()) {
        reply_syntax(op.sink());
        return false;
    }
    numkeys = static_cast<uint32_t>(parsed_numkeys);
    const uint32_t edge_arg = 2 + numkeys;
    if (kind == Kind::Lmpop) {
        if (op.arg(edge_arg).eq_icase("left")) edge = false;
        else if (op.arg(edge_arg).eq_icase("right")) edge = true;
        else { reply_syntax(op.sink()); return false; }
    } else {
        if (op.arg(edge_arg).eq_icase("min")) edge = false;
        else if (op.arg(edge_arg).eq_icase("max")) edge = true;
        else { reply_syntax(op.sink()); return false; }
    }
    count = 1;
    bool count_seen = false;
    for (uint32_t i = edge_arg + 1; i < op.argc(); i++) {
        if (count_seen || !op.arg(i).eq_icase("count") || i + 1 >= op.argc()) {
            reply_syntax(op.sink());
            return false;
        }
        int64_t parsed_count = 0;
        if (!parse_i64(op.arg(++i), parsed_count)) {
            reply_invalid_integer(op);
            return false;
        }
        if (parsed_count < 1) {
            reply_err(op.sink(), "ERR count should be greater than 0");
            return false;
        }
        count = static_cast<uint64_t>(parsed_count);
        count_seen = true;
    }
    return true;
}

bool parse_zrangestore_options(Op& op, ImageRangeOptions& options) {
    bool kind_seen = false;
    bool reverse_seen = false;
    for (uint32_t i = 5; i < op.argc(); i++) {
        if (!kind_seen && op.arg(i).eq_icase("byscore")) {
            options.kind = ImageRangeKind::Score;
            kind_seen = true;
        } else if (!kind_seen && op.arg(i).eq_icase("bylex")) {
            options.kind = ImageRangeKind::Lex;
            kind_seen = true;
        } else if (!reverse_seen && op.arg(i).eq_icase("rev")) {
            options.reverse = true;
            reverse_seen = true;
        } else if (op.arg(i).eq_icase("limit") && i + 2 < op.argc()) {
            if (!parse_i64(op.arg(i + 1), options.offset) ||
                !parse_i64(op.arg(i + 2), options.limit)) {
                reply_invalid_integer(op);
                return false;
            }
            options.limit_seen = true;
            i += 2;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    // Redis rejects LIMIT on an index range only when the COUNT actually bounds the answer: its
    // guard tests the parsed count against the -1 default and ignores the offset entirely, so
    // "ZRANGE k 0 -1 LIMIT 0 -1" (and any offset with count -1) is accepted and the LIMIT is a
    // no-op. Client libraries that always emit LIMIT depend on that.
    if (options.limit_seen && options.limit != -1 && options.kind == ImageRangeKind::Rank) {
        reply_err(op.sink(),
                  "ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
        return false;
    }
    return true;
}

bool parse_sort_options(Op& op, SortOptions& options) {
    for (uint32_t i = 2; i < op.argc(); i++) {
        if (op.arg(i).eq_icase("alpha")) {
            options.alpha = true;
        } else if (op.arg(i).eq_icase("asc")) {
            options.descending = false;
        } else if (op.arg(i).eq_icase("desc")) {
            options.descending = true;
        } else if (op.arg(i).eq_icase("limit") && i + 2 < op.argc()) {
            if (!parse_i64(op.arg(i + 1), options.offset) ||
                !parse_i64(op.arg(i + 2), options.count)) {
                reply_invalid_integer(op);
                return false;
            }
            i += 2;
        } else if (op.arg(i).eq_icase("by") && i + 1 < op.argc()) {
            options.by_arg = ++i;
            // A BY pattern with no '*' dereferences nothing -- it only suppresses ordering.
            if (std::memchr(op.arg(options.by_arg).p, '*', op.arg(options.by_arg).n)) {
                options.deref = true;
            }
        } else if (op.arg(i).eq_icase("get") && i + 1 < op.argc()) {
            const uint32_t arg = ++i;
            // `#` is the element itself and a pattern with no '*' is a constant nil, so neither
            // reads a key. Only a '*'-bearing pattern needs the derived-key gather.
            const Slice pattern = op.arg(arg);
            const bool reads_a_key = !(pattern.n == 1 && pattern.p[0] == '#') &&
                                     std::memchr(pattern.p, '*', pattern.n) != nullptr;
            if (reads_a_key) {
                options.deref = true;
            }
            try {
                options.get_args.push_back(arg);
            } catch (const std::bad_alloc&) {
                set_oom(op);
                return false;
            }
        } else if (op.arg(i).eq_icase("store") && i + 1 < op.argc()) {
            // SORT_RO is the read-only alias: it shares every option EXCEPT STORE, which would
            // make it a write. Redis reports the rejection as a plain syntax error.
            if (name_is(op, "SORT_RO")) { reply_syntax(op.sink()); return false; }
            options.store = true;
            options.store_arg = ++i;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    return true;
}

void set_oom(Op& op) { reply_err(op.sink(), "ERR out of memory"); }

bool classify(const Op& op, Kind& kind) {
    struct Entry { const char* name; Kind kind; };
    static constexpr Entry entries[] = {
        {"MGET", Kind::Mget}, {"MSET", Kind::Mset}, {"DEL", Kind::Del},
        {"UNLINK", Kind::Unlink}, {"EXISTS", Kind::Exists}, {"TOUCH", Kind::Touch},
        {"KEYS", Kind::Keys}, {"PFCOUNT", Kind::Pfcount}, {"MSETNX", Kind::Msetnx},
        {"PFMERGE", Kind::Pfmerge}, {"RENAME", Kind::Rename},
        {"RENAMENX", Kind::Renamenx}, {"COPY", Kind::Copy}, {"SMOVE", Kind::Smove},
        {"LMOVE", Kind::Lmove}, {"BLMOVE", Kind::Lmove},
        {"RPOPLPUSH", Kind::Rpoplpush}, {"BRPOPLPUSH", Kind::Rpoplpush},
        {"SINTER", Kind::Sinter}, {"SUNION", Kind::Sunion}, {"SDIFF", Kind::Sdiff},
        {"SINTERCARD", Kind::Sintercard}, {"SINTERSTORE", Kind::Sinterstore},
        {"SUNIONSTORE", Kind::Sunionstore}, {"SDIFFSTORE", Kind::Sdiffstore},
        {"ZUNION", Kind::Zunion}, {"ZINTER", Kind::Zinter}, {"ZDIFF", Kind::Zdiff},
        {"ZINTERCARD", Kind::Zintercard}, {"ZUNIONSTORE", Kind::Zunionstore},
        {"ZINTERSTORE", Kind::Zinterstore}, {"ZDIFFSTORE", Kind::Zdiffstore},
        {"BITOP", Kind::Bitop},
        {"LMPOP", Kind::Lmpop}, {"ZMPOP", Kind::Zmpop},
        {"ZRANGESTORE", Kind::Zrangestore}, {"SORT", Kind::Sort},
        {"GEOSEARCHSTORE", Kind::Geosearchstore},
        {"GEORADIUS", Kind::Georadius}, {"GEORADIUS_RO", Kind::Georadius},
        {"GEORADIUSBYMEMBER", Kind::Georadiusbymember},
        {"GEORADIUSBYMEMBER_RO", Kind::Georadiusbymember},
        {"SORT_RO", Kind::Sort}, {"LCS", Kind::Lcs},
        {"XREAD", Kind::Xread}, {"XREADGROUP", Kind::Xreadgroup},
        {"EVAL", Kind::Script}, {"EVALSHA", Kind::Script},
        {"EVAL_RO", Kind::Script}, {"EVALSHA_RO", Kind::Script},
        {"FCALL", Kind::Script}, {"FCALL_RO", Kind::Script},
    };
    for (const Entry& entry : entries)
        if (name_is(op, entry.name)) { kind = entry.kind; return true; }
    return false;
}

bool append_key_span(const Op& op, XshardKeyPlan& keys, uint32_t first,
                     uint32_t count, uint32_t step, XshardKeyUse use) {
    if (!count) return true;
    if (!step || keys.span_count >= 2 ||
        count > UINT32_MAX - keys.key_count) return false;
    const uint64_t last = static_cast<uint64_t>(first) +
                          static_cast<uint64_t>(count - 1) * step;
    if (last >= op.argc()) return false;
    keys.spans[keys.span_count++] = XshardKeySpan{first, count, step, use};
    keys.key_count += count;
    return true;
}

bool classify_keys(const Op& op, Kind kind, const XshardKeyHints& hints,
                   XshardKeyPlan& keys) {
    keys = XshardKeyPlan{};
    const auto add_read = [&](uint32_t first, uint32_t count, uint32_t step = 1) {
        return append_key_span(op, keys, first, count, step, XshardKeyUse::Read);
    };
    const auto add_write = [&](uint32_t first, uint32_t count, uint32_t step = 1) {
        return append_key_span(op, keys, first, count, step, XshardKeyUse::SnapshotWrite);
    };
    const uint32_t after_command = op.argc() > 1 ? op.argc() - 1 : 0;

    switch (kind) {
        case Kind::AllShards:
        case Kind::DebugReload:
        case Kind::DebugLoadAof:
        case Kind::DebugBorrowCount:
        case Kind::DbsizeExact:
        case Kind::Keys:
            return true;
        case Kind::Mget:
        case Kind::Exists:
        case Kind::Touch:
        case Kind::Pfcount:
        case Kind::Sinter:
        case Kind::Sunion:
        case Kind::Sdiff:
            return add_read(1, after_command);
        case Kind::Mset:
        case Kind::Msetnx:
            return add_write(1, after_command / 2, 2);
        case Kind::Del:
        case Kind::Unlink:
            return add_write(1, after_command);
        // Option-hidden stream positions live here once for routing, the own-write fence, and
        // snapshot COW. Keeping XREADGROUP writable is the S2 invariant.
        case Kind::Xread:
            return add_read(hints.first, hints.count);
        case Kind::Xreadgroup:
            return add_write(hints.first, hints.count);
        case Kind::Lcs:
            return add_read(1, 2);
        case Kind::Rename:
        case Kind::Renamenx:
        case Kind::Smove:
        case Kind::Lmove:
        case Kind::Rpoplpush:
            return add_write(1, 2);
        case Kind::Copy:
            return add_read(1, 1) && add_write(2, 1);
        case Kind::Sintercard:
            return add_read(2, hints.count);
        case Kind::Sinterstore:
        case Kind::Sunionstore:
        case Kind::Sdiffstore:
            return add_write(1, 1) && add_read(2, op.argc() - 2);
        case Kind::Zunion:
        case Kind::Zinter:
        case Kind::Zdiff:
        case Kind::Zintercard:
            return add_read(2, hints.count);
        case Kind::Zunionstore:
        case Kind::Zinterstore:
        case Kind::Zdiffstore:
            return add_write(1, 1) && add_read(3, hints.count);
        case Kind::Bitop:
            return add_write(2, 1) && add_read(3, op.argc() - 3);
        case Kind::Pfmerge:
            return add_write(1, 1) && add_read(2, op.argc() - 2);
        case Kind::Lmpop:
        case Kind::Zmpop:
            return add_write(2, hints.count);
        case Kind::Zrangestore:
            return add_write(1, 1) && add_read(2, 1);
        case Kind::Sort:
            if (hints.store)
                return add_write(hints.destination, 1) && add_read(1, 1);
            return add_read(1, 1);
        case Kind::Geosearchstore:
        case Kind::Georadius:
        case Kind::Georadiusbymember:
            if (hints.store)
                return add_write(hints.destination, 1) && add_read(hints.source, 1);
            return add_read(hints.source, 1);
        case Kind::Script: {
            const bool needs_snapshot_cow =
                (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) != 0;
            return needs_snapshot_cow
                ? add_write(hints.first, hints.count)
                : add_read(hints.first, hints.count);
        }
    }
    return false;
}

// Owner-side localfast consumers reconstruct only the dynamic positions. xshard_prepare already
// validated the full grammar before marking an op local, so count-only families do not allocate
// the weights/result scratch their execution parser already built and discarded on IO.
bool classify_keys(Op& op, Kind kind, XshardKeyPlan& keys) {
    XshardKeyHints hints;
    if (kind == Kind::Sintercard) {
        int64_t count = 0;
        if (!parse_i64(op.arg(1), count) || count < 1 ||
            static_cast<uint64_t>(count) > UINT32_MAX) return false;
        hints.count = static_cast<uint32_t>(count);
    } else if (is_any_zsetop(kind)) {
        int64_t count = 0;
        const uint32_t count_arg = is_store_zsetop(kind) ? 2 : 1;
        if (!parse_i64(op.arg(count_arg), count) || count < 1 ||
            static_cast<uint64_t>(count) > UINT32_MAX) return false;
        hints.count = static_cast<uint32_t>(count);
    } else if (kind == Kind::Lmpop || kind == Kind::Zmpop) {
        int64_t count = 0;
        if (!parse_i64(op.arg(1), count) || count < 1 ||
            static_cast<uint64_t>(count) > UINT32_MAX) return false;
        hints.count = static_cast<uint32_t>(count);
    } else if (kind == Kind::Sort) {
        SortOptions options;
        if (!parse_sort_options(op, options)) return false;
        hints.store = options.store;
        hints.destination = options.store_arg;
    } else if (is_geo_kind(kind)) {
        GeoRoute route;
        if (!geo_prepare_route(op, route)) return false;
        hints.store = route.store;
        hints.source = route.source_arg;
        hints.destination = route.destination_arg;
    } else if (kind == Kind::Xread) {
        StreamXreadArgs parsed;
        if (!stream_parse_xread(op, parsed)) return false;
        hints.first = parsed.first_key;
        hints.count = parsed.key_count;
    } else if (kind == Kind::Xreadgroup) {
        StreamXreadGroupArgs parsed;
        if (!stream_parse_xreadgroup(op, parsed)) return false;
        hints.first = parsed.key_arg;
        hints.count = 1;
    } else if (kind == Kind::Script) {
        if (!command_script_key_range(op, hints.first, hints.count)) return false;
    }
    return classify_keys(static_cast<const Op&>(op), kind, hints, keys);
}


bool decode_elements(const ObjectImage& image, std::vector<std::string>& out) {
    out.clear();
    const uint8_t* p = image.payload.data();
    size_t left = image.payload.size();
    try {
        out.reserve(image.entries);
        while (left) {
            if (left < 4) return false;
            const uint32_t len = snapshot_get_u32(p);
            p += 4; left -= 4;
            if (left < len) return false;
            out.emplace_back(reinterpret_cast<const char*>(p), len);
            p += len; left -= len;
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool encode_elements(const std::vector<std::string>& elements, ObjectImage& image, Type type,
                     int64_t expire_at_ms, bool reserve_ttl_slot = false) {
    uint64_t total = 0;
    for (const std::string& element : elements) total += 4ull + element.size();
    if (total > UINT32_MAX) return false;
    try {
        image = ObjectImage{};
        image.present = true;
        image.type = type;
        image.encoding = 0;
        image.entries = static_cast<uint32_t>(elements.size());
        image.expire_at_ms = expire_at_ms;
        image.reserve_ttl_slot = reserve_ttl_slot;
        image.payload.resize(static_cast<size_t>(total));
    } catch (const std::bad_alloc&) {
        return false;
    }
    uint8_t* p = image.payload.data();
    for (const std::string& element : elements) {
        snapshot_put_u32(p, static_cast<uint32_t>(element.size())); p += 4;
        std::memcpy(p, element.data(), element.size()); p += element.size();
    }
    return true;
}

bool bitop_operation(Slice name, BitOperation& operation) {
    if (name.eq_icase("and")) operation = BitOperation::And;
    else if (name.eq_icase("or")) operation = BitOperation::Or;
    else if (name.eq_icase("xor")) operation = BitOperation::Xor;
    else if (name.eq_icase("not")) operation = BitOperation::Not;
    else return false;
    return true;
}

bool decode_string_image(const ObjectImage& image, std::string& value, WorkError& error) {
    value.clear();
    if (!image.present) return true;
    if (image.type != Type::String) { error = WorkError::WrongType; return false; }
    try {
        const Enc encoding = static_cast<Enc>(image.encoding);
        if (encoding == Enc::Int) {
            if (image.payload.size() != sizeof(int64_t)) {
                error = WorkError::Corrupt;
                return false;
            }
            char integer[24];
            const int64_t decoded = static_cast<int64_t>(snapshot_get_u64(image.payload.data()));
            value.assign(integer, i64_to_dec(integer, decoded));
        } else if (encoding == Enc::Raw || encoding == Enc::Extern) {
            if (!image.payload.empty())
                value.assign(reinterpret_cast<const char*>(image.payload.data()),
                             image.payload.size());
        } else {
            error = WorkError::Corrupt;
            return false;
        }
    } catch (const std::bad_alloc&) {
        error = WorkError::Oom;
        return false;
    }
    return true;
}

int binary_compare(Slice a, Slice b) {
    const uint32_t common = std::min(a.n, b.n);
    const int cmp = common ? std::memcmp(a.p, b.p, common) : 0;
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    if (a.n == b.n) return 0;
    return a.n < b.n ? -1 : 1;
}



bool parse_image_score_range(Slice min, Slice max, ImageScoreRange& range) {
    if (min.n && min.p[0] == '(') { range.min_exclusive = true; min.p++; min.n--; }
    if (max.n && max.p[0] == '(') { range.max_exclusive = true; max.p++; max.n--; }
    return parse_double_lenient(min, range.min) && parse_double_lenient(max, range.max);
}


bool parse_image_lex_bound(Slice input, ImageLexBound& bound) {
    if (input.n == 1 && input.p[0] == '-') {
        bound.infinity = ImageInfinity::Minus;
        bound.exclusive = true;
        return true;
    }
    if (input.n == 1 && input.p[0] == '+') {
        bound.infinity = ImageInfinity::Plus;
        bound.exclusive = true;
        return true;
    }
    if (!input.n || (input.p[0] != '(' && input.p[0] != '[')) return false;
    bound.exclusive = input.p[0] == '(';
    bound.value = Slice(input.p + 1, input.n - 1);
    return true;
}

int member_bound_compare(Slice member, const ImageLexBound& bound) {
    if (bound.infinity == ImageInfinity::Minus) return 1;
    if (bound.infinity == ImageInfinity::Plus) return -1;
    return binary_compare(member, bound.value);
}


bool parse_zrangestore_range(Op& op, const ImageRangeOptions& options, ImageRangeSpec& range) {
    uint32_t min_arg = 3, max_arg = 4;
    if (options.reverse && options.kind != ImageRangeKind::Rank) std::swap(min_arg, max_arg);
    if (options.kind == ImageRangeKind::Rank) {
        if (!parse_i64(op.arg(min_arg), range.rank_start) ||
            !parse_i64(op.arg(max_arg), range.rank_stop)) {
            reply_invalid_integer(op);
            return false;
        }
    } else if (options.kind == ImageRangeKind::Score) {
        if (!parse_image_score_range(op.arg(min_arg), op.arg(max_arg), range.score)) {
            reply_err(op.sink(), "ERR min or max is not a float");
            return false;
        }
    } else if (!parse_image_lex_bound(op.arg(min_arg), range.lex.min) ||
               !parse_image_lex_bound(op.arg(max_arg), range.lex.max)) {
        reply_err(op.sink(), "ERR min or max not valid string range item");
        return false;
    }
    return true;
}

struct ZImageItem {
    std::string member;
    double score = 0;
};

bool decode_zset_image(const ObjectImage& image, std::vector<ZImageItem>& items) {
    items.clear();
    const uint8_t* p = image.payload.data();
    uint64_t left = image.payload.size();
    try {
        items.reserve(image.entries);
        while (left) {
            if (left < 12) return false;
            const uint64_t bits = snapshot_get_u64(p);
            double score;
            std::memcpy(&score, &bits, sizeof(score));
            const uint32_t len = snapshot_get_u32(p + 8);
            p += 12; left -= 12;
            if (left < len || std::isnan(score)) return false;
            items.push_back({std::string(reinterpret_cast<const char*>(p), len), score});
            p += len; left -= len;
        }
    } catch (const std::bad_alloc&) {
        items.clear();
        return false;
    }
    return true;
}

bool compute_bitop(const ObjectImage* images, uint32_t first, uint32_t count, Slice operation_name,
                   ObjectImage& output, long long& output_length, WorkError& error) {
    BitOperation operation;
    if (!bitop_operation(operation_name, operation)) {
        error = WorkError::Corrupt;
        return false;
    }
    std::vector<std::string> sources;
    try {
        sources.resize(count);
        size_t max_length = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!decode_string_image(images[first + i], sources[i], error)) return false;
            max_length = std::max(max_length, sources[i].size());
        }
        output = ObjectImage{};
        output_length = static_cast<long long>(max_length);
        if (!max_length) return true;

        output.present = true;
        output.type = Type::String;
        output.encoding = static_cast<uint8_t>(Enc::Raw);
        output.expire_at_ms = -1;
        output.payload.resize(max_length);
        for (size_t byte = 0; byte < max_length; byte++) {
            uint8_t result = byte < sources[0].size()
                ? static_cast<uint8_t>(sources[0][byte]) : 0;
            if (operation == BitOperation::Not) {
                result = static_cast<uint8_t>(~result);
            } else {
                for (uint32_t source = 1; source < count; source++) {
                    const uint8_t next = byte < sources[source].size()
                        ? static_cast<uint8_t>(sources[source][byte]) : 0;
                    if (operation == BitOperation::And) result &= next;
                    else if (operation == BitOperation::Or) result |= next;
                    else result ^= next;
                }
            }
            output.payload[byte] = result;
        }
    } catch (const std::bad_alloc&) {
        error = WorkError::Oom;
        return false;
    }
    return true;
}

bool encode_zset_image(const std::vector<ZImageItem>& items, ObjectImage& image) {
    uint64_t total = 0;
    for (const ZImageItem& item : items) total += 12ull + item.member.size();
    if (total > UINT32_MAX || items.size() > UINT32_MAX) return false;
    try {
        image = ObjectImage{};
        image.present = true;
        image.type = Type::Zset;
        image.encoding = 0;
        image.entries = static_cast<uint32_t>(items.size());
        image.expire_at_ms = -1;
        image.payload.resize(static_cast<size_t>(total));
    } catch (const std::bad_alloc&) {
        return false;
    }
    uint8_t* p = image.payload.data();
    for (const ZImageItem& item : items) {
        uint64_t bits;
        std::memcpy(&bits, &item.score, sizeof(bits));
        snapshot_put_u64(p, bits);
        snapshot_put_u32(p + 8, static_cast<uint32_t>(item.member.size()));
        if (!item.member.empty()) std::memcpy(p + 12, item.member.data(), item.member.size());
        p += 12 + item.member.size();
    }
    return true;
}

// `expanded` is the SOURCE key's encoding (skiplist vs listpack): it selects how a negative LIMIT
// offset resolves, exactly as on the single-shard range path.
bool select_zrange(const std::vector<ZImageItem>& input, const ImageRangeOptions& options,
                   const ImageRangeSpec& range, bool expanded,
                   std::vector<ZImageItem>& selected) {
    selected.clear();
    try {
        if (options.kind == ImageRangeKind::Rank) {
            int64_t start = range.rank_start;
            int64_t stop = range.rank_stop;
            const int64_t size = static_cast<int64_t>(input.size());
            if (start < 0) start += size;
            if (stop < 0) stop += size;
            if (start < 0) start = 0;
            if (start > stop || start >= size || stop < 0) return true;
            if (stop >= size) stop = size - 1;
            selected.reserve(static_cast<size_t>(stop - start + 1));
            for (int64_t i = start; i <= stop; i++) {
                const size_t index = options.reverse ? static_cast<size_t>(size - i - 1)
                                                     : static_cast<size_t>(i);
                selected.push_back(input[index]);
            }
            return true;
        }

        if (options.limit == 0) return true;
        std::vector<size_t> matches;
        matches.reserve(input.size());
        bool started = false;
        for (size_t i = 0; i < input.size(); i++) {
            const Slice member(input[i].member.data(), static_cast<uint32_t>(input[i].member.size()));
            bool at_or_after = false;
            bool at_or_before = false;
            if (options.kind == ImageRangeKind::Score) {
                at_or_after = range.score.min_exclusive
                    ? input[i].score > range.score.min : input[i].score >= range.score.min;
                at_or_before = range.score.max_exclusive
                    ? input[i].score < range.score.max : input[i].score <= range.score.max;
            } else {
                const int lo = member_bound_compare(member, range.lex.min);
                const int hi = member_bound_compare(member, range.lex.max);
                at_or_after = range.lex.min.exclusive ? lo > 0 : lo >= 0;
                at_or_before = range.lex.max.exclusive ? hi < 0 : hi <= 0;
            }
            if (!started && !at_or_after) continue;
            if (!at_or_before) break;
            started = true;
            matches.push_back(i);
        }
        uint64_t offset = 0;
        if (!zset_resolve_limit_offset(options.offset, matches.size(), expanded, offset))
            return true;
        const uint64_t available = matches.size() - offset;
        const uint64_t take = options.limit < 0
            ? available : std::min<uint64_t>(available, static_cast<uint64_t>(options.limit));
        selected.reserve(static_cast<size_t>(take));
        for (uint64_t i = 0; i < take; i++) {
            const size_t match = options.reverse
                ? matches[matches.size() - 1 - static_cast<size_t>(offset + i)]
                : matches[static_cast<size_t>(offset + i)];
            selected.push_back(input[match]);
        }
    } catch (const std::bad_alloc&) {
        selected.clear();
        return false;
    }
    return true;
}

// Decode the source at its natural order and build the stable option spec. Dereferencing SORT keeps
// both objects in its arena sidecar across the owner-grouped gather wave; ordinary SORT immediately
// passes them to sort_image_finish below.
WorkError sort_image_prepare(const ObjectImage& image, const Op& op, const SortOptions& options,
                             SortSpec& spec, std::vector<std::string>& elements) {
    spec = SortSpec{};
    elements.clear();
    if (!image.present) return WorkError::None;
    if (image.type != Type::List && image.type != Type::Set && image.type != Type::Zset)
        return WorkError::WrongType;
    try {
        if (image.type == Type::Zset) {
            std::vector<ZImageItem> zitems;
            if (!decode_zset_image(image, zitems)) return WorkError::Corrupt;
            elements.reserve(zitems.size());
            for (ZImageItem& item : zitems) elements.push_back(std::move(item.member));
        } else if (!decode_elements(image, elements)) {
            return WorkError::Corrupt;
        }
        spec.alpha = options.alpha;
        spec.descending = options.descending;
        spec.store = options.store;
        spec.offset = options.offset;
        spec.count = options.count;
        spec.source_is_set = image.type == Type::Set;
        spec.source_is_list = image.type == Type::List;
        spec.by_given = options.by_arg != 0;
        if (spec.by_given) {
            sort_pattern_parse(op.arg(options.by_arg), spec.by);
            spec.dontsort = !spec.by.has_star;
        }
        spec.gets.reserve(options.get_args.size());
        for (uint32_t arg : options.get_args) {
            SortPattern pattern;
            sort_pattern_parse(op.arg(arg), pattern);
            spec.gets.push_back(pattern);
        }
    } catch (const std::bad_alloc&) {
        elements.clear();
        return WorkError::Oom;
    }
    return WorkError::None;
}

WorkError sort_image_finish(const SortSpec& spec, const SortResolved* resolved,
                            std::vector<std::string>& elements,
                            std::vector<std::string>& values, std::vector<uint8_t>& present,
                            bool& conversion_error) {
    conversion_error = false;
    switch (sort_run(spec, resolved, elements, values, present)) {
        case SortStatus::Ok: return WorkError::None;
        case SortStatus::ConversionError:
            conversion_error = true;
            values.clear();
            present.clear();
            return WorkError::None;
        case SortStatus::Oom: return WorkError::Oom;
    }
    return WorkError::None;
}

// ONE ordering implementation for every SORT form: `sort_run` in src/cmd/t_sort.cc. This wrapper
// is the immediate path for forms with no dynamic gather.
WorkError sort_image(const ObjectImage& image, const Op& op, const SortOptions& options,
                     const SortResolved* resolved,
                     std::vector<std::string>& values, std::vector<uint8_t>& present,
                     bool& conversion_error) {
    SortSpec spec;
    std::vector<std::string> elements;
    const WorkError prepared = sort_image_prepare(image, op, options, spec, elements);
    if (prepared != WorkError::None) {
        conversion_error = false;
        values.clear();
        present.clear();
        return prepared;
    }
    return sort_image_finish(spec, resolved, elements, values, present, conversion_error);
}

// A stored SORT result is always a list of plain strings: an absent GET becomes an empty element,
// which is what the reference stores too.
bool sort_encode_store(const std::vector<std::string>& values,
                       const std::vector<uint8_t>& present, ObjectImage& image) {
    std::vector<std::string> flat;
    try {
        flat.reserve(values.size());
        for (size_t i = 0; i < values.size(); i++)
            flat.push_back(i < present.size() && !present[i] ? std::string() : values[i]);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return encode_elements(flat, image, Type::List, -1);
}

void sort_reply_rows(Op& op, const std::vector<std::string>& values,
                     const std::vector<uint8_t>& present) {
    reply_array_header(op.sink(), values.size());
    for (size_t i = 0; i < values.size(); i++) {
        if (i < present.size() && !present[i]) { reply_null(op.sink(), op.resp3()); continue; }
        reply_bulk(op.sink(), Slice(values[i].data(), static_cast<uint32_t>(values[i].size())));
    }
}

bool same_key(const Op& op) {
    const Slice a = op.arg(1), b = op.arg(2);
    return a.n == b.n && (a.n == 0 || std::memcmp(a.p, b.p, a.n) == 0);
}

bool image_type(const ObjectImage& image, Type type) {
    return !image.present || image.type == type;
}

bool contains(const std::vector<std::string>& values, Slice wanted) {
    for (const std::string& value : values)
        if (value.size() == wanted.n &&
            (wanted.n == 0 || std::memcmp(value.data(), wanted.p, wanted.n) == 0)) return true;
    return false;
}

bool erase_member(std::vector<std::string>& values, Slice wanted) {
    for (auto it = values.begin(); it != values.end(); ++it)
        if (it->size() == wanted.n &&
            (wanted.n == 0 || std::memcmp(it->data(), wanted.p, wanted.n) == 0)) {
            values.erase(it); return true;
        }
    return false;
}

void reply_work_error(Op& op, WorkError error) {
    if (error == WorkError::WrongType) reply_wrongtype(op.sink());
    else if (error == WorkError::BadHll)
        reply_err(op.sink(), "WRONGTYPE Key is not a valid HyperLogLog string value.");
    else if (error == WorkError::CorruptHll)
        reply_err(op.sink(), "INVALIDOBJ Corrupted HLL object detected");
    else if (error == WorkError::Maxmemory) reply_maxmemory_oom(op);
    else if (error == WorkError::Oom) reply_err(op.sink(), "ERR out of memory");
    else if (error == WorkError::InsertFailed) reply_err(op.sink(), "ERR keyspace insert failed");
    else if (error == WorkError::GeoMember)
        reply_err(op.sink(), "ERR could not decode requested zset member");
    else reply_err(op.sink(), "ERR internal cross-shard value error");
}


bool compute_setop(ScatterState& state) {
    ResultHeap* result_heap = ensure_result(state);
    if (!result_heap) { state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return false; }
    std::vector<std::vector<std::string>> inputs;
    try {
        inputs.resize(state.set_count);
        for (uint32_t i = 0; i < state.set_count; i++) {
            const ObjectImage& image = state.images[state.set_first + i];
            if (!image_type(image, Type::Set)) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::WrongType;
                return false;
            }
            if (image.present && !decode_elements(image, inputs[i])) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return false;
            }
        }
        std::unordered_set<std::string> result;
        if (state.kind == Kind::Sunion || state.kind == Kind::Sunionstore) {
            for (const auto& input : inputs) for (const std::string& member : input) result.insert(member);
        } else if (state.kind == Kind::Sdiff || state.kind == Kind::Sdiffstore) {
            if (!inputs.empty()) for (const std::string& member : inputs[0]) result.insert(member);
            for (size_t i = 1; i < inputs.size(); i++)
                for (const std::string& member : inputs[i]) result.erase(member);
        } else {
            if (!inputs.empty()) for (const std::string& member : inputs[0]) result.insert(member);
            for (size_t i = 1; i < inputs.size(); i++) {
                std::unordered_set<std::string> next(inputs[i].begin(), inputs[i].end());
                for (auto it = result.begin(); it != result.end();)
                    if (!next.count(*it)) it = result.erase(it); else ++it;
            }
        }
        result_heap->members.assign(result.begin(), result.end());
    } catch (const std::bad_alloc&) {
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    return true;
}

bool decode_zsetop_input(const ObjectImage& image, std::vector<ZImageItem>& items,
                         WorkError& error) {
    items.clear();
    if (!image.present) return true;
    if (image.type == Type::Zset) {
        if (!decode_zset_image(image, items)) { error = WorkError::Oom; return false; }
    } else if (image.type == Type::Set) {
        std::vector<std::string> members;
        if (!decode_elements(image, members)) { error = WorkError::Oom; return false; }
        try {
            items.reserve(members.size());
            for (std::string& member : members) items.push_back({std::move(member), 1.0});
        } catch (const std::bad_alloc&) {
            error = WorkError::Oom;
            return false;
        }
    } else {
        error = WorkError::WrongType;
        return false;
    }
    std::sort(items.begin(), items.end(), [](const ZImageItem& a, const ZImageItem& b) {
        return a.member < b.member;
    });
    return true;
}

bool compute_zsetop(ScatterState& state) {
    ResultHeap* result = ensure_result(state);
    if (!result) {
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    result->members.clear();
    result->scores.clear();
    std::vector<std::vector<ZImageItem>> inputs;
    try { inputs.resize(state.set_count); }
    catch (const std::bad_alloc&) {
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    WorkError error = WorkError::None;
    for (uint32_t i = 0; i < state.set_count; i++) {
        if (!decode_zsetop_input(state.images[state.set_first + i], inputs[i], error)) {
            state.final_reply = FinalReply::Work;
            state.final_error = error;
            return false;
        }
    }

    // ACCUMULATION ORDER. The oracle folds the sources smallest-cardinality-first (a stable
    // reorder, so equal cardinalities keep argument order), not in argument order. That is
    // invisible for a commutative aggregate over ordinary values, but it decides two results we
    // compare byte-for-byte:
    //   * which operand is the incumbent when MIN/MAX tie, which is what prints -0 vs 0;
    //   * the rounding of a SUM, because float addition is not associative -- with cardinalities
    //     ONE=1, NEG=3, BIG=8, `ZUNION 3 NEG BIG ONE AGGREGATE SUM` is 0 on the oracle (it folds
    //     1 + -1e16 + 1e16) and 1 in argument order (-1e16 + 1e16 + 1).
    // Only the fold order moves: every membership test below still keys on the ORIGINAL argument
    // index, so ZDIFF's "source 0 only" rule and ZINTER's seen-count are untouched.
    std::vector<uint32_t> fold_rank(inputs.size());
    {
        std::vector<uint32_t> order(inputs.size());
        for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return inputs[a].size() < inputs[b].size();
        });
        for (uint32_t i = 0; i < order.size(); i++) fold_rank[order[i]] = i;
    }

    struct Cursor { uint32_t source; uint32_t index; };
    auto later = [&](const Cursor& a, const Cursor& b) {
        const std::string& am = inputs[a.source][a.index].member;
        const std::string& bm = inputs[b.source][b.index].member;
        if (am != bm) return am > bm;
        return fold_rank[a.source] > fold_rank[b.source];
    };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> heap(later);
    try {
        for (uint32_t source = 0; source < inputs.size(); source++)
            if (!inputs[source].empty()) heap.push({source, 0});
        std::vector<ZImageItem> output;
        while (!heap.empty()) {
            const std::string member = inputs[heap.top().source][heap.top().index].member;
            uint32_t seen = 0;
            bool source_zero = false;
            bool other_source = false;
            bool have_score = false;
            double score = 0;
            while (!heap.empty()) {
                const Cursor cursor = heap.top();
                const ZImageItem& item = inputs[cursor.source][cursor.index];
                if (item.member != member) break;
                heap.pop();
                seen++;
                source_zero |= cursor.source == 0;
                other_source |= cursor.source != 0;
                double weighted = item.score * state.zset_weights[cursor.source];
                if (std::isnan(weighted)) weighted = 0;
                if (!have_score) {
                    score = weighted;
                    have_score = true;
                // MIN/MAX are STRICT comparisons against the running accumulator, so a tie keeps
                // the incumbent -- the value contributed by the EARLIER source. That is the whole
                // rule for signed zeroes here, because -0 == +0: MIN(+0 then -0) is +0 and
                // MAX(-0 then +0) is -0 on the oracle, purely by argument order. An earlier
                // canonicalizing tie-break here (MIN prefers -0, MAX prefers +0) inverted the sign
                // on exactly those orders; the oracle does not canonicalize.
                } else if (state.zset_aggregate == ZAggregate::Min) {
                    if (weighted < score) score = weighted;
                } else if (state.zset_aggregate == ZAggregate::Max) {
                    if (weighted > score) score = weighted;
                } else {
                    score += weighted;
                    if (std::isnan(score)) score = 0;
                }
                const uint32_t next = cursor.index + 1;
                if (next < inputs[cursor.source].size()) heap.push({cursor.source, next});
            }
            bool include = false;
            if (state.kind == Kind::Zunion || state.kind == Kind::Zunionstore)
                include = true;
            else if (state.kind == Kind::Zdiff || state.kind == Kind::Zdiffstore)
                include = source_zero && !other_source;
            else
                include = seen == state.set_count;
            if (include) output.push_back({member, score});
            if (state.kind == Kind::Zintercard && state.zset_limit &&
                output.size() >= state.zset_limit) break;
        }
        std::sort(output.begin(), output.end(), [](const ZImageItem& a, const ZImageItem& b) {
            if (a.score < b.score) return true;
            if (a.score > b.score) return false;
            return a.member < b.member;
        });
        result->members.reserve(output.size());
        result->scores.reserve(output.size());
        for (ZImageItem& item : output) {
            result->members.push_back(std::move(item.member));
            result->scores.push_back(item.score);
        }
    } catch (const std::bad_alloc&) {
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    return true;
}

WorkError merge_hll_image(const ObjectImage& image,
                          std::array<uint8_t, hll::kRegisters>& maximum,
                          bool& any_dense) {
    if (!image.present) return WorkError::None;
    if (image.type != Type::String) return WorkError::WrongType;
    if (static_cast<Enc>(image.encoding) == Enc::Int ||
        static_cast<Enc>(image.encoding) == Enc::Compact)
        return WorkError::BadHll;
    const Slice value(reinterpret_cast<const char*>(image.payload.data()),
                      static_cast<uint32_t>(image.payload.size()));
    if (!hll::header_valid(value)) return WorkError::BadHll;
    any_dense |= hll::is_dense(value);
    return hll::merge_registers(value, maximum) ? WorkError::None : WorkError::CorruptHll;
}

void finish_pfcount(ScatterState& state) {
    WorkError error;
    if (first_error(state, error)) {
        state.final_reply = FinalReply::Work;
        state.final_error = error;
        return;
    }
    std::array<uint8_t, hll::kRegisters> maximum{};
    bool any_dense = false;
    for (uint32_t i = 0; i < state.key_count; i++) {
        error = merge_hll_image(state.images[i], maximum, any_dense);
        if (error != WorkError::None) {
            state.final_reply = FinalReply::Work;
            state.final_error = error;
            return;
        }
    }
    state.final_reply = FinalReply::Integer;
    state.final_integer = static_cast<long long>(hll::count_registers(maximum));
}

// Runs on the executor that closed the group, with acquire-ordered visibility of both images.
// Non-trivial CPU here follows the established precedent (SORT sorts, BITOP walks every byte,
// PFCOUNT merges 16384 registers); the reply itself is still emitted on IO.
void finish_lcs(ScatterState& state) {
    WorkError error;
    if (first_error(state, error)) {
        state.final_reply = FinalReply::Work;
        state.final_error = error;
        return;
    }
    ResultHeap* result = ensure_result(state);
    if (!result) { state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return; }

    // A non-string key is not a WRONGTYPE here: redis has a dedicated message for LCS.
    for (uint32_t i = 0; i < 2; i++)
        if (state.images[i].present && state.images[i].type != Type::String) {
            result->bulk = "ERR The specified keys must contain string values";
            state.final_reply = FinalReply::LcsError;
            return;
        }

    std::string a, b;
    if (!decode_string_image(state.images[0], a, error) ||
        !decode_string_image(state.images[1], b, error)) {
        state.final_reply = FinalReply::Work;
        state.final_error = error;
        return;
    }
    const char* message = nullptr;
    if (!lcs_compute(a, b, state.lcs_options, result->lcs, message)) {
        result->bulk = message ? message : "ERR LCS failed";
        state.final_reply = FinalReply::LcsError;
        return;
    }
    state.final_reply = FinalReply::Lcs;
}

bool compute_pfmerge(ScatterState& state) {
    std::array<uint8_t, hll::kRegisters> maximum{};
    bool any_dense = false;
    for (uint32_t i = 0; i < state.key_count; i++) {
        const WorkError error = merge_hll_image(state.images[i], maximum, any_dense);
        if (error != WorkError::None) {
            state.final_reply = FinalReply::Work;
            state.final_error = error;
            return false;
        }
    }

    const ObjectImage& destination = state.images[0];
    const Slice destination_value(
        destination.present ? reinterpret_cast<const char*>(destination.payload.data()) : nullptr,
        destination.present ? static_cast<uint32_t>(destination.payload.size()) : 0);
    std::string merged;
    try {
        if (!hll::merge_result(destination_value, destination.present, maximum, any_dense, merged)) {
            state.final_reply = FinalReply::Work;
            state.final_error = WorkError::CorruptHll;
            return false;
        }
        ObjectImage& apply = state.apply[0];
        apply = ObjectImage{};
        apply.present = true;
        apply.type = Type::String;
        apply.encoding = static_cast<uint8_t>(Enc::Extern);
        apply.expire_at_ms = destination.present ? destination.expire_at_ms : -1;
        apply.reserve_ttl_slot = destination.present && destination.reserve_ttl_slot;
        apply.payload.assign(merged.begin(), merged.end());
    } catch (const std::bad_alloc&) {
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    return true;
}

// True means phase one is terminal. False means hop2[] is complete and ready for one all-or-none
// capacity preflight/publication.
bool finish_phase1(ScatterState& state, Op& op) {
    WorkError error;
    if (first_error(state, error)) {
        state.final_reply = FinalReply::Work; state.final_error = error; return true;
    }

    if (state.atomic_write) {
        if (state.kind == Kind::Msetnx) {
            for (uint32_t i = 0; i < state.key_count; i++)
                if (state.values[i].kind != ValueKind::Nil) {
                    state.final_reply = FinalReply::Integer;
                    state.final_integer = 0;
                    return true;
                }
            state.final_integer = 1;
        }
        state.hop2_count = state.key_count;
        for (uint32_t i = 0; i < state.key_count; i++) state.hop2[i] = i;
        return false;
    }

    switch (state.kind) {
        case Kind::Pfmerge:
            if (!compute_pfmerge(state)) return true;
            state.hop2[0] = 0;
            state.hop2_count = 1;
            return false;
        case Kind::Msetnx:
            for (uint32_t i = 0; i < state.key_count; i++)
                if (state.values[i].kind != ValueKind::Nil) {
                    state.final_reply = FinalReply::Integer; state.final_integer = 0; return true;
                }
            state.final_integer = 1;
            state.hop2_count = state.key_count;
            for (uint32_t i = 0; i < state.key_count; i++) state.hop2[i] = i;
            return false;
        case Kind::Rename:
        case Kind::Renamenx: {
            const ObjectImage& source = state.images[0];
            if (!source.present) { state.final_reply = FinalReply::NoSuchKey; return true; }
            if (same_key(op)) {
                state.final_reply = state.kind == Kind::Rename ? FinalReply::Ok : FinalReply::Integer;
                state.final_integer = 0;
                return true;
            }
            if (state.kind == Kind::Renamenx && !state.atomic_apply &&
                state.images[1].present) {
                state.final_reply = FinalReply::Integer; state.final_integer = 0; return true;
            }
            try {
                state.apply[1] = source;
                state.apply[0] = ObjectImage{};
            } catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            state.final_integer = state.kind == Kind::Renamenx ? 1 : -1;
            state.hop2[0] = 1; state.hop2[1] = 0; state.hop2_count = 2;
            return false;
        }
        case Kind::Copy:
            if (same_key(op)) { state.final_reply = FinalReply::SameObject; return true; }
            if (!state.images[0].present ||
                (!state.atomic_apply && state.images[1].present && !state.copy_replace)) {
                state.final_reply = FinalReply::Integer; state.final_integer = 0; return true;
            }
            try {
                state.apply[1] = state.images[0];
                // COPY creates a new destination lifetime. A live TTL allocates its own slot;
                // a source that was merely PERSISTed does not donate its historical reservation.
                state.apply[1].reserve_ttl_slot = false;
            }
            catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            state.final_integer = 1; state.hop2[0] = 1; state.hop2_count = 1; return false;
        case Kind::Smove: {
            const ObjectImage& source = state.images[0];
            const ObjectImage& dest = state.images[1];
            if (!source.present) { state.final_reply = FinalReply::Integer; state.final_integer = 0; return true; }
            if (!image_type(source, Type::Set) || !image_type(dest, Type::Set)) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::WrongType; return true;
            }
            if (!state.atomic_apply) {
                if (state.final_integer == 0) {
                    state.final_reply = FinalReply::Integer;
                    return true;
                }
                if (same_key(op)) {
                    state.final_reply = FinalReply::Integer;
                    state.final_integer = 1;
                    return true;
                }
                state.final_integer = 1;
                state.hop2[0] = 0;
                state.hop2[1] = 1;
                state.hop2_count = 2;
                return false;
            }
            std::vector<std::string> src, dst;
            if (!decode_elements(source, src) || (dest.present && !decode_elements(dest, dst))) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            const Slice member = op.arg(3);
            if (!contains(src, member)) { state.final_reply = FinalReply::Integer; state.final_integer = 0; return true; }
            if (same_key(op)) { state.final_reply = FinalReply::Integer; state.final_integer = 1; return true; }
            try {
                erase_member(src, member);
                if (!contains(dst, member)) dst.emplace_back(member.p, member.n);
            } catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            if (src.empty()) state.apply[0] = ObjectImage{};
            else if (!encode_elements(src, state.apply[0], Type::Set, source.expire_at_ms,
                                      source.reserve_ttl_slot)) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            if (!encode_elements(dst, state.apply[1], Type::Set,
                                 dest.present ? dest.expire_at_ms : -1,
                                 dest.present && dest.reserve_ttl_slot)) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            state.final_integer = 1; state.hop2[0] = 0; state.hop2[1] = 1; state.hop2_count = 2;
            return false;
        }
        case Kind::Lmove:
        case Kind::Rpoplpush: {
            const ObjectImage& source = state.images[0];
            const ObjectImage& dest = state.images[1];
            if (!source.present) { state.final_reply = FinalReply::Nil; return true; }
            if (!image_type(source, Type::List) || !image_type(dest, Type::List)) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::WrongType; return true;
            }
            if (!state.atomic_apply) {
                if (!source.entries) { state.final_reply = FinalReply::Nil; return true; }
                if (!state.result) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = WorkError::Oom;
                    return true;
                }
                state.hop2[0] = 0;
                state.hop2[1] = 1;
                state.hop2_count = 2;
                return false;
            }
            std::vector<std::string> src, dst;
            if (!decode_elements(source, src) || (dest.present && !decode_elements(dest, dst))) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            if (src.empty()) { state.final_reply = FinalReply::Nil; return true; }
            ResultHeap* result_heap = ensure_result(state);
            if (!result_heap) { state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true; }
            try {
                result_heap->bulk = state.from_left ? src.front() : src.back();
                if (state.from_left) src.erase(src.begin()); else src.pop_back();
                if (same_key(op)) {
                    if (state.to_left) src.insert(src.begin(), result_heap->bulk);
                    else src.push_back(result_heap->bulk);
                    if (!encode_elements(src, state.apply[0], Type::List, source.expire_at_ms,
                                         source.reserve_ttl_slot))
                        throw std::bad_alloc();
                    state.hop2[0] = 0; state.hop2_count = 1;
                } else {
                    if (state.to_left) dst.insert(dst.begin(), result_heap->bulk);
                    else dst.push_back(result_heap->bulk);
                    if (src.empty()) state.apply[0] = ObjectImage{};
                    else if (!encode_elements(src, state.apply[0], Type::List,
                                              source.expire_at_ms,
                                              source.reserve_ttl_slot))
                        throw std::bad_alloc();
                    if (!encode_elements(dst, state.apply[1], Type::List,
                                         dest.present ? dest.expire_at_ms : -1,
                                         dest.present && dest.reserve_ttl_slot))
                        throw std::bad_alloc();
                    state.hop2[0] = 0; state.hop2[1] = 1; state.hop2_count = 2;
                }
            } catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
            }
            return false;
        }
        case Kind::Bitop:
            if (!compute_bitop(state.images, 1, state.key_count - 1, op.arg(1), state.apply[0],
                               state.final_integer, state.final_error)) {
                state.final_reply = FinalReply::Work;
                return true;
            }
            state.hop2[0] = 0;
            state.hop2_count = 1;
            return false;
        case Kind::Lmpop:
        case Kind::Zmpop: {
            const Type wanted = state.kind == Kind::Lmpop ? Type::List : Type::Zset;
            for (uint32_t i = 0; i < state.key_count; i++) {
                const ObjectImage& probe = state.images[i];
                if (!probe.present) continue;
                if (probe.type != wanted) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = WorkError::WrongType;
                    return true;
                }
                if (!probe.entries) continue;
                if (!ensure_result(state)) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = WorkError::Oom;
                    return true;
                }
                state.selected = i;
                state.hop2[0] = i;
                state.hop2_count = 1;
                return false;
            }
            state.final_reply = FinalReply::NullArray;
            return true;
        }
        case Kind::Zrangestore: {
            const ObjectImage& source = state.images[1];
            if (source.present && source.type != Type::Zset) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::WrongType;
                return true;
            }
            std::vector<ZImageItem> input, selected;
            if (source.present && !decode_zset_image(source, input)) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            if (!select_zrange(input, state.range_options, state.range_spec, source.expanded,
                               selected)) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            if (selected.empty()) state.apply[0] = ObjectImage{};
            else if (!encode_zset_image(selected, state.apply[0])) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            state.final_integer = static_cast<long long>(selected.size());
            state.hop2[0] = 0;
            state.hop2_count = 1;
            return false;
        }
        case Kind::Geosearchstore:
        case Kind::Georadius:
        case Kind::Georadiusbymember: {
            if (!state.geo_store) { state.final_reply = FinalReply::Internal; return true; }
            const ObjectImage& source = state.images[1];
            if (source.present && source.type != Type::Zset) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::WrongType;
                return true;
            }
            std::vector<ZImageItem> decoded;
            if (source.present && !decode_zset_image(source, decoded)) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            std::vector<ZsetEntry> input, output;
            try {
                input.reserve(decoded.size());
                for (ZImageItem& item : decoded)
                    input.push_back({std::move(item.member), item.score});
            } catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            const GeoBuildResult built = geo_build_store(op, input, output);
            if (built != GeoBuildResult::Ok) {
                state.final_reply = FinalReply::Work;
                state.final_error = built == GeoBuildResult::MissingMember
                    ? WorkError::GeoMember : WorkError::Oom;
                return true;
            }
            std::vector<ZImageItem> encoded;
            try {
                encoded.reserve(output.size());
                for (ZsetEntry& item : output)
                    encoded.push_back({std::move(item.member), item.score});
            } catch (const std::bad_alloc&) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            if (encoded.empty()) state.apply[0] = ObjectImage{};
            else if (!encode_zset_image(encoded, state.apply[0])) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            state.final_integer = static_cast<long long>(encoded.size());
            state.hop2[0] = 0;
            state.hop2_count = 1;
            return false;
        }
        case Kind::Sort: {
            ResultHeap* result = ensure_result(state);
            if (!result) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            bool conversion_error = state.sort_conversion_error;
            if (!state.sort_general_done) {
                // Non-store SORT has ONE key at index 0; only the STORE form puts the destination
                // at 0 and the source at 1. This arm historically ran only for STORE (localfast
                // covered every single-key group), so the store-shaped indexing and the
                // unconditional hop-2 below sat unexercised until atomic tracking started routing
                // same-owner groups here.
                //
                // A dereferencing BY/GET form lands here only after its owner-grouped gather has
                // already populated state.result and set sort_general_done.
                const ObjectImage& source = state.images[state.sort_options.store ? 1 : 0];
                const WorkError error = sort_image(source, op, state.sort_options, nullptr,
                                                   result->members,
                                                   result->present, conversion_error);
                if (error != WorkError::None) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = error;
                    return true;
                }
            }
            if (conversion_error) {
                state.final_reply = FinalReply::SortConversion;
                return true;
            }
            if (!state.sort_options.store) {
                state.final_reply = FinalReply::Array;
                return true;
            }
            if (result->members.empty()) state.apply[0] = ObjectImage{};
            else if (!sort_encode_store(result->members, result->present, state.apply[0])) {
                state.final_reply = FinalReply::Work;
                state.final_error = WorkError::Oom;
                return true;
            }
            state.final_integer = static_cast<long long>(result->members.size());
            state.hop2[0] = 0;
            state.hop2_count = 1;
            return false;
        }
        default:
            if (is_any_zsetop(state.kind)) {
                if (!compute_zsetop(state)) return true;
                if (state.kind == Kind::Zintercard) {
                    state.final_reply = FinalReply::Integer;
                    state.final_integer = static_cast<long long>(state.result->members.size());
                    return true;
                }
                if (is_plain_zsetop(state.kind)) {
                    state.final_reply = FinalReply::Array;
                    return true;
                }
                std::vector<ZImageItem> output;
                try {
                    output.reserve(state.result->members.size());
                    for (size_t i = 0; i < state.result->members.size(); i++) {
                        double score = state.result->scores[i];
                        if (score == 0) score = std::abs(score);
                        output.push_back({state.result->members[i], score});
                    }
                } catch (const std::bad_alloc&) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = WorkError::Oom;
                    return true;
                }
                if (output.empty()) state.apply[0] = ObjectImage{};
                else if (!encode_zset_image(output, state.apply[0])) {
                    state.final_reply = FinalReply::Work;
                    state.final_error = WorkError::Oom;
                    return true;
                }
                state.final_integer = static_cast<long long>(output.size());
                state.hop2[0] = 0;
                state.hop2_count = 1;
                return false;
            }
            if (is_plain_setop(state.kind) || state.kind == Kind::Sintercard || is_store_setop(state.kind)) {
                if (!compute_setop(state)) return true;
                if (state.kind == Kind::Sintercard) {
                    uint64_t count = state.result->members.size();
                    if (state.sinter_limit && count > state.sinter_limit) count = state.sinter_limit;
                    state.final_reply = FinalReply::Integer;
                    state.final_integer = static_cast<long long>(count);
                    return true;
                }
                if (is_plain_setop(state.kind)) { state.final_reply = FinalReply::Array; return true; }
                if (state.result->members.empty()) state.apply[0] = ObjectImage{};
                else if (!encode_elements(state.result->members, state.apply[0], Type::Set, -1)) {
                    state.final_reply = FinalReply::Work; state.final_error = WorkError::Oom; return true;
                }
                state.final_integer = static_cast<long long>(state.result->members.size());
                state.hop2[0] = 0; state.hop2_count = 1;
                return false;
            }
            state.final_reply = FinalReply::Internal;
            return true;
    }
}

bool finish_phase2(ScatterState& state) {
    WorkError error;
    if (first_error(state, error)) {
        if (state.atomic_apply) state.aborted.store(true, std::memory_order_release);
        state.final_reply = FinalReply::Work; state.final_error = error; return false;
    }
    if (state.atomic_direct && state.final_reply != FinalReply::None) return false;
    bool commit_ready = false;
    if (state.atomic_apply) {
        if (state.conditional_failed.load(std::memory_order_acquire)) {
            state.final_reply = FinalReply::Integer;
            state.final_integer = 0;
            return false;
        }
        if (state.aborted.load(std::memory_order_acquire)) {
            state.final_reply = FinalReply::Internal;
            return false;
        }
        commit_ready = !state.multi_child;
    }
    switch (state.kind) {
        case Kind::Msetnx: state.final_reply = FinalReply::Integer; state.final_integer = 1; break;
        case Kind::Rename: state.final_reply = FinalReply::Ok; break;
        case Kind::Renamenx:
        case Kind::Copy:
        case Kind::Smove:
        case Kind::Sinterstore:
        case Kind::Sunionstore:
        case Kind::Sdiffstore:
        case Kind::Zunionstore:
        case Kind::Zinterstore:
        case Kind::Zdiffstore: state.final_reply = FinalReply::Integer; break;
        case Kind::Bitop: state.final_reply = FinalReply::Integer; break;
        case Kind::Pfmerge: state.final_reply = FinalReply::Ok; break;
        case Kind::Lmove:
        case Kind::Rpoplpush: state.final_reply = FinalReply::Bulk; break;
        case Kind::Lmpop:
            state.final_reply = state.result && !state.result->members.empty()
                ? FinalReply::Lmpop : FinalReply::NullArray;
            break;
        case Kind::Zmpop:
            state.final_reply = state.result && !state.result->members.empty()
                ? FinalReply::Zmpop : FinalReply::NullArray;
            break;
        case Kind::Zrangestore:
        case Kind::Geosearchstore:
        case Kind::Georadius:
        case Kind::Georadiusbymember:
        case Kind::Sort: state.final_reply = FinalReply::Integer; break;
        default: state.final_reply = FinalReply::Internal; break;
    }
    return commit_ready;
}

void assemble_mget(Client& client, Op& op, ScatterState& state, void* release_ctx,
                   void (*release_fn)(void*, int32_t, const char*),
                   void (*suppressed_fn)(void*)) {
    // MULTI assembles child replies on an executor, where no IO->owner release callback is
    // available. Copy a TLS child into its Op and leave the borrow descriptors in ScatterState;
    // multi_retire_entry returns them from the owning IO before destroying the state.
    if (client.is_tls() && !release_fn) {
        reply_array_header(op.sink(), state.key_count);
        for (uint32_t i = 0; i < state.key_count; i++) {
            const ValueSlot& slot = state.values[i];
            if (slot.kind == ValueKind::Nil) reply_null(op.sink(), op.resp3());
            else if (slot.kind == ValueKind::Inline)
                reply_bulk(op.sink(), Slice(slot.small, slot.len));
            else
                reply_bulk(op.sink(), Slice(slot.ptr, slot.len));
        }
        return;
    }
    bool borrowed = false;
    for (uint32_t i = 0; i < state.key_count; i++)
        borrowed |= state.values[i].kind == ValueKind::Borrow;
    if (!borrowed) {
        reply_array_header(op.sink(), state.key_count);
        for (uint32_t i = 0; i < state.key_count; i++) {
            const ValueSlot& slot = state.values[i];
            if (slot.kind == ValueKind::Nil) reply_null(op.sink(), op.resp3());
            else reply_bulk(op.sink(), Slice(slot.small, slot.len));
        }
        return;
    }

    client.seal_fill_segment();
    // NO CODED REPLIES FOR THE REST OF THIS OP. flush_bytes below hands bytes to the connection
    // and then EMPTIES the sink, mid-op. A coded reply is only equivalent to its bytes because a
    // code is taken on an empty sink and therefore stands for the WHOLE reply; here the sink goes
    // empty again with elements already on the wire, so the very next reply_null() would code a
    // "$-1" that retire then emits ahead of everything staged after it. That desynchronises the
    // connection -- it is exactly how the MGET-with-a-hole row in tests/edgeenc.py failed.
    // Disarming for the remainder of the op keeps this whole assembly on the byte path it has
    // always used; it runs once per borrowed MGET, not per reply.
    op.reply_code_ok_ = 0;
    op_materialise_code(op);        // anything already coded becomes bytes, in place, in order
    auto flush_bytes = [&] {
        client.append_buf_segment(op.direct, op.direct_len, op.reply.data(), op.reply.size());
        op.direct_len = 0;
        op.reply.clear();
    };
    reply_array_header(op.sink(), state.key_count);
    for (uint32_t i = 0; i < state.key_count; i++) {
        ValueSlot& slot = state.values[i];
        if (slot.kind == ValueKind::Nil) reply_null(op.sink(), op.resp3());
        else if (slot.kind == ValueKind::Inline) reply_bulk(op.sink(), Slice(slot.small, slot.len));
        else {
            reply_bulk_header(op.sink(), slot.len);
            flush_bytes();
            if (client.is_tls()) {
                client.append_buf_segment(slot.ptr, slot.len);
                if (release_fn) release_fn(release_ctx, slot.shard, slot.ptr);
                if (suppressed_fn) suppressed_fn(release_ctx);
            } else {
                client.append_borrow_segment(slot.ptr, slot.len, slot.shard);
            }
            static constexpr char crlf[2] = {'\r', '\n'};
            client.append_static_segment(crlf, sizeof(crlf));
            // Ownership moved to the connection queue. Arena teardown must not return it again.
            slot.kind = ValueKind::Nil;
        }
    }
}

void assemble_final(Client& client, Op& op, ScatterState& state, void* release_ctx,
                    void (*release_fn)(void*, int32_t, const char*),
                    void (*suppressed_fn)(void*)) {
    // The coordinator ran the original EVAL/FCALL handler, so its RESP (including a runtime error
    // after retained effects) is already complete. Engine refusals replace that buffer before
    // reaching retirement. Generic status bytes are the script read/write set, not WorkError.
    if (state.kind == Kind::Script) return;
    if (state.final_reply != FinalReply::None) {
        switch (state.final_reply) {
            case FinalReply::Ok: reply_ok(op.sink()); break;
            case FinalReply::Lcs: lcs_reply(op, state.lcs_options, state.result->lcs); break;
            case FinalReply::LcsError: reply_err(op.sink(), state.result->bulk.c_str()); break;
            case FinalReply::Integer: reply_int(op.sink(), state.final_integer); break;
            case FinalReply::Nil: reply_null(op.sink(), op.resp3()); break;
            case FinalReply::NullArray: reply_null_array(op.sink(), op.resp3()); break;
            case FinalReply::Bulk:
                reply_bulk(op.sink(), Slice(state.result->bulk.data(),
                                            static_cast<uint32_t>(state.result->bulk.size())));
                break;
            case FinalReply::Array:
                // SORT's GET projection is the one Array result whose rows can be RESP nulls.
                if (state.kind == Kind::Sort) {
                    sort_reply_rows(op, state.result->members, state.result->present);
                    break;
                }
                if (is_plain_setop(state.kind))
                    reply_set_header(op.sink(), state.result->members.size(), op.resp3());
                else if (is_plain_zsetop(state.kind))
                    reply_array_header(op.sink(), state.result->members.size() *
                        (state.zset_withscores && !op.resp3() ? 2 : 1));
                else
                    reply_array_header(op.sink(), state.result->members.size());
                for (size_t i = 0; i < state.result->members.size(); i++) {
                    const std::string& member = state.result->members[i];
                    if (is_plain_zsetop(state.kind) && state.zset_withscores && op.resp3())
                        reply_array_header(op.sink(), 2);
                    reply_bulk(op.sink(), Slice(member.data(), static_cast<uint32_t>(member.size())));
                    if (is_plain_zsetop(state.kind) && state.zset_withscores)
                        reply_double(op.sink(), state.result->scores[i], op.resp3());
                }
                break;
            case FinalReply::Lmpop:
                reply_array_header(op.sink(), 2);
                reply_bulk(op.sink(), op.arg(state.keys[state.selected].arg));
                if (state.pop_reply_shape == PopReplyShape::ListPair) {
                    const std::string& member = state.result->members.front();
                    reply_bulk(op.sink(), Slice(member.data(), static_cast<uint32_t>(member.size())));
                } else {
                    reply_array_header(op.sink(), state.result->members.size());
                    for (const std::string& member : state.result->members)
                        reply_bulk(op.sink(), Slice(member.data(), static_cast<uint32_t>(member.size())));
                }
                break;
            case FinalReply::Zmpop:
                reply_array_header(op.sink(),
                    state.pop_reply_shape == PopReplyShape::ZsetTriple ? 3 : 2);
                reply_bulk(op.sink(), op.arg(state.keys[state.selected].arg));
                if (state.pop_reply_shape != PopReplyShape::ZsetTriple)
                    reply_array_header(op.sink(), state.result->members.size());
                for (size_t i = 0; i < state.result->members.size(); i++) {
                    if (state.pop_reply_shape != PopReplyShape::ZsetTriple)
                        reply_array_header(op.sink(), 2);
                    const std::string& member = state.result->members[i];
                    reply_bulk(op.sink(), Slice(member.data(), static_cast<uint32_t>(member.size())));
                    reply_double(op.sink(), state.result->scores[i], op.resp3());
                }
                break;
            case FinalReply::SortConversion:
                reply_err(op.sink(), "ERR One or more scores can't be converted into double");
                break;
            case FinalReply::Work: reply_work_error(op, state.final_error); break;
            case FinalReply::NoSuchKey: reply_err(op.sink(), "ERR no such key"); break;
            case FinalReply::SameObject:
                reply_err(op.sink(), "ERR source and destination objects are the same"); break;
            case FinalReply::QueueFull:
                reply_err(op.sink(), "ERR cross-shard second hop queue is full"); break;
            case FinalReply::Internal:
                reply_err(op.sink(), "ERR internal cross-shard completion error"); break;
            case FinalReply::None: break;
        }
        return;
    }

    WorkError error;
    if (first_error(state, error)) { reply_work_error(op, error); return; }
    switch (state.kind) {
        case Kind::AllShards:
        case Kind::DebugReload:
        case Kind::DebugLoadAof: reply_ok(op.sink()); break;
        case Kind::DebugBorrowCount:
        case Kind::DbsizeExact:
            reply_int(op.sink(), static_cast<long long>(state.exact_sum.load(std::memory_order_relaxed)));
            break;
        case Kind::Mget:
            assemble_mget(client, op, state, release_ctx, release_fn, suppressed_fn);
            break;
        case Kind::Xread: {
            // Stream payloads are copied images, never zc borrows, so the TLS release plumbing
            // that Mget threads through does not apply here.
            uint32_t ready = 0;
            for (uint32_t i = 0; i < state.key_count; i++) ready += state.images[i].present;
            if (!ready) { reply_null_array(op.sink(), op.resp3()); break; }
            if (op.resp3()) reply_map_header(op.sink(), ready, true);
            else reply_array_header(op.sink(), ready);
            for (uint32_t i = 0; i < state.key_count; i++) {
                if (!state.images[i].present) continue;
                if (!stream_reply_xread_payload(op, op.arg(state.keys[i].arg),
                                                state.images[i].payload)) {
                    reply_err(op.sink(), "ERR corrupt stream encoding");
                    return;
                }
            }
            break;
        }
        case Kind::Mset: reply_ok(op.sink()); break;
        case Kind::Del:
        case Kind::Unlink:
        case Kind::Exists:
        case Kind::Touch: {
            uint64_t count = 0;
            for (uint32_t i = 0; i < state.key_count; i++) count += state.status[i] != 0;
            reply_int(op.sink(), static_cast<long long>(count));
            break;
        }
        case Kind::Keys: {
            uint64_t count = 0;
            for (uint32_t i = 0; i < state.nsub; i++)
                if (state.groups[i].aux) count += state.groups[i].aux->keys_result.size();
            reply_array_header(op.sink(), count);
            for (uint32_t i = 0; i < state.nsub; i++)
                if (state.groups[i].aux)
                    for (const std::string& key : state.groups[i].aux->keys_result)
                        reply_bulk(op.sink(), Slice(key.data(), static_cast<uint32_t>(key.size())));
            break;
        }
        default: reply_err(op.sink(), "ERR internal cross-shard completion error"); break;
    }
}

bool compute_local_setop(Kind kind, const std::vector<ObjectImage>& images, uint32_t first,
                         uint32_t count, std::vector<std::string>& members, WorkError& error) {
    std::vector<std::vector<std::string>> inputs;
    try {
        inputs.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            const ObjectImage& image = images[first + i];
            if (!image_type(image, Type::Set)) { error = WorkError::WrongType; return false; }
            if (image.present && !decode_elements(image, inputs[i])) { error = WorkError::Oom; return false; }
        }
        std::unordered_set<std::string> result;
        if (kind == Kind::Sunion || kind == Kind::Sunionstore) {
            for (const auto& input : inputs) for (const std::string& member : input) result.insert(member);
        } else if (kind == Kind::Sdiff || kind == Kind::Sdiffstore) {
            if (!inputs.empty()) for (const std::string& member : inputs[0]) result.insert(member);
            for (size_t i = 1; i < inputs.size(); i++)
                for (const std::string& member : inputs[i]) result.erase(member);
        } else {
            if (!inputs.empty()) for (const std::string& member : inputs[0]) result.insert(member);
            for (size_t i = 1; i < inputs.size(); i++) {
                std::unordered_set<std::string> next(inputs[i].begin(), inputs[i].end());
                for (auto it = result.begin(); it != result.end();)
                    if (!next.count(*it)) it = result.erase(it); else ++it;
            }
        }
        members.assign(result.begin(), result.end());
        return true;
    } catch (const std::bad_alloc&) {
        error = WorkError::Oom;
        return false;
    }
}

template <bool kNotify>
void local_apply(Shard& shard, Op& op, const std::vector<ObjectImage>& apply,
                 const std::vector<uint32_t>& args, WorkError& error,
                 int32_t suppress_del_arg = -1) {
    for (uint32_t arg : args) {
        const WorkError result = apply_image<kNotify>(
            shard, op.arg(arg), FlatStore::hash_key(op.arg(arg)), apply[arg],
            static_cast<int32_t>(arg) == suppress_del_arg);
        if (error == WorkError::None && result != WorkError::None) error = result;
    }
}

}  // namespace

template <bool kNotify>
void cmd_xshard_only_impl(Shard& shard, Op& op) {
    if (!op.local_xshard()) {
        reply_err(op.sink(), "ERR internal cross-shard routing error");
        return;
    }
    Kind kind;
    if (!classify(op, kind)) { reply_err(op.sink(), "ERR internal cross-shard routing error"); return; }

    if (kind == Kind::Mget) {
        reply_array_header(op.sink(), op.argc() - 1);
        for (uint32_t arg = 1; arg < op.argc(); arg++) {
            const uint64_t hash = arg == 1 ? op.hash : FlatStore::hash_key(op.arg(arg));
            KvObj* object = shard.store_find_read<kNotify>(hash, op.arg(arg));
            if (!object || static_cast<Type>(object->type) != Type::String) {
                reply_null(op.sink(), op.resp3()); continue;
            }
            if (object->is_int()) {
                char integer[24];
                const uint32_t n = i64_to_dec(integer, object->int_value());
                reply_bulk(op.sink(), Slice(integer, n));
            } else {
                KvObjRawReadBuffer raw;
                reply_bulk(op.sink(), kvobj_string_value(object, raw));
            }
        }
        return;
    }
    if (is_geo_kind(kind)) {
        cmd_geo_xshard_local(shard, op, kNotify);
        return;
    }
    if (kind == Kind::Xread) {
        StreamXreadArgs parsed;
        if (!stream_parse_xread(op, parsed)) return;
        if (parsed.key_count != 1) {
            reply_err(op.sink(), "ERR internal XREAD local routing error");
            return;
        }
        StreamID cursor;
        bool latest = false;
        if (!stream_parse_xread_id(op.arg(parsed.first_id), cursor, latest)) {
            reply_err(op.sink(), "ERR Invalid stream ID specified as stream command argument");
            return;
        }
        std::vector<uint8_t> payload;
        const Slice key = op.arg(parsed.first_key);
        const StreamReadResult read = stream_xread_gather(
            shard, key, FlatStore::hash_key(key), cursor, latest, parsed.count, payload);
        if (read == StreamReadResult::WrongType) reply_wrongtype(op.sink());
        else if (read == StreamReadResult::Oom) reply_err(op.sink(), "ERR out of memory");
        else if (read == StreamReadResult::Corrupt)
            reply_err(op.sink(), "ERR corrupt stream encoding");
        else if (read != StreamReadResult::Ready) reply_null_array(op.sink(), op.resp3());
        else {
            if (op.resp3()) reply_map_header(op.sink(), 1, true);
            else reply_array_header(op.sink(), 1);
            if (!stream_reply_xread_payload(op, key, payload))
                reply_err(op.sink(), "ERR corrupt stream encoding");
        }
        return;
    }
    if (kind == Kind::Mset || kind == Kind::Msetnx) {
        if (kind == Kind::Msetnx) {
            for (uint32_t arg = 1; arg < op.argc(); arg += 2)
                if (shard.store_find<kNotify>(FlatStore::hash_key(op.arg(arg)), op.arg(arg))) {
                    reply_int(op.sink(), 0); return;
                }
        }
        WorkError error = WorkError::None;
        for (uint32_t arg = 1; arg < op.argc(); arg += 2) {
            const WorkError result = store_xstring<kNotify>(
                shard, op.arg(arg), FlatStore::hash_key(op.arg(arg)), op.arg(arg + 1));
            if (error == WorkError::None && result != WorkError::None) error = result;
            if constexpr (kNotify) if (result == WorkError::None)
                notify_record(shard, op, NOTIFY_STRING, NotifyEventId::Set, op.arg(arg));
        }
        if (error != WorkError::None) reply_work_error(op, error);
        else if (kind == Kind::Mset) reply_ok(op.sink());
        else reply_int(op.sink(), 1);
        return;
    }
    if (kind == Kind::Lmpop || kind == Kind::Zmpop) {
        uint32_t numkeys = 0;
        bool edge = false;
        uint64_t count = 1;
        if (!parse_mpop(op, kind, numkeys, edge, count)) return;
        uint32_t selected_arg = 0;
        const Type wanted = kind == Kind::Lmpop ? Type::List : Type::Zset;
        for (uint32_t i = 0; i < numkeys; i++) {
            const uint32_t arg = 2 + i;
            const uint64_t hash = FlatStore::hash_key(op.arg(arg));
            KvObj* object = shard.store_find<kNotify>(hash, op.arg(arg));
            if (!object) continue;
            if (static_cast<Type>(object->type) != wanted) {
                reply_wrongtype(op.sink());
                return;
            }
            if (CollectionRef(object).entries()) { selected_arg = arg; break; }
        }
        if (!selected_arg) { reply_null_array(op.sink(), op.resp3()); return; }

        std::vector<std::string> members;
        std::vector<double> scores;
        const uint64_t hash = FlatStore::hash_key(op.arg(selected_arg));
        const XshardPopResult popped = kind == Kind::Lmpop
            ? xshard_pop_list(shard, op.arg(selected_arg), hash, !edge, count, members)
            : xshard_pop_zset(shard, op.arg(selected_arg), hash, edge, count, members, scores);
        if (popped == XshardPopResult::WrongType) { reply_wrongtype(op.sink()); return; }
        if (popped == XshardPopResult::Oom) { set_oom(op); return; }
        if (popped == XshardPopResult::Missing) {
            reply_null_array(op.sink(), op.resp3()); return;
        }
        reply_array_header(op.sink(), 2);
        reply_bulk(op.sink(), op.arg(selected_arg));
        reply_array_header(op.sink(), members.size());
        for (size_t i = 0; i < members.size(); i++) {
            if (kind == Kind::Zmpop) reply_array_header(op.sink(), 2);
            reply_bulk(op.sink(), Slice(members[i].data(), static_cast<uint32_t>(members[i].size())));
            if (kind == Kind::Zmpop) reply_double(op.sink(), scores[i], op.resp3());
        }
        return;
    }
    if (kind == Kind::Zrangestore) {
        ImageRangeOptions options;
        ImageRangeSpec range;
        if (!parse_zrangestore_options(op, options) ||
            !parse_zrangestore_range(op, options, range)) return;
        ObjectImage source;
        const uint64_t source_hash = FlatStore::hash_key(op.arg(2));
        KvObj* object = shard.store_find_read<kNotify>(source_hash, op.arg(2));
        if (object && static_cast<Type>(object->type) != Type::Zset) {
            reply_wrongtype(op.sink());
            return;
        }
        if (object && !serialize_object(object, source)) { set_oom(op); return; }
        std::vector<ZImageItem> input, selected;
        if (source.present && !decode_zset_image(source, input)) { set_oom(op); return; }
        if (!select_zrange(input, options, range, source.expanded, selected)) {
            set_oom(op);
            return;
        }
        ObjectImage destination;
        if (!selected.empty() && !encode_zset_image(selected, destination)) { set_oom(op); return; }
        WorkError error = apply_image<kNotify>(
            shard, op.arg(1), FlatStore::hash_key(op.arg(1)), destination, true);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify)
                notify_record(shard, op, selected.empty() ? NOTIFY_GENERIC : NOTIFY_ZSET,
                              selected.empty() ? NotifyEventId::Del : NotifyEventId::Zrangestore,
                              op.arg(1));
            reply_int(op.sink(), static_cast<long long>(selected.size()));
        }
        return;
    }
    if (kind == Kind::Sort) {
        SortOptions options;
        if (!parse_sort_options(op, options)) return;
        ObjectImage source;
        const uint64_t source_hash = FlatStore::hash_key(op.arg(1));
        // Before the pointer is taken: promoting replaces an embedded zset object.
        zset_sort_promote(shard, source_hash, op.arg(1), kNotify);
        KvObj* object = shard.store_find_read<kNotify>(source_hash, op.arg(1));
        if (object && static_cast<Type>(object->type) != Type::List &&
            static_cast<Type>(object->type) != Type::Set &&
            static_cast<Type>(object->type) != Type::Zset) {
            reply_wrongtype(op.sink());
            return;
        }
        if (object && !serialize_object(object, source)) { set_oom(op); return; }
        std::vector<std::string> values;
        std::vector<uint8_t> present;
        bool conversion_error = false;
        // An actual `*` dereference is structurally kept off this local arm and lowered through the
        // scatter engine. This immediate path covers plain SORT plus BY/GET forms that read no key.
        const WorkError sorted = sort_image(source, op, options, nullptr,
                                            values, present, conversion_error);
        if (sorted != WorkError::None) { reply_work_error(op, sorted); return; }
        if (conversion_error) {
            reply_err(op.sink(), "ERR One or more scores can't be converted into double");
            return;
        }
        if (!options.store) {
            sort_reply_rows(op, values, present);
            return;
        }
        ObjectImage destination;
        if (!values.empty() && !sort_encode_store(values, present, destination)) {
            set_oom(op);
            return;
        }
        const Slice key = op.arg(options.store_arg);
        const WorkError stored = apply_image<kNotify>(
            shard, key, FlatStore::hash_key(key), destination, true);
        if (stored != WorkError::None) reply_work_error(op, stored);
        else {
            if constexpr (kNotify)
                notify_record(shard, op, values.empty() ? NOTIFY_GENERIC : NOTIFY_LIST,
                              values.empty() ? NotifyEventId::Del : NotifyEventId::Sortstore, key);
            reply_int(op.sink(), static_cast<long long>(values.size()));
        }
        return;
    }

    // LCS's own local arm. It must precede the generic gather, which is bounded by argc and would
    // otherwise serialize the trailing option words as if they were keys. Same DP, same reply
    // builder as the cross-shard path -- one implementation, so the two cannot diverge.
    if (kind == Kind::Lcs) {
        LcsOptions options;
        if (!lcs_parse_options(op, options)) return;
        std::string values[2];
        for (uint32_t i = 0; i < 2; i++) {
            const Slice key = op.arg(1 + i);
            KvObj* object = shard.store_find_read<kNotify>(
                i == 0 ? op.hash : FlatStore::hash_key(key), key);
            if (!object) continue;
            if (static_cast<Type>(object->type) != Type::String) {
                reply_err(op.sink(), "ERR The specified keys must contain string values");
                return;
            }
            if (object->is_int()) {
                char integer[24];
                values[i].assign(integer, i64_to_dec(integer, object->int_value()));
            } else {
                KvObjRawReadBuffer raw;
                const Slice value = kvobj_string_value(object, raw);
                try { values[i].assign(value.p, value.n); }
                catch (const std::bad_alloc&) { set_oom(op); return; }
            }
        }
        LcsResult result;
        const char* message = nullptr;
        if (!lcs_compute(values[0], values[1], options, result, message)) {
            reply_err(op.sink(), message ? message : "ERR LCS failed");
            return;
        }
        lcs_reply(op, options, result);
        return;
    }

    std::vector<ObjectImage> images, apply;
    try { images.resize(op.argc()); apply.resize(op.argc()); }
    catch (const std::bad_alloc&) { set_oom(op); return; }
    uint32_t gather_first = kind == Kind::Bitop ? 3 :
                            is_store_zsetop(kind) ? 3 :
                            is_plain_zsetop(kind) || kind == Kind::Zintercard ? 2 :
                            is_store_setop(kind) ? 2 :
                            kind == Kind::Sintercard ? 2 : 1;
    uint32_t gather_end = op.argc();
    if (kind == Kind::Rename || kind == Kind::Renamenx || kind == Kind::Copy ||
        kind == Kind::Smove || kind == Kind::Lmove || kind == Kind::Rpoplpush)
        gather_end = 3;
    if (kind == Kind::Sintercard) {
        uint64_t n = 0; parse_u64(op.arg(1), n); gather_end = 2 + static_cast<uint32_t>(n);
    }
    uint32_t local_zset_count = 0;
    uint64_t local_zset_limit = 0;
    std::vector<double> local_zset_weights;
    ZAggregate local_zset_aggregate = ZAggregate::Sum;
    bool local_zset_withscores = false;
    if (is_any_zsetop(kind)) {
        if (!parse_zset_operation(op, kind, local_zset_count, local_zset_weights,
                                  local_zset_aggregate, local_zset_withscores,
                                  local_zset_limit)) return;
        gather_end = gather_first + local_zset_count;
    }
    const bool sources_are_reads = kind == Kind::Pfcount || kind == Kind::Pfmerge ||
                                   kind == Kind::Bitop || is_any_zsetop(kind) ||
                                   is_plain_setop(kind) || is_store_setop(kind) ||
                                   kind == Kind::Sintercard;
    for (uint32_t arg = gather_first; arg < gather_end; arg++) {
        const uint64_t hash = FlatStore::hash_key(op.arg(arg));
        const bool read_lookup = sources_are_reads || (kind == Kind::Copy && arg == 1);
        KvObj* object = read_lookup ? shard.store_find_read<kNotify>(hash, op.arg(arg))
                                    : shard.store_find<kNotify>(hash, op.arg(arg));
        if (!serialize_object(object, images[arg])) { set_oom(op); return; }
    }
    WorkError error = WorkError::None;
    if (kind == Kind::Rename || kind == Kind::Renamenx) {
        if (!images[1].present) { reply_err(op.sink(), "ERR no such key"); return; }
        if (same_key(op)) { if (kind == Kind::Rename) reply_ok(op.sink()); else reply_int(op.sink(), 0); return; }
        if (kind == Kind::Renamenx && images[2].present) { reply_int(op.sink(), 0); return; }
        try { apply[2] = images[1]; apply[1] = ObjectImage{}; }
        catch (const std::bad_alloc&) { set_oom(op); return; }
        local_apply<kNotify>(shard, op, apply, {2, 1}, error, 1);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify) {
                notify_record(shard, op, NOTIFY_GENERIC, NotifyEventId::RenameFrom, op.arg(1));
                notify_record(shard, op, NOTIFY_GENERIC, NotifyEventId::RenameTo, op.arg(2));
            }
            if (kind == Kind::Rename) reply_ok(op.sink()); else reply_int(op.sink(), 1);
        }
        return;
    }
    if (kind == Kind::Copy) {
        if (same_key(op)) { reply_err(op.sink(), "ERR source and destination objects are the same"); return; }
        bool replace = false;
        for (uint32_t i = 3; i < op.argc(); i++) if (op.arg(i).eq_icase("replace")) replace = true;
        if (!images[1].present || (images[2].present && !replace)) { reply_int(op.sink(), 0); return; }
        try {
            apply[2] = images[1];
            apply[2].reserve_ttl_slot = false;
        } catch (const std::bad_alloc&) { set_oom(op); return; }
        local_apply<kNotify>(shard, op, apply, {2}, error);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify)
                notify_record(shard, op, NOTIFY_GENERIC, NotifyEventId::CopyTo, op.arg(2));
            reply_int(op.sink(), 1);
        }
        return;
    }
    if (kind == Kind::Smove) {
        if (!images[1].present) { reply_int(op.sink(), 0); return; }
        if (!image_type(images[1], Type::Set) || !image_type(images[2], Type::Set)) {
            reply_wrongtype(op.sink()); return;
        }
        std::vector<std::string> src, dst;
        if (!decode_elements(images[1], src) || (images[2].present && !decode_elements(images[2], dst))) {
            set_oom(op); return;
        }
        if (!contains(src, op.arg(3))) { reply_int(op.sink(), 0); return; }
        if (same_key(op)) { reply_int(op.sink(), 1); return; }
        try { erase_member(src, op.arg(3)); if (!contains(dst, op.arg(3))) dst.emplace_back(op.arg(3).p, op.arg(3).n); }
        catch (const std::bad_alloc&) { set_oom(op); return; }
        if (src.empty()) apply[1] = ObjectImage{};
        else if (!encode_elements(src, apply[1], Type::Set, images[1].expire_at_ms,
                                  images[1].reserve_ttl_slot)) { set_oom(op); return; }
        if (!encode_elements(dst, apply[2], Type::Set,
                             images[2].present ? images[2].expire_at_ms : -1,
                             images[2].present && images[2].reserve_ttl_slot)) {
            set_oom(op); return;
        }
        local_apply<kNotify>(shard, op, apply, {1, 2}, error);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify) {
                notify_record(shard, op, NOTIFY_SET, NotifyEventId::Srem, op.arg(1));
                notify_record(shard, op, NOTIFY_SET, NotifyEventId::Sadd, op.arg(2));
            }
            reply_int(op.sink(), 1);
        }
        return;
    }
    if (kind == Kind::Lmove || kind == Kind::Rpoplpush) {
        if (!images[1].present) { reply_null(op.sink(), op.resp3()); return; }
        if (!image_type(images[1], Type::List) || !image_type(images[2], Type::List)) {
            reply_wrongtype(op.sink()); return;
        }
        std::vector<std::string> src, dst;
        if (!decode_elements(images[1], src) || (images[2].present && !decode_elements(images[2], dst))) {
            set_oom(op); return;
        }
        if (src.empty()) { reply_null(op.sink(), op.resp3()); return; }
        const bool from_left = kind == Kind::Lmove ? op.arg(3).eq_icase("left") : false;
        const bool to_left = kind == Kind::Lmove ? op.arg(4).eq_icase("left") : true;
        std::string moved = from_left ? src.front() : src.back();
        if (from_left) src.erase(src.begin()); else src.pop_back();
        if (same_key(op)) {
            if (to_left) src.insert(src.begin(), moved); else src.push_back(moved);
            if (!encode_elements(src, apply[1], Type::List, images[1].expire_at_ms,
                                 images[1].reserve_ttl_slot)) { set_oom(op); return; }
            local_apply<kNotify>(shard, op, apply, {1}, error);
        } else {
            if (to_left) dst.insert(dst.begin(), moved); else dst.push_back(moved);
            if (src.empty()) apply[1] = ObjectImage{};
            else if (!encode_elements(src, apply[1], Type::List, images[1].expire_at_ms,
                                      images[1].reserve_ttl_slot)) { set_oom(op); return; }
            if (!encode_elements(dst, apply[2], Type::List,
                                 images[2].present ? images[2].expire_at_ms : -1,
                                 images[2].present && images[2].reserve_ttl_slot)) {
                set_oom(op); return;
            }
            local_apply<kNotify>(shard, op, apply, {1, 2}, error);
        }
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify) {
                notify_record(shard, op, NOTIFY_LIST,
                              from_left ? NotifyEventId::Lpop : NotifyEventId::Rpop, op.arg(1));
                notify_record(shard, op, NOTIFY_LIST,
                              to_left ? NotifyEventId::Lpush : NotifyEventId::Rpush, op.arg(2));
            }
            reply_bulk(op.sink(), Slice(moved.data(), static_cast<uint32_t>(moved.size())));
            const Slice destination = op.arg(2);
            blocking_publish_key(shard, FlatStore::hash_key(destination),
                                 destination.p, destination.n);
        }
        return;
    }
    if (kind == Kind::Bitop) {
        ObjectImage output;
        long long output_length = 0;
        if (!compute_bitop(images.data(), 3, op.argc() - 3, op.arg(1), output,
                           output_length, error)) {
            reply_work_error(op, error);
            return;
        }
        const Slice destination = op.arg(2);
        const uint64_t hash = FlatStore::hash_key(destination);
        if (!output.present) {
            shard.store_erase<kNotify>(hash, destination, FlatStore::EraseEvent::None);
        } else {
            // Same contract as the scatter hop-2 arm: -1 clears TTL, raw encoding only.
            error = store_xstring<kNotify>(
                shard, destination, hash,
                Slice(reinterpret_cast<const char*>(output.payload.data()),
                      static_cast<uint32_t>(output.payload.size())),
                -1, false);
        }
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify)
                notify_record(shard, op, output.present ? NOTIFY_STRING : NOTIFY_GENERIC,
                              output.present ? NotifyEventId::Set : NotifyEventId::Del, destination);
            reply_int(op.sink(), output_length);
        }
        return;
    }
    if (is_any_zsetop(kind)) {
        ScatterState local;
        ResultHeap result;
        local.kind = kind;
        local.images = images.data();
        local.set_first = gather_first;
        local.set_count = local_zset_count;
        local.zset_limit = local_zset_limit;
        local.zset_weights = std::move(local_zset_weights);
        local.zset_aggregate = local_zset_aggregate;
        local.zset_withscores = local_zset_withscores;
        local.result = &result;
        if (!compute_zsetop(local)) { reply_work_error(op, local.final_error); return; }
        if (kind == Kind::Zintercard) {
            reply_int(op.sink(), static_cast<long long>(result.members.size()));
            return;
        }
        if (is_plain_zsetop(kind)) {
            reply_array_header(op.sink(), result.members.size() *
                (local_zset_withscores && !op.resp3() ? 2 : 1));
            for (size_t i = 0; i < result.members.size(); i++) {
                if (local_zset_withscores && op.resp3()) reply_array_header(op.sink(), 2);
                reply_bulk(op.sink(), Slice(result.members[i].data(),
                                             static_cast<uint32_t>(result.members[i].size())));
                if (local_zset_withscores) reply_double(op.sink(), result.scores[i], op.resp3());
            }
            return;
        }
        std::vector<ZImageItem> output;
        try {
            output.reserve(result.members.size());
            for (size_t i = 0; i < result.members.size(); i++) {
                double score = result.scores[i];
                if (score == 0) score = std::abs(score);
                output.push_back({result.members[i], score});
            }
        } catch (const std::bad_alloc&) { set_oom(op); return; }
        if (!output.empty() && !encode_zset_image(output, apply[1])) { set_oom(op); return; }
        local_apply<kNotify>(shard, op, apply, {1}, error, 1);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            if constexpr (kNotify) {
                const NotifyEventId event = kind == Kind::Zunionstore
                    ? NotifyEventId::Zunionstore : kind == Kind::Zinterstore
                    ? NotifyEventId::Zinterstore : NotifyEventId::Zdiffstore;
                notify_record(shard, op, output.empty() ? NOTIFY_GENERIC : NOTIFY_ZSET,
                              output.empty() ? NotifyEventId::Del : event, op.arg(1));
            }
            reply_int(op.sink(), static_cast<long long>(output.size()));
        }
        local.result = nullptr;
        return;
    }
    if (is_plain_setop(kind) || kind == Kind::Sintercard || is_store_setop(kind)) {
        uint32_t first = is_store_setop(kind) ? 2 : kind == Kind::Sintercard ? 2 : 1;
        uint32_t count = 0;
        if (kind == Kind::Sintercard) { uint64_t parsed = 0; parse_u64(op.arg(1), parsed); count = parsed; }
        else count = op.argc() - first;
        std::vector<std::string> members;
        if (!compute_local_setop(kind, images, first, count, members, error)) {
            reply_work_error(op, error); return;
        }
        if (kind == Kind::Sintercard) {
            uint64_t limit = 0;
            const uint32_t tail = first + count;
            if (tail < op.argc()) parse_u64(op.arg(tail + 1), limit);
            uint64_t result = members.size();
            if (limit && result > limit) result = limit;
            reply_int(op.sink(), static_cast<long long>(result));
            return;
        }
        if (is_plain_setop(kind)) {
            reply_set_header(op.sink(), members.size(), op.resp3());
            for (const std::string& member : members)
                reply_bulk(op.sink(), Slice(member.data(), static_cast<uint32_t>(member.size())));
            return;
        }
        if (members.empty()) apply[1] = ObjectImage{};
        else if (!encode_elements(members, apply[1], Type::Set, -1)) { set_oom(op); return; }
        local_apply<kNotify>(shard, op, apply, {1}, error, 1);
        if (error != WorkError::None) reply_work_error(op, error);
        else {
            NotifyEventId event = NotifyEventId::Sinterstore;
            if (kind == Kind::Sunionstore) event = NotifyEventId::Sunionstore;
            else if (kind == Kind::Sdiffstore) event = NotifyEventId::Sdiffstore;
            if constexpr (kNotify)
                notify_record(shard, op, members.empty() ? NOTIFY_GENERIC : NOTIFY_SET,
                              members.empty() ? NotifyEventId::Del : event, op.arg(1));
            reply_int(op.sink(), static_cast<long long>(members.size()));
        }
        return;
    }
    reply_err(op.sink(), "ERR internal cross-shard completion error");
}

void cmd_xshard_only(Shard& shard, Op& op) {
    cmd_xshard_only_impl<false>(shard, op);
}

void cmd_xshard_only_notify_body(Shard& shard, Op& op) {
    cmd_xshard_only_impl<true>(shard, op);
}

void cmd_xshard_only_notify(Shard& shard, Op& op) {
    notify_execute_handler(shard, op, cmd_xshard_only_notify_body);
}

}  // namespace tomo
