# DESIGN-RINGDIET.md — arm the RYOW write ring on demand

**Status: implemented exactly as specified below. The design was fixed before the lane opened;
this file states it, and the sections after it are the proof.**

## The measured fact this answers

The ring-sizing merge (`b0335c239`, *"RYOW ring sized to the ROB window"*) versus the commit before
it, on the owner's box — 512 connections, 64 shards, pipeline depth 32, fused with read-local
armed, 8 interleaved runs per arm, idle box:

| cell | delta | instructions/op |
|---|---|---|
| pure SET | **−1.32%** | **+87** |
| pure GET | +0.80% | — |
| 1:1 mix | −0.08% | — |

The ring lane's own numbers: restoring correct bookkeeping after an overflow costs **+43
instructions per write**, and the armed sidecar costs **+972.8 bytes per connection**.

### Mechanism

The old sixteen-entry RYOW ring **overflowed** on a pure-write stream at depth 32 — and then
stopped bookkeeping entirely. It was cheap for precisely the wrong reason: it gave up. Sizing the
ring to the reordering window (`kRobWindow`, 64) made overflow unreachable, so **every** write is
now recorded — including on connections where no read will ever consult the record.

Main commands are zero-regression by law, and pure SET is a main command.

## The design

### (1) Arm on demand

A connection records its in-flight writes **only after a local read has armed it**.

Until armed:

* a write's entire bookkeeping is **one store of its ROB id** into `read_local_unarmed_write_id_`,
  a word on the producer's own cache line that `dispatch_` has already dirtied. No sidecar, no
  prune, no descriptor, no `Staged` tag — and therefore no resolve on the following frame either;
* a read that arrives **while writes are in flight must take the OWNER path** (demote). Read-your-
  own-writes stays exact: a read and a write of one key share one owner queue, so the owner path
  preserves per-key order by construction. RYOW may be held back only on an explicit key conflict,
  and demotion is strictly stronger than the ring, not weaker.

The arming state lives on a cache line the parse path already owns (the `dispatch_` line, in the
padding that already held `read_local_write_valid_/wide_/force_`), and the check is a **single
predictable test on that line** — `read_local_arm_state_ != 0`, zero in the state every connection
ends up in. **Nothing is evaluated behind it**: the gate-the-argument law. A dormant mechanism that
pulls one cold line per pass cost 0.345 fills per op and 3.8% on a control cell earlier this week,
so the gate word had to be one this frame's `acquire_read_local` has already pulled into L1.

#### The arming transient, and why it is not the overflow generation

The writes a connection published *before* arming are described by nothing. So the read that arms
the connection, and every read behind it, is demoted until the **newest of those writes retires** —
which, retirement being in order, retires all of them. The transient is bounded by one ROB drain.

The obvious implementation reuses the ring's conservative **overflow generation** for that fence.
That is wrong, and silently so: an overflow generation is *extended* by every write published while
it is live (`read_local_resolve_pending_body`), so on a 1:1 connection — which always has a write in
flight — it would never end, and not one read would ever be served locally again. The arming fence
is fixed at arming and is **never extended**; writes published after arming take ordinary ring slots
underneath it and are exact the moment it lifts. `tests/read_local_write_ring_unit.cc` case 16 is
that assertion.

### (2) Sidecar on first write

The 1216-byte `ReadLocalRobState` (a 1280-byte jemalloc class; the +972.8 B/connection of the
measured fact) is allocated **at the first write of an armed connection — never at accept**. A
pure-write connection, a pure-read connection and an idle connection each carry none of it.

Arming itself allocates nothing, which is what keeps a read-only connection free: the transient's
fence is a Rob word, not sidecar state. Allocation failure at the first armed write falls back to
the unarmed contract — always safe, and self-healing, because the next quiescent read re-arms and
the next write retries.

