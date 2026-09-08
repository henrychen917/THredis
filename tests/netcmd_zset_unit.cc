#include "netcmd_unit.h"
#include "src/cmd/t_zset.cc"

void test_zpop_faults() {
    for (bool maximum : {false, true}) for (long fail_at : {0L, 1L}) {
        tomo::Shard shard;
        shard.init_private(nullptr, 0, tomo::TypeLimits{}, tomo::StreamLimits{});
        check(execute(shard, {"ZADD", "z", "1", "m"}) == ":1\r\n", "seed compact zset");
        auto* object = shard.store().find(tomo::FlatStore::hash_key(tomo::Slice("z", 1)), tomo::Slice("z", 1));
        check(tomo::CollectionRef(object).encoding() == tomo::CollectionEncoding::Compact, "compact arm entered");
        tomo::Op op; args(op, {maximum ? "ZPOPMAX" : "ZPOPMIN", "z"});
        fault(fail_at);
        op.spec->handler(shard, op);
        const auto failures = netcmd_failures, allocations = netcmd_allocations;
        fault(-1);
        const std::string reply(op.reply.data(), op.reply.size());
        if (fail_at == 0) {
            check(failures == 1 && reply.starts_with("-ERR"), "first compact load failure reported");
            check(execute(shard, {"ZCARD", "z"}) == ":1\r\n", "failed load leaves member");
        } else {
            check(allocations == 1 && failures == 0, "pop reuses the first loaded index without a second allocation");
            check(reply == "*2\r\n$1\r\nm\r\n$1\r\n1\r\n", "pop reply identifies member");
            check(execute(shard, {"ZCARD", "z"}) == ":0\r\n", "successful pop removed member");
        }
    }
}
