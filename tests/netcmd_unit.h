#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#include "src/cmd/command.h"
#include "src/core/shard.h"
#include "src/exec/op.h"

extern long netcmd_fail_after;
extern size_t netcmd_fail_size;
extern unsigned netcmd_allocations, netcmd_failures;
inline void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
inline void fault(long after, size_t size = 0) {
    netcmd_fail_after = after; netcmd_fail_size = size;
    netcmd_allocations = netcmd_failures = 0;
}
inline void args(tomo::Op& op, std::initializer_list<const char*> values) {
    for (const char* value : values)
        check(op.push_arg(tomo::Slice(value, std::strlen(value))), "argument allocation");
    op.spec = tomo::command_lookup(op.cmd_name());
    check(op.spec != nullptr, "registered command");
    const int first = op.spec->first_key;
    if (first > 0) op.hash = tomo::FlatStore::hash_key(op.arg(first));
    op.shard = 0;
}
inline std::string execute(tomo::Shard& shard, std::initializer_list<const char*> values) {
    tomo::Op op; args(op, values); op.spec->handler(shard, op);
    return std::string(op.reply.data(), op.reply.size());
}
void test_stream_faults();
void test_zpop_faults();
void test_config_rewrite();
void test_config_bounds();
