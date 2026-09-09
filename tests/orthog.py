#!/usr/bin/env python3
"""Prove one externally started mode cell, including actual multi-client permutations.

orthog.py HOST PORT 1s|2s READ_LOCAL OVERLAP REORDER [--output FILE]
Requires atomic 1, DEBUG enabled, key/client LB and automatic FLIP disabled.
No throughput measurement, server lifecycle, or gate invocation lives here.
"""
import argparse
import concurrent.futures
import json
import threading
import time

import _lib


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def burst(conn, commands, expected):
    conn.raw(b"".join(_lib.encode(*command) for command in commands))
    actual = [conn.read() for _ in commands]
    require(actual == expected, "ordered replies differ: %r != %r" % (actual, expected))


def reader_rows(info):
    return {int(key.removeprefix("read_local_thread_")):
            dict(part.split("=", 1) for part in value.split(","))
            for key, value in info.items() if key.startswith("read_local_thread_")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("mode", choices=("1s", "2s"))
    for name in ("read_local", "overlap", "reorder"):
        parser.add_argument(name, type=int, choices=(0, 1))
    parser.add_argument("--output")
    args = parser.parse_args()
    ctl = _lib.Conn(args.host, args.port, timeout=20)
    opened = []
    keys = []
    try:
        cfg = ctl.must("CONFIG", "GET", "*")
        cfg = dict(zip(cfg[::2], cfg[1::2]))
        expected = {"thread-mode": args.mode, "read-local": args.read_local,
                    "overlap": args.overlap, "reorder": args.reorder, "atomic": 1,
                    "key-lb": 0, "client-lb": 0, "flip-auto": 0}
        for name, value in expected.items():
            require(cfg.get(name.encode()) == str(value).encode(), "CONFIG mismatch: " + name)
        for _ in range(200):
            before = _lib.info(ctl, "server")
            expected_active = int(before["io_threads"]) if args.mode == "2s" else int(before["fused_threads"])
            if not args.read_local or int(before.get("read_local_active_threads", 0)) == expected_active:
                break
            time.sleep(0.01)
        for name in ("thread_mode", "read_local", "overlap", "reorder", "atomic"):
            wanted = args.mode if name == "thread_mode" else 1 if name == "atomic" else getattr(args, name)
            require(before[name] == str(wanted), "INFO mismatch: " + name)
        nthreads = int(before["io_threads"]) + int(before["ex_threads"]) if args.mode == "2s" else int(before["fused_threads"])
        require(int(before.get("schedule_stats_threads", 0)) == (nthreads if args.overlap or args.reorder else 0),
                "disabled schedule knobs allocated witnesses, or enabled witnesses are absent")
        topo = _lib.topology(ctl)
        prefix = "orthog:%d" % time.time_ns()
        a, b, _, _ = _lib.cross_owner_pair(ctl, prefix + ":local")
        keys += [a, b]
        value = b"immutable:" + b"v" * 256
        require(ctl.must("SET", a, value) == ctl.must("SET", b, value) == b"OK", "seed failed")
        initial_rows = reader_rows(before)
        if args.read_local:
            active = {tid for tid, row in initial_rows.items() if row["active"] == "1"}
            require(len(active) == expected_active, "not every reader loop entered its lane")
            for tid, row in initial_rows.items():
                owned = list(topo.shard_owner.values()).count(tid)
                require(int(row["shards"]) == owned, "INFO ownership mismatch")
                if args.mode == "2s":
                    require((row["role"] == "ifid") == (tid in active), "role/lane mismatch")
                    require(tid not in active or owned == 0, "split IO owns shards")
            found = set()
            for _ in range(32 + 16 * len(active)):
                conn = _lib.Conn(args.host, args.port, timeout=20)
                opened.append(conn)
                tid = int(conn.must("DEBUG", "IO-THREAD"))
                if tid in found:
                    continue
                require(tid in active, "a client reached an inactive reader")
                burst(conn, [("GET", a)] * 32 + [("MGET", a, b)] * 16,
                      [value] * 32 + [[value, value]] * 16)
                found.add(tid)
                if found == active:
                    break
            require(found == active, "fresh connections did not reach every reader")
            local_info = _lib.info(ctl, "server")
            local_rows = reader_rows(local_info)
            for tid in active:
                require(int(local_rows[tid]["hits_total"]) - int(initial_rows[tid]["hits_total"]) == 48,
                        "reader %d did not complete all 48 clean local reads: %r -> %r; %r" %
                        (tid, initial_rows[tid], local_rows[tid], _lib.info(ctl, "stats")))
                require(int(local_rows[tid]["mget_hits_total"]) - int(initial_rows[tid]["mget_hits_total"]) == 16,
                        "reader %d did not complete local MGET" % tid)
            if args.mode == "2s":
                require(all(int(row["hits_total"]) == 0 for tid, row in local_rows.items() if tid not in active),
                        "an executor served a local read before any FLIP")
        else:
            require(not initial_rows and "read_local_active_threads" not in before,
                    "disabled read-local retained lane telemetry")
            local_info = _lib.info(ctl, "server")
        for conn in opened:
            conn.close()
        opened.clear()

        # Every command below reaches an owner, including armed boots. Fresh independent keys
        # share a real owner; 8-deep concurrent batches permit legal cross-client permutations.
        # An absent witness gets fresh keys/connections, bounded; wrong replies never get retried.
        start = _lib.info(ctl, "server")
        for attempt in range(4):
            owner = min(topo.owners)
            chosen = []
            for key, _sid, tid in _lib.probe_keys(ctl, prefix + ":reorder:%d" % attempt, topo, limit=16000):
                if tid == owner:
                    chosen.append(key)
                if len(chosen) == 64:
                    break
            require(len(chosen) == 64, "could not find 64 keys on one owner")
            keys += chosen
            barrier = threading.Barrier(len(chosen))
            def worker(item):
                index, key = item
                long_command = index % 2 == 0
                conn = _lib.Conn(args.host, args.port, timeout=20)
                try:
                    require(conn.must("SET", key, b"x" * 65536 if long_command else b"0") == b"OK",
                            "counter seed failed")
                    barrier.wait(timeout=20)
                    for turn in range(12):
                        # Neither command is eligible for local reads. Long/point heads also
                        # exercise class priority when a gathered run contains only rank-zero tasks.
                        if long_command:
                            burst(conn, [("BITCOUNT", key)] * 8, [262144] * 8)
                        else:
                            burst(conn, [("INCR", key)] * 8, list(range(turn * 8 + 1, turn * 8 + 9)))
                finally:
                    conn.close()
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(chosen)) as pool:
                list(pool.map(worker, enumerate(chosen)))
            final = _lib.info(ctl, "server")
            if not args.reorder or (int(final["reorder_permuted_runs"]) > int(start["reorder_permuted_runs"])):
                break
        if args.reorder:
            require(int(final["reorder_batches"]) > int(start["reorder_batches"]), "reorder was never called")
            require(int(final["reorder_multi_client_runs"]) > int(start["reorder_multi_client_runs"]),
                    "reorder never saw several clients in one eligible run")
            require(int(final["reorder_permuted_runs"]) > int(start["reorder_permuted_runs"]),
                    "no real permutation after four fresh arms: %r" %
                    {key: value for key, value in final.items() if key.startswith("reorder") })
        else:
            require(all(int(final.get(name, 0)) == 0 for name in
                        ("reorder_batches", "reorder_multi_client_runs", "reorder_permuted_runs", "reorder_max_batch")),
                    "reorder ran while disabled")
        schedule = "plain" if not args.overlap else "split-io-overlap" if args.mode == "2s" else "fused-overlap"
        require(final.get("overlap_schedule", "plain") == schedule, "actual schedule differs from requested mode")
        if args.overlap:
            require(int(final["overlap_passes"]) > int(start["overlap_passes"]), "overlap did not run")
            require(int(final["overlap_interleaved_passes"]) > 0, "overlap never entered its interleaved arm")
        else:
            require(int(final.get("overlap_passes", 0)) == int(final.get("overlap_interleaved_passes", 0)) == 0,
                    "overlap ran while disabled")
        if not args.read_local:
            require(int(_lib.info(ctl, "stats")["read_local_hits"]) == 0, "disabled lane completed reads")
        evidence = {"requested": expected, "before": before, "local_info": local_info,
                    "scheduler_before": start, "after": final, "reorder_arm_attempts": attempt + 1}
        if args.output:
            with open(args.output, "w") as f:
                json.dump(evidence, f, indent=2, sort_keys=True)
                f.write("\n")
        print("PASS orthog %s/%d/%d/%d: active=%s schedule=%s passes=%s interleaved=%s reorder=%s/%s/%s max=%s" %
              (args.mode, args.read_local, args.overlap, args.reorder,
               local_info.get("read_local_active_threads", "0"), schedule,
               final.get("overlap_passes", "0"), final.get("overlap_interleaved_passes", "0"), final.get("reorder_batches", "0"),
               final.get("reorder_multi_client_runs", "0"), final.get("reorder_permuted_runs", "0"), final.get("reorder_max_batch", "0")))
    finally:
        for conn in opened:
            conn.close()
        try:
            if keys:
                ctl.cmd("DEL", *keys)
        finally:
            ctl.close()


if __name__ == "__main__":
    main()
