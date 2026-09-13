# XACCT: stream-group and transaction accounting costs

Both assigned claims were reproduced on live servers before any production fix. Item A was a
PEL-depth walk in stream resident accounting. Item B was the linear duplicate scan in MULTI
preparation. The fixes are deliberately local: stream-group bytes are maintained by their owning
executor, and transaction write-key membership uses a hash set in the already side-allocated
`MultiExecState`. No `Op`, `Client`, command-registry, configuration, or persistence grammar
changed.

The prompt-referenced `scratchpad/wave3/PREAMBLE.md` is not present in this worktree (`rg --files
-g PREAMBLE.md` returned no files). I followed the complete lane preamble supplied in the task and
did not inspect another worktree to obtain a missing copy.

## Item A — stream accounting walks the whole PEL

**Verdict: REPRODUCED.** Each row is the median elapsed time per XADD from five pipelined samples.
Every setup checked `XPENDING key group` and rejected the sample unless its count equalled P.
TomoKV and Redis were one-thread-role/one-shard servers pinned to CPUs 16–17 and 18 respectively;
the load generator was pinned to CPU 19. Units are microseconds per XADD.

| PEL P | TomoKV before | Redis 7.4.2 | TomoKV after |
| ---: | ---: | ---: | ---: |
| 0 | 2.688 | 3.092 | 2.679 |
| 100 | 3.977 | 3.079 | 2.653 |
| 1,000 | 19.776 | 3.156 | 2.681 |
| 10,000 | 198.742 | 3.406 | 2.788 |
| 100,000 | 1,744.017 | 4.377 | 3.827 |

Before the fix, P=100k/P=0 was **648.8x** on TomoKV and **1.42x** on Redis. After the
fix it is **1.43x** on TomoKV. Redis is the unchanged reference measurement from the before run.

The requested call-count proof was a temporary counter in
`stream_groups_allocation_bytes()`, built and run separately from the latency binary. After group
and PEL setup the count was 3. One XADD produced exactly:

```text
XACCT_STREAM_WALK 4
XACCT_STREAM_WALK 5
XACCT_STREAM_WALK 6
XACCT_STREAM_WALK 7
```

Thus one live XADD caused four complete group/consumer/PEL walks: constructor and `finish()` for
each of its append and trim `ObjectSizeTracker` brackets.

### Fix

`StreamGroups::bytes_` starts at `sizeof(StreamGroups)`. Small owner-only helpers maintain the
same formula the old traversal used:

- group insert/delete, including nested consumers and pending entries;
- consumer insert/delete;
- pending insert, replacement, ACK, deleted-entry cleanup, and consumer deletion;
- consumer-string capacity changes at **both** `XCLAIM` and `XAUTOCLAIM` reassignment sites;
- snapshot decode insertions.

`stream_groups_allocation_bytes()` is now a null check and one field load. Streams without groups
still have a null pointer and allocate nothing. Snapshot decode performs one cold full traversal
and refuses the image if its independently recomputed total differs from the maintained total.
The directed test builds an identical no-group twin stream, proves its MEMORY delta is zero,
visits every mutation family, then compares the group-only `MEMORY USAGE` delta before and after
`DEBUG RELOAD`. The observed maintained and recomputed deltas were byte-identical: **1,434 bytes**.

## Item B — MULTI prepare duplicate scan

**Verdict: REPRODUCED.** The primary requested curve uses successful transactions of K unique,
single-key SET commands. Queuing is outside the interval; the timed interval sends EXEC and reads
its entire reply.

| K write-key instances | TomoKV before (us) | Redis 7.4.2 (us) | TomoKV after (us) |
| ---: | ---: | ---: | ---: |
| 100 | 128.540 | 86.730 | 125.410 |
| 1,000 | 4,167.960 | 595.041 | 3,027.308 |
| 5,000 | 86,326.334 | 3,023.798 | 59,792.183 |
| 10,000 | 371,439.964 | 6,101.135 | 254,901.729 |

