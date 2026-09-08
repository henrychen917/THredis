#!/usr/bin/env python3
"""ARMED-WRITE BLOCK CACHE churn stressor (src/store/kv_block_cache.h).

Usage: rlcache_churn.py HOST PORT [SECONDS] [WORKERS]
  boot: --thread-mode 1s|2s --read-local 1 --atomic 1 --enable-debug-command yes

WHAT THIS DRIVES, AND WHY THE DIFFERENTIAL MATRIX DOES NOT. While read-local is armed a published
object is immutable, so every write BUILDS a fresh object, publishes it, and retires the displaced
one through QSBR; after the grace floor passes, the block is offered to the fused owner's private
block cache and the next write of the same size class TAKES it back. The differential matrix
compares replies -- it walks broad command surface at modest depth and rarely rewrites one key
enough times to cycle a block through put -> grace -> take more than a handful of times.

This battery does the opposite: a small key space, rewritten continuously at sizes chosen so the
displaced block is exactly the class the next write asks for, from many connections at once, with
the reads that ARM the lane interleaved on the same connection. That is the tightest possible loop
through the cache, which is where the corruption this reproduces lives:

  * a block that is put() into a class list twice splices a CYCLE into that list;
  * take() then hands the same block out while it is still linked;
  * the caller initialises it as a KvObj, whose header word overwrites the list `next`;
  * the next take() either aborts on the allocation check or dereferences a wild head.

SIZE CLASSES. kv_block_class() is injective over good_size() outputs; below 128 bytes the class is
allocation/16. The value lengths below therefore land in several distinct classes, and each key
keeps ITS size for the whole run so that its own retired block is always the exact class its next
write wants -- a guaranteed take() rather than an allocator call.

THE THREE RETIRE PATHS are all driven, because they use different reclaim callbacks:
  * point writes (SET/INCR)          -> read_local_reclaim_object
  * atomic group writes (MULTI/MSET) -> read_local_reclaim_atomic_object
  * expiry and FLUSH                 -> the same ring, plus release_all() on the whole cache

NON-VACUITY. A run in which the armed lane never served a read, or in which the cache never held a
block, proves nothing about either. Both are asserted from INFO deltas over this battery's own
traffic; a pass is refused otherwise.
"""
import os
import sys
import threading
import time

import _lib

# BISECT SWITCHES. Each names one retire path the battery drives; setting it to 0 removes exactly
# that path and nothing else, so a reproduction that survives every removal but one has named its
# cause. They exist for the P0 bisect and default to on.
DO_EXPIRE = os.environ.get("RLCHURN_EXPIRE", "1") == "1"
DO_ATOMIC = os.environ.get("RLCHURN_ATOMIC", "1") == "1"
DO_FLUSH  = os.environ.get("RLCHURN_FLUSH",  "1") == "1"
DO_INT    = os.environ.get("RLCHURN_INT",    "1") == "1"
DO_READ   = os.environ.get("RLCHURN_READ",   "1") == "1"

VALUE_SIZES = (0, 8, 24, 40, 56, 72, 88, 104)   # spread across kv_block_class buckets below 128
KEYSPACE = int(os.environ.get("RLCHURN_KEYSPACE", "192"))  # small enough that every key is rewritten constantly
BURST = 32                                       # pipelined GET+SET pairs per round trip
HOT = 6                                          # keys in the rotating hot window
HOT_SHARE = 4                                    # 3 of every 4 ops land in the window


# THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
# shard ever changes owner -- which is the precondition for the defect this battery exists for
# (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
# different ones hot, so the balancer moves shards continuously and in both directions, which is
# exactly the traffic under which the crash was first seen.
def hot_key(keyspace, i, salt):
    n = len(keyspace)
    if i % HOT_SHARE:
        base = int(time.time() * 2) * 13
        return keyspace[(base + (i + salt) % HOT) % n]
    return keyspace[(i * 7 + salt) % n]


# CONNECTION HOMOGENEITY. A connection that carries writes fences its own reads: the RYOW ring
# holds a descriptor for every write it has in flight, and a read that overlaps one is demoted to
# the owner. Mixing both on one connection therefore drives the cache but leaves the ARMED LOCAL
# READ -- the reader holding a pointer into an object the owner is concurrently retiring -- almost
# unexercised (measured: 210 local hits across 6M GETs). Readers and writers are split onto
# separate connections over ONE key space so that both halves run at full rate against each other.
def reader(host, port, wid, keyspace, stop, errors, counts):
    try:
        conn = _lib.Conn(host, port)
        n = len(keyspace)
        ops = 0
        rnd = 0
        while not stop.is_set():
            rnd += 1
            frame = []
            expect = 0
            base = (rnd * 7 + wid) % n
            for i in range(BURST):
                frame.append(_lib.encode("GET", hot_key(keyspace, i, wid)))
                expect += 1
            # MGET takes the multi-key local-read path, which holds several foreign pointers at once.
            frame.append(_lib.encode("MGET", keyspace[base % n], keyspace[(base + 1) % n],
                                     keyspace[(base + 2) % n], keyspace[(base + 3) % n]))
            expect += 1
            conn.raw(b"".join(frame))
            for _ in range(expect):
                conn.read()
            ops += expect
        counts[wid] = ops
        conn.close()
    except Exception as exc:                                    # noqa: BLE001
        errors.append("reader %d: %r" % (wid, exc))