The MGET latest-read fence moved out of the sidecar into the Rob for the same reason: it is a
**read**-side fence, and a pure-MGET connection must not have to allocate 1216 bytes of write ring
to hold one id. It is also cheaper where it now lives — `local_mget_fence_pending()` runs once per
armed parse pass and no longer dereferences the heap.

### What did not change

* `Rob<64>` is still **192 bytes**: the four new producer-line words (`read_local_arm_state_`,
  `read_local_unarmed_write_id_`, `local_mget_fence_id_`, `read_local_arm_stats_`) fit the padding
  `dispatch_` already owned, and `local_mget_fence_id_` arrived from the sidecar, not from nowhere.
* `ReadLocalRobState` is still **1216 bytes**.
* The ring itself, its tag mirror, the overflow machinery, the derivation `static_assert`
  (`kWriteRingCapacity >= Capacity`) and both kept capacity fallbacks are untouched. This lane
  changes *when* the ring is used, never *how*.
* `op->mark_read_local_precise_write()` — and therefore the EX-side eviction guard — fires exactly
  where it fired before. The unarmed `refine_*` calls return the answer an unfilled ring returns,
  because the caller uses that answer to decide a property of the *command's keys*, which ring
  capacity never had a say in.
* No knob. Arm-on-demand is the only behaviour; there is nothing to turn off.

## Counters (INFO, `# Readlocal`)

| field | meaning |
|---|---|
| `read_local_arms` | connections a local read armed |
| `read_local_write_ring_sidecars` | RYOW sidecars allocated (never at accept) |
| `read_local_write_ring_records` | writes committed into a ring — **0 for a pure-write stream** |
| `read_local_fallback_arm_transient` | reads demoted by the arming transient, reported apart from `read_local_fallback_inflight_write` (a steady-state key conflict) so a bench can never confuse the two |

Every increment sits behind a `noinline` call the unarmed stream never makes, so the counters are
always on rather than diagnostic-only. The block is per IO thread; the ROB holds a pointer to it,
installed at `adopt_client` and re-pointed at the migration ownership edge, and **nullable** —
`adopt_client` runs in split mode too, where a thread has no read-local state at all.

---

# Results

**Geometry.** Lane cores: server on physical 40,41; load generator on 42,43 + SMT siblings 170,171.
`--thread-mode fused --read-local 1 --shards 64`, memtier `--pipeline=32`, 512 connections
(`-t 4 -c 128`) or 2048 (`-c 512`), 64-byte values, keyspace pinned to dbsize (200k) so GET hit rate
is 100%. Three rounds, visits interleaved **A B B A** within each round, one fresh boot and preload
per visit. Instructions, cycles and fills come from `perf stat -C` over a 9-second window starting
4 s into each 14-second run; operations in that window come from the server's own
`total_commands_processed` delta, so instructions-per-op is a ratio of two quantities measured over
the same window rather than a rate multiplied by a duration.

**The same-binary null ran first, on the same cells**, and it is printed under every row. It is what
says which of these numbers are real. It also condemned two cells outright: on pure GET and on split
SET the two-core server was **not saturated** — its cores idled down to a 2.40 GHz-equivalent
against 3.05 on the saturated cells, and the same binary against itself moved 28% on GET. Those two
cells were re-run in a geometry that saturates (server shrunk to ONE physical core, load generator
given three), where the GET null is +0.07% / −0.19% / +0.03%. The GET row below is that re-run; the
split row is the original, whose instruction null is the tighter of the two.