Before the fix, TomoKV's 1k→10k step was **89.1x** for 10x more keys; Redis was **10.3x**.
Replacing the named scan removes it, but the successful after curve remains superlinear because
each command also installs and resolves a separate transaction MVCC pending record. That is a
different execution-path scaling cost, not duplicate detection. I did not redesign MVCC grouping:
the lane explicitly limits this fix to how the duplicate check is computed.

To measure the named preparation phase without that later execution cost, the regression arm
invalidates a WATCH before EXEC. TomoKV still builds/deduplicates every queued command, then the
owner proves the dirty WATCH and executes zero writes (`DBSIZE == 1` is asserted). A clean-WATCH
transaction is the negative control and must execute normally.

| K queued unique SETs | TomoKV before aborted EXEC (us) | Redis 7.4.2 (us) | TomoKV after (us) |
| ---: | ---: | ---: | ---: |
| 100 | 68.930 | 24.060 | 66.891 |
| 1,000 | 1,712.235 | 27.960 | 385.041 |
| 5,000 | 31,856.883 | 27.640 | 1,802.445 |
| 10,000 | 143,143.910 | 29.869 | 3,747.091 |

The targeted 1k→10k growth changed from **83.6x** to **9.73x**. A second exact gate cell at
2k→10k measured 4,248.113→144,148.718 us before (**33.93x**) and
723.942→4,637.814 us after (**6.41x**). `tests/xacct.py` uses this 5x-work shape and requires
growth below 15x; the original implementation fails that bound decisively.

### Fix

`MultiExecState` now owns an `unordered_set<MultiWriteKey>` keyed by the already-computed
FlatStore hash plus shard, with exact `(shard, string)` equality for collision safety. The first
occurrence is still copied to the existing ordered `write_keys` vector, so reservation, finalizer,
AOF, reply, and duplicate semantics do not change. Repeated-key directed coverage checks all child
replies and the final last-write-wins value. This is transaction-only side state; `Op` and `Client`
did not grow.

## Exact reproduction and verification commands

Representative commands (all server processes were resolved from `ss -lntp` before TERM, and the
listener was checked absent before reuse):

```sh
make -j12 clean && make -j12

taskset -c 16-17 build/tomokv-before --port 7106 --bind 127.0.0.1 \
  --shards 1 --place ifid@16,ex@17 --atomic 0
taskset -c 18 /tmp/claude-1000/redis74/src/redis-server \
  --port 7107 --bind 127.0.0.1 --save '' --appendonly no --protected-mode no
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7106 stream
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7107 stream
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7106 multi
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7107 multi
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7106 multi-abort
taskset -c 19 python3 scratchpad/xacct_measure.py 127.0.0.1 7107 multi-abort

taskset -c 16-19 build/tomokv --port 7106 --bind 127.0.0.1 --shards 4 \
  --place ifid@16,ifid@17,ex@18,ex@19 --atomic 0 \
  --enable-debug-command yes --dir /tmp/xacct-final0.U516gn --dbfilename xacct.tomo
taskset -c 20 python3 tests/xacct.py 127.0.0.1 7106
taskset -c 20 python3 tests/streamgroups.py 127.0.0.1 7106
taskset -c 20 python3 tests/multi_exec.py 127.0.0.1 7106
# Repeated with --atomic 1 and /tmp/xacct-final1.gwqNwI.

taskset -c 19 python3 tests/differ.py 127.0.0.1 7106 127.0.0.1 7107 stream
taskset -c 19 python3 tests/differ.py 127.0.0.1 7106 127.0.0.1 7107 streamgrp
taskset -c 19 python3 tests/differ.py 127.0.0.1 7106 127.0.0.1 7107 string

make asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
taskset -c 16-17 build/tomokv-asan --port 7106 --bind 127.0.0.1 \
  --shards 1 --place ifid@16,ex@17 --atomic 0 --enable-debug-command yes \
  --dir /tmp/xacct-asan.z6HgsV --dbfilename xacct.tomo
taskset -c 19 python3 tests/xacct.py 127.0.0.1 7106
```