def writer(host, port, wid, keyspace, stop, errors, counts):
    try:
        conn = _lib.Conn(host, port)
        n = len(keyspace)
        # Each key keeps one size for the whole run: its retired block is then always exactly the
        # class its next write asks for, so the write TAKES from the cache instead of allocating.
        vals = [("v%d" % i) * (VALUE_SIZES[i % len(VALUE_SIZES)] // 4 + 1) for i in range(n)]
        ops = 0
        rnd = 0
        while not stop.is_set():
            rnd += 1
            frame = []
            expect = 0
            base = (rnd * 5 + wid * 3) % n
            for i in range(BURST):
                k = hot_key(keyspace, i, wid)
                j = keyspace.index(k)
                frame.append(_lib.encode("SET", k, vals[j]))
                expect += 1
                if DO_INT and i % 4 == 3:
                    # Int-encoded objects reach the same cache through make_set_int.
                    frame.append(_lib.encode("SET", keyspace[j], str(1000 + j)))
                    frame.append(_lib.encode("INCR", keyspace[j]))
                    expect += 2
            if DO_ATOMIC and rnd % 3 == 0:
                # Atomic group write: read_local_reclaim_atomic_object, the other callback.
                frame.append(_lib.encode("MSET", keyspace[base % n], vals[base % n],
                                         keyspace[(base + 1) % n], vals[(base + 1) % n],
                                         keyspace[(base + 2) % n], vals[(base + 2) % n]))
                expect += 1
            if DO_EXPIRE and rnd % 11 == 0:
                # Expiry-driven retire: the sampler frees these, not a write.
                frame.append(_lib.encode("SET", keyspace[(base + 4) % n], vals[(base + 4) % n],
                                         "PX", "1"))
                frame.append(_lib.encode("PEXPIRE", keyspace[(base + 5) % n], "1"))
                expect += 2
            conn.raw(b"".join(frame))
            for _ in range(expect):
                conn.read()
            ops += expect
        counts[wid] = ops
        conn.close()
    except Exception as exc:                                    # noqa: BLE001
        errors.append("writer %d: %r" % (wid, exc))


def main():
    host, port = _lib.host_port(default_port=6379)
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
    workers = int(sys.argv[4]) if len(sys.argv) > 4 else 24
    rep = _lib.Report("rlcache-churn")

    ctl = _lib.Conn(host, port)
    mode = _lib.thread_mode(ctl)
    if mode not in ("1s", "2s"):
        rep.bad("geometry", "unknown thread_mode:%s" % mode)
        return rep.finish()
    if _lib.info(ctl, "server").get("read_local") != "1":
        rep.bad("geometry", "needs --read-local 1")
        return rep.finish()
    if _lib.info_int(ctl, "server", "read_local_active_threads") == 0:
        rep.bad("geometry", "read-local enabled but no reader lane is active")
        return rep.finish()

    before_hits = _lib.info_int(ctl, "all", "read_local_hits")
    # SHARD MOVES ARE THE PRECONDITION. The defect this battery exists for lives at the ownership
    # edge: a shard changing owner while its read-local retire sink still names the old owner's
    # QSBR ring and block cache. A run in which the load balancer moved nothing cannot have
    # exercised it, and must not be reported as evidence that it is fixed.
    before_moves = _lib.info_int(ctl, "all", "tomokv_keylb_bucket_moves")
    peak_cache = 0
    stop = threading.Event()
    errors = []
    counts = [0] * workers
    # One shared key space: every reader can be serving a key some writer is retiring.
    keyspace = ["ch:%d" % i for i in range(KEYSPACE)]
    for i in range(0, KEYSPACE, 64):
        ctl.raw(b"".join(_lib.encode("SET", keyspace[j], "seed")
                         for j in range(i, min(i + 64, KEYSPACE))))
        for _ in range(min(64, KEYSPACE - i)):
            ctl.read()
    threads = []
    for w in range(workers):
        fn = reader if (DO_READ and w % 2 == 0) else writer
        threads.append(threading.Thread(target=fn,
                                        args=(host, port, w, keyspace, stop, errors, counts)))
    for t in threads:
        t.start()

    deadline = time.time() + seconds
    flushes = 0
    while time.time() < deadline and not errors:
        time.sleep(0.25)
        try:
            peak_cache = max(peak_cache, _lib.info_int(ctl, "all", "mem_block_cache"))
        except Exception:                                        # noqa: BLE001
            break
        # release_all() on a live cache, interleaved with the workers' take()/put().
        if DO_FLUSH and int((deadline - time.time()) * 4) % 20 == 0:
            try:
                ctl.must("FLUSHALL")
                flushes += 1
            except Exception:                                    # noqa: BLE001
                break
    stop.set()
    for t in threads:
        t.join(timeout=30)

    rep.check("workers completed", not errors, "; ".join(errors[:3]))
    alive = False
    try:
        alive = _lib.call(ctl, "PING") in (b"PONG", "PONG")
    except Exception as exc:                                     # noqa: BLE001
        errors.append("post-run PING: %r" % exc)
    rep.check("server alive after churn", alive, "; ".join(errors[:3]))

    ops = sum(counts)
    if alive:
        after_hits = _lib.info_int(ctl, "all", "read_local_hits")
        rep.check("armed lane served reads (non-vacuity)", after_hits > before_hits,
                  "read_local_hits delta %d" % (after_hits - before_hits))
        rep.check("block cache held blocks (non-vacuity)", peak_cache > 0,
                  "peak mem_block_cache %d bytes" % peak_cache)
        # Ownership movement is the required precondition, including with automatic LB policy.
        after_moves = _lib.info_int(ctl, "all", "tomokv_keylb_bucket_moves")
        rep.check("load balancer moved shards (non-vacuity)", after_moves > before_moves,
                  "tomokv_keylb_bucket_moves delta %d -- the automatic balancer must move "
                  "shards; retain more shards than workers" % (after_moves - before_moves))
    print("  churn: ops=%d workers=%d flushes=%d peak_block_cache=%d"
          % (ops, workers, flushes, peak_cache))
    return rep.finish()


if __name__ == "__main__":
    sys.exit(main())