**Interference.** Another lane ran an UNPINNED `make -j8` during these runs and its `cc1plus`
children landed on cores 40-43 and 168-171. Visits that lost their cores mid-window measure the
interference, not the binary, so one objective rule was applied identically to both arms and is
reported: a visit whose rate collapsed below 60% of its cell's two-arm median is dropped (6 of 72 in
the main pass, 6 of 24 in the supplementary). The `n=` counts below are the survivors. Note that
**instructions per op is immune to this**: a stolen core scales the instruction count and the op
count together, and the pure-SET arms read 3054/3055/3054/3057/3055 against 2900/2902/2901/2903
across every round, corrupted visits included.

    PRE = mainline ceb6b02f8 | POST = t-ringdiet | median of the valid ABBA visits
    cell                                  M ops/s   instr/op    cyc/op      IPC   ccx/kop  dram/kop
    -----------------------------------------------------------------------------------------------
    pure SET  p32 512c  fused armed         2.690     3055.0    2242.5    1.362     16.51  13900.43
                                            2.713     2901.4    2224.7    1.304     18.16  13530.95
                                           +0.83%     -5.03%    -0.79%   -4.27%   +10.00%    -2.66%   <- delta
                                           +0.09%     -0.03%    +0.33%   -0.34%    +0.52%    -0.73%   <- SAME-BINARY NULL (n=6/6)
    
    pure GET  p32 512c  fused armed         1.228     3004.9    2489.6    1.207     18.87  13917.78
                                            1.270     2972.8    2396.5    1.237     17.05  13825.09
                                           +3.46%     -1.07%    -3.74%   +2.53%    -9.65%    -0.67%   <- delta
                                           +0.07%     -0.19%    +0.03%   -0.15%    +6.23%    -0.18%   <- SAME-BINARY NULL (n=5/6)
    
    1:1 alternating p32 512c                2.196     4526.5    2766.0    1.642     21.78  12366.74
                                            2.215     4582.8    2740.3    1.679     21.36  12288.63
                                           +0.85%     +1.24%    -0.93%   +2.23%    -1.92%    -0.63%   <- delta
                                           -0.29%     +2.92%    +0.26%   +1.12%    +2.14%    -2.06%   <- SAME-BINARY NULL (n=6/6)
    
    blocked 10:10  p32 512c                 2.179     4712.7    2775.6    1.693     18.75  11776.70
                                            2.171     4714.0    2788.8    1.690     18.43  11773.31
                                           -0.38%     +0.03%    +0.47%   -0.17%    -1.70%    -0.03%   <- delta
                                           +0.69%     +0.65%    -1.08%   +1.00%   -23.46%    +0.80%   <- SAME-BINARY NULL (n=5/5)
    
    1:1 p32 2048c (footprint)               1.026    10063.2    5662.5    1.780     37.10  20984.38
                                            1.032     9938.4    5614.8    1.766     48.15  20179.91
                                           +0.55%     -1.24%    -0.84%   -0.79%   +29.76%    -3.83%   <- delta
                                           +0.25%     -0.26%    -0.07%   +0.23%    +0.24%    -1.80%   <- SAME-BINARY NULL (n=5/6)
    
    pure SET p32 512c SPLIT (off path)      2.922     2652.6    1989.9    1.330     12.12   8545.40
                                            2.913     2654.4    1981.8    1.333     20.48   8442.51
                                           -0.32%     +0.07%    -0.41%   +0.26%   +68.88%    -1.20%   <- delta
                                           -5.62%     -1.82%    +7.01%   -8.77%   -33.02%   +19.87%   <- SAME-BINARY NULL (n=5/4)
    
    
    INDEPENDENT CONFIRMATION on the FINAL rebuilt binary (1 round ABBA, quieter box)
    cell                                  M ops/s   instr/op    cyc/op
    ------------------------------------------------------------------
    pure SET  p32 512c  fused armed        +1.04%    3055.2->2901.5    -1.07%
    pure GET p32 512c fused armed          +0.12%    2389.0->2329.0    -1.39%
    1:1 alternating p32 512c               +1.21%    4503.4->4481.2    -1.05%
    blocked 10:10 p32 512c                 +1.20%    4730.3->4773.7    -1.12%
    1:1 p32 2048c                          +0.97%    9990.3->9868.9    -0.35%

## Verdict per cell