## Test evidence

Final optimized directed tails:

```text
atomic 0:
  ok   XADD cost growth is bounded across a 10k PEL ... ratio=0.97x bound<4.00x
  ok   MULTI prepare growth is bounded for 5x queued write keys ... ratio=5.46x bound<15.00x
  ok   MEMORY USAGE group bytes equal cold recomputation before=1434 after=1434 reload=b'OK'
XACCT directed battery passed

atomic 1:
  ok   XADD cost growth is bounded across a 10k PEL ... ratio=1.01x bound<4.00x
  ok   MULTI prepare growth is bounded for 5x queued write keys ... ratio=6.19x bound<15.00x
  ok   MEMORY USAGE group bytes equal cold recomputation before=1434 after=1434 reload=b'OK'
XACCT directed battery passed
```

Existing directed batteries, both atomic modes:

```text
STREAMGROUPS PASS
MULTI/WATCH directed battery passed
```

`tests/xacct.py` is registered in the existing purpose-booted DEBUG section of `tests/gate.sh` so
its reload oracle runs under both atomic settings. Per the lane instruction, I did not run
`tests/gate.sh` itself (its port and cores are reserved for the mainline operator).

Differential tails (the required suites already existed, so no new differ suite was needed):

```text
DIFFER stream: 4031 ops, 0 diffs -> PASS
DIFFER streamgrp: 4052 ops, 0 diffs -> PASS
DIFFER string: 4033 ops, 0 diffs -> PASS
```

Sanitizer tail:

```text
  ok   XADD cost growth is bounded across a 10k PEL ... ratio=0.98x bound<4.00x
  ok   MULTI prepare growth is bounded for 5x queued write keys ... ratio=5.05x bound<15.00x
  ok   MEMORY USAGE group bytes equal cold recomputation before=1434 after=1434 reload=b'OK'
XACCT directed battery passed
```

The ASAN/UBSAN server terminated with exit 0 and emitted no `AddressSanitizer` or `runtime error:`
diagnostic. `make -j12 clean && make -j12` passed after all edits; its compile-time footprint locks
therefore passed too.

## Plain p32 GET/SET guard (INDICATIVE)

The static guard is exact. Normalized disassembly (absolute/RIP-relative link addresses removed)
of the clean handlers had identical before/after sizes and SHA-256 hashes:

| handler | size | normalized SHA-256 before and after |
| --- | ---: | --- |
| `cmd_get<false,true>` | `0x76d` | `c7853baed05102bca4ef0afa29a9efa8d069b96f6a7e2b4a3fdfa742b0e514e8` |
| `cmd_set<false>` | `0xa87` | `ac5f7287fa980d19153e22583c2e6a54b293bec182c5a8a71fe100e03517a79b` |

The loopback corroboration used server CPUs 16–17 and memtier CPUs 19–22, eight connections,
four load threads, 32-byte values, a 100k random-key space, pipeline 32, and three 3-second runs.
It is client/system-noise limited and is not a NIC result.

| cell | before runs (ops/s) | before median | after runs (ops/s) | after median | delta |
| --- | --- | ---: | --- | ---: | ---: |
| SET p32 | 2,305,236 / 2,297,649 / 2,321,586 | 2,305,236 | 2,247,872 / 2,283,902 / 2,289,092 | 2,283,902 | -0.93% |
| GET p32 | 2,435,071 / 2,409,156 / 2,446,652 | 2,435,071 | 2,348,175 / 2,355,225 / 2,370,956 | 2,355,225 | -3.28% |

The source changes are confined to stream-group handlers and the already-taken MULTI branch;
the identical clean-handler machine code is the load-independent proof that the plain path was
untouched.

## Knobs and scope

No runtime knob was added or changed, so `config.h` and `tomokv.conf` require no update. No scope
was silently dropped. The only shelved work is the separately observed successful-EXEC MVCC
pending-list scaling described under Item B; changing it would exceed the instruction to alter
only duplicate checking and would require its own reproduction/design lane.
