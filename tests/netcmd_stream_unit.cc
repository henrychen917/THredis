// Compiling the handler in this TU exposes its private PEL representation to a precise allocator
// fault. No production hook or running server is involved.
#include "netcmd_unit.h"
#include "src/cmd/t_stream_groups.cc"

void test_stream_faults() {
    tomo::Shard shard;
    shard.init_private(nullptr, 0, tomo::TypeLimits{}, tomo::StreamLimits{});
    shard.set_cached_now_ms(1000);
    check(execute(shard, {"XADD", "s", "1-0", "f", "one"}).starts_with("$3"), "first entry");
    check(execute(shard, {"XADD", "s", "2-0", "f", "two"}).starts_with("$3"), "second entry");
    check(execute(shard, {"XGROUP", "CREATE", "s", "g", "0"}) == "+OK\r\n", "group created");
    check(execute(shard, {"XGROUP", "CREATECONSUMER", "s", "g", "c"}) == ":1\r\n", "consumer exists");
    tomo::Op op; args(op, {"XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS", "s", ">"});
    // This is the libstdc++ node actually used by PendingMap, not an assumed allocation size.
    fault(1, sizeof(std::_Rb_tree_node<tomo::PendingMap::value_type>));
    tomo::stream_xreadgroup_execute(shard, op);
    const unsigned failures = netcmd_failures;
    fault(-1);
    check(failures == 1, "second PEL node allocation failed");
    check(std::string(op.reply.data(), op.reply.size()).starts_with("-ERR"), "failed delivery reports error");
    auto* object = shard.store().find(tomo::FlatStore::hash_key(tomo::Slice("s", 1)), tomo::Slice("s", 1));
    auto* group = tomo::find_group(object, tomo::Slice("g", 1));
    check(group && group->pending.empty(), "failed batch published no PEL prefix");
    check(group->last_delivered.ms == 0, "failed batch did not advance delivery cursor");
    const std::string retry = execute(shard, {"XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS", "s", ">"});
    check(retry.find("1-0") != std::string::npos && retry.find("2-0") != std::string::npos,
          "retry delivers both entries");
    check(group->pending.size() == 2, "both delivered entries are pending");

    shard.set_notify_mask(tomo::NOTIFY_SAVE);
    tomo::Op create; args(create, {"XGROUP", "CREATECONSUMER", "s", "g", "new"});
    const auto before = shard.save_changes();
    create.spec = tomo::command_notify_variant(create.spec);
    create.spec->handler(shard, create);
    check(std::string(create.reply.data(), create.reply.size()) == ":1\r\n", "new consumer inserted");
    check(shard.save_changes() == before + 1, "existing stream group mutation observed by save");
}