| cell | requirement | result |
|---|---|---|
| pure SET, fused armed | recover the −1.32% / +87 instr | **−153.6 instr/op (−5.03%), −0.79% cyc/op, +0.83% rate**, against an instruction null of ±0.03%. Not merely recovered: the ring's per-write bookkeeping is gone from the stream entirely, which is more than the sizing merge had added. |
| pure GET, fused armed | flat | **−1.07% instr/op, −3.74% cyc/op, +3.46% rate** (null +0.07/−0.19/+0.03). Better than flat — a pure-read connection now allocates no sidecar at all. |
| 1:1 alternating | flat | −0.93% cyc/op, +0.85% rate; instr/op +1.24% inside a null of +2.92%. Flat. |
| blocked 10:10 | keep the ring win, demotions near zero | **+0.03% instr/op, +0.47% cyc/op, −0.38% rate — every one inside the null.** Local service 99.94% (POST) vs 99.93% (PRE); write-conflict demotions 716 vs 708 out of 9.8M reads. The ring win is kept exactly. |
| 1:1 at 2048 connections | footprint | −1.24% instr/op, −0.84% cyc/op, +0.55% rate. Flat-to-better. |
| pure SET, SPLIT | an exact instruction null (off path) | **+0.07% instr/op.** Nothing on the split path moved. |

## The proof obligations

**Pure SET does ZERO ring bookkeeping.** Over a 25,036,702-operation window the armed server
reports `read_local_write_ring_records:0`, `read_local_write_ring_sidecars:0`, `read_local_arms:0`.
Pure GET the same. The 1:1 and blocked shapes report 10,054,155 and 10,058,724 records against
20,117,318 and 20,149,571 operations — 49.98%, i.e. **every** write on a connection that also reads.

**100% local service after the transient**, and the transient bound. Measured over whole runs:

| shape | connections | arms | sidecars | arm-transient demotions | per connection | local hits |
|---|---|---|---|---|---|---|
| 1:1 | 512 | 512 | 512 | 8192 | **16** | 14,853,474 |
| 1:1 | 2048 | 2048 | 2048 | 32768 | **16** | 7,396,718 |
| 10:10 | 512 | 512 | 512 | 6176 | **12.1** | 15,286,909 |
| 10:10 | 2048 | 2048 | 2048 | 24576 | **12.0** | 7,448,078 |

`arms` equals the connection count exactly — every connection arms once and never again — and the
transient costs at most sixteen demoted reads on that connection, ever: 0.055% of the run's local
reads at 512 connections. In the mid-run measurement windows above,
`read_local_fallback_arm_transient` is **0 on every cell**, because the transient is over before the
window opens. That is the one-shot property, measured.

**A read arriving with writes in flight before arming is demoted, never served stale.** Directed and
deterministic in `tests/read_local_write_ring_unit.cc` (cases 13-17 plus a 200k-frame soak that
starts UNARMED and crosses the transition), and end-to-end against a live armed server: 40 rounds of
a 300-deep SET pipeline followed by a GET of the last key written, on a fresh connection each time —
0 stale reads, 40 arms, 40 transient demotions reported.

## RSS per connection

512 and 2048 connections, each driven through one shape, against the same boot's zero-connection
baseline:

| connection shape | conns | PRE B/conn | POST B/conn | saved |
|---|---|---|---|---|
| idle (PING only) | 512 | 11424 | 10104 | **−1320** |
| idle | 2048 | 11394 | 10092 | **−1302** |
| read-only | 512 | 11480 | 10248 | **−1232** |
| read-only | 2048 | 11438 | 10114 | **−1324** |
| write-only | 512 | 11552 | 10232 | **−1320** |
| write-only | 2048 | 11412 | 10136 | **−1276** |
| reads AND writes | 512 | 11632 | 11480 | −152 |
| reads AND writes | 2048 | 11458 | 11450 | −8 |

Idle, read-only and write-only connections each stop paying the sidecar — about 1300 bytes, the
1280-byte jemalloc class plus its page share. A connection that both reads and writes pays exactly
what it paid before, which is the design: the ring is for connections that have both.

---

# Gate

`tests/gate.sh full` on lane cores 40-43,168-171, port 8431, with the Redis 7.4 oracle present:
**324 ok, 20 FAIL**. Every layout lock held (`Op` 336, `Client` 1984, `ThreadCtx` 1408, `Shard` 1440,
`Rob<64>` 192, `ReadLocalRobState` 1216 — the "release build (+footprint locks)" row), both
differential matrices passed (**Redis 7.4 differential matrix** and **Redis 7.4 differential matrix
(armed fused + read-local)**, the second being this lane's own geometry), glob/scan parity passed,
the new **read-local write ring + arming transient unit** row passed, and the
**read-local lane admission battery** — the one that requires ≥95% of a 32-connection ×
64-deep GET load to be served locally — passed.

## The 20 failures are three environment clusters, and the control says so

Another lane ran an unpinned `make -j8` and a second server on cores 40-43/168-171 throughout this
run; the gate's own preflight caught it by name twice.

| cluster | rows | what the log says |
|---|---|---|
| one poisoned fused+armed atomic-1 boot | 12 | that server logged `Client id=773/819/820/823/825 scheduled to be closed ASAP for overcoming of output buffer limits` — the Python battery client stopped draining. Every battery after it on that one boot failed downstream, and the boot's `hits=0` shutdown row with it. |
| gate preflight refused to boot | 4 | `boot preflight: cores/port not quiet: 3092661 137 82.7 tomokv` — twice, which is also why the ledger counted 343 against 341: a failed `boot` prints a row a successful one does not, so 341 + 2 = 343 and the EXPECT arithmetic in this file is correct. |
| load-sensitive assertions | 4 | `multirace`: every correctness assertion passed (leaked=0, stale=0, bad_exec=0) and the row failed its own **vacuity** check — the timing window it exists to close never opened. `flipctl`: "controller moved during stable hold", `last_trigger: fingerprint-shift` — the controller reacted to a workload shift that a shared box produces. `atomic RYOW under ASAN`: a THROUGHPUT ratio, pipe=2840/s against serial=14328/s. |

**Three of the four clusters cannot involve this lane at all**: `multirace`, `flipctl` and the ASAN
rows all boot through `boot`, which is `--ratio 6:2` — **split mode, no `--read-local`**. On those
boots `read_local_enabled()` is false, no `ReadLocalRobState` is ever allocated, and every line this
lane touches is unreachable.

**The one cluster that does run this code was controlled directly.** Same batteries, same boot
geometry (`--thread-mode fused --atomic 1 --read-local 1 --enable-debug-command yes`), mainline
`ceb6b02f8` against `t-ringdiet`, on a quiet box:

| battery | mainline ceb6b02f8 | t-ringdiet |
|---|---|---|
| concur, edgeproto, edgeenc, arity, contarity, cmdgap, aclsel, expwide, infofix, pushtear | **all ok** | **all ok** |
| multirace (atomic 1) | ok | ok |

Both arms pass every one of them. The gate rows are the box, not the binary.

And the flipctl row was controlled the same way, on its own `--ratio 6:2 --flip-auto 1` boot:

| battery | mainline ceb6b02f8 | t-ringdiet |
|---|---|---|
| flipctl | **FAIL** ("controller moved during stable hold") | FAIL ("surge response did not settle and hold") |

**It fails on mainline too**, on a different assertion each time — the signature of a controller
reacting to a workload another lane is perturbing, not of a defect either binary carries.

## The 20, accounted for

12 (one poisoned boot, both arms pass the batteries when quiet) + 4 (gate preflight refused to boot,
naming the offending pid) + 1 (multirace vacuity, passes on both arms when quiet, and its boot has
no read-local lane) + 1 (flipctl, fails on mainline too) + 1 (atomic RYOW under ASAN, a throughput
ratio on a boot with no read-local lane) + 1 (PROGRAM-STATE 343/341, which is 341 + the two rows the
failed boots add) = **20**. None is attributable to this lane, and the cluster that could have been
was controlled against mainline directly rather than argued away.
