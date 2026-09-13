# Lane xperf2 — efficiency findings E4–E7

Four audit claims. Each was tested on a live server BEFORE any code was touched; only the ones that
reproduced were fixed, and only the ones that reproduced got a test. One came back empty and is
recorded as such.

| item | claim | verdict | fixed | guard added |
|---|---|---|---|---|
| E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
| E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
| E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |

Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
`/tmp/claude-1000/redis74/src/redis-server`. Every server was stopped by the pid resolved from its
listening socket and the listener confirmed released before the next boot. All numbers INDICATIVE
(loopback, this lane's cores; other lanes were active on the same box).

---

## E4 — ExpireIndex — REPRODUCED, fixed

### What the code did

`ExpireIndex` (src/store/flatstore.h) is the per-shard sidecar of key hashes carrying a deadline.
Three separate costs were claimed and all three are real:

1. **Growth moved every entry in ONE pass.** `rehash(cap)` allocated a new table, `assign`-ed it
   (which *writes* the zeroes), and re-inserted every live entry before returning — inside one
   ordinary `SET key v PX …`. That runs on the shard-owning executor, so the whole shard stops.
2. **live→0 cleared the WHOLE sidecar.** `erase()` ended with
   `memset(states_.data(), 0, states_.size())` whenever the live count hit zero.
3. **Capacity was monotone in the all-time-high volatile population**, so (2) was permanently taxed
   by any historical burst: `std::vector::clear()` on FLUSHALL kept capacity too.

### BEFORE — worst single SET latency at a growth trigger

Single shard (`--ratio 1:1 --shards 1`), pipeline depth 1, so the number is one command's latency.
The trigger point is predictable: the sidecar grew when `(live+1)*100 >= capacity*70`.

| volatile keys live at the trigger | tomokv, SET…PX | tomokv, SET (no TTL) — control | redis 7.4, SET…PX |
|---:|---:|---:|---:|
| 45,875 | **676 µs** | 55 µs | 34 µs |
| 183,500 | **4,237 µs** | 64 µs | 32 µs |
| 734,003 | **18,358 µs** | 67 µs | 34 µs |
| 2,936,012 | **76,630 µs** | 55 µs | 31 µs |

Baseline SET on the same connection ≈ 32 µs in every row. The no-TTL control is the attribution:
it grows the main FlatStore table at the *same* live counts (the main table's rehash is already
incremental) and is flat, so the curve belongs to the sidecar and to nothing else. Redis is flat.

### BEFORE — the DEL that takes a shard's live volatile count to zero

Paired on one connection: `DEL A` leaves a volatile key behind (control), `DEL B` takes live→0.

| all-time-high volatile keys | ctl µs | live→0 µs | ratio | no-TTL control ratio | redis 7.4 ratio |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 31.58 | 31.81 | 1.01 | 1.01 | 1.00 |
| 131,072 | 31.76 | 32.26 | 1.02 | 1.01 | 1.00 |
| 262,144 | 31.69 | 41.89 | **1.32** | 1.01 | 1.00 |
| 524,288 | 31.73 | 50.78 | **1.60** | 1.01 | 1.00 |
| 1,048,576 | 31.82 | 64.67 | **2.03** | 1.00 | 1.01 |

Two controls report zero across the whole range: the same workload with no deadlines at all, and
redis.

Third symptom, the monotone capacity itself. Fresh boot, 1M volatile keys, then FLUSHALL, RSS from
`/proc/<pid>/status` (same script, both binaries, fresh boot each):

| | fresh | after 1M volatile keys | after FLUSHALL | retained over fresh |
|---|---:|---:|---:|---:|
| BEFORE | 9,964 kB | 89,320 kB | 72,948 kB | **62,984 kB** |
| AFTER | 9,824 kB | 89,420 kB | 56,660 kB | **46,836 kB** |

16.1 MB less retained, against a sidecar of 2,097,152 slots × 9 B = 18.9 MB at that population.
The rest of what is retained is the main table (also monotone, not part of this claim) and
jemalloc's own extent cache.

### Fix

`ExpireIndex` now has the two-table shape `FlatStore::rehash_step()` already uses:

* **incremental migration** — `kMigrateSlotsPerOp = 8` old slots move per operation, driven by both
  `insert()` and `sample()` so a migration finishes even after writes stop;
* **`calloc`, not `assign`** — the new table arrives as demand-zero pages exactly like
  `FlatStore::alloc_table()`. Writing the zeroes up front was, on its own, most of the residual
  stall (45 ms at 2.9M keys even after the migration was made incremental);
* **release on empty** — reaching zero live deadlines frees both tables instead of clearing every
  state byte, so the transition is O(1) *and* capacity stops being monotone in the all-time high. A
  table already at the 16-entry minimum keeps its buffer and clears 16 bytes, so the ordinary "a few
  volatile keys come and go" case stays allocation-free. `clear()` (FLUSHALL) releases too.

Three things this required that are easy to get wrong, all found by the new battery rather than by
reading:

* a migrated old slot must become a **tombstone**, not empty — `erase_in()` still probes the old
  table while the move runs, and a hole terminates its probe run. Punching holes made a still-live
  deadline unfindable and showed up as INFO `expires` over-reporting after PERSIST;
* the sampler must **skip the vacated prefix** of the old table, and
* **a migration in flight is pending work.** Without that, `active_expire` returned 0 on a barren
  sampling pass, the executor parked, and expiry advanced ~20 slots per incoming command. This was
  a genuine regression I introduced: 200k keys with a 5 s TTL expired at ~3,700 keys/s on the
  pre-fix binary and **0 keys in 60 s** on the first version of mine. `active_expire()` now returns
  a work count that includes the in-flight move.

### AFTER

| volatile keys live at the trigger | BEFORE | AFTER | speedup |
|---:|---:|---:|---:|
| 45,875 | 676 µs | **122 µs** | 5.5× |
| 183,500 | 4,237 µs | **162 µs** | 26× |
| 734,003 | 18,358 µs | **329 µs** | 56× |
| 2,936,012 | 76,630 µs | **105 µs** | 731× |
| 11,744,051 | not run (BEFORE curve is linear ⇒ ≈300 ms) | **106 µs** | — |

The curve is now BOUNDED, not linear: 4M and 11.7M volatile keys cost the same 105 µs. The residual
is one `calloc` of the new table, the same allocation the main table pays; the no-TTL control on
the same binary reads 54–166 µs. The 329 µs at 734k is jemalloc recycling a dirty extent (it
memsets those), not work proportional to the population.

live→0, same paired measurement:

| all-time-high | BEFORE ratio | AFTER ratio | no-TTL control | redis 7.4 |
|---:|---:|---:|---:|---:|
| 65,536 | 1.01 | 1.00 | 1.01 | 1.00 |
| 131,072 | 1.02 | 1.01 | 1.01 | 1.00 |
| 262,144 | 1.32 | 1.01 | 1.01 | 1.00 |
| 524,288 | 1.60 | 1.00 | 1.01 | 1.00 |
| 1,048,576 | **2.03** | **1.01** | 1.00 | 1.01 |

---

## E5 — zero-copy borrow registry — REPRODUCED, fixed

### Confirming the path first

The brief is explicit that a measurement of a path that never executed proves nothing, and the
first attempt at this measurement fell into exactly that trap: with `--shards 1` an MGET takes the
same-owner `localfast` path (`ScatterPrepare::NotScatter`) and never reaches the scatter gather, so
`zc_sends=0` — the borrow path had not run at all. Every number below comes from a run whose
shutdown accounting shows `zc_sends`/`zc_bytes`/`zc_releases` non-zero (e.g. `zc_sends=69120` per
MGET sweep, `zc_sends=752456` in the E7 positive control), or from live `CLIENT LIST oll` growth.

Side finding, not fixed (outside this brief): MGET's borrow gate is
`min(zc_min, ValueSlot::kInline)`, so **`--zc-min 0` does not turn borrowing off for MGET** — it
sets the cutover to 0 and borrows *every* non-empty string value. Only single-key GET honours
`0 = off`. Worth a knob-semantics pass by whoever owns zc.

### BEFORE — per-op cost of a borrowed GET vs live borrows on its shard

One shard, `--zc-min 64`, probe value 128 B (borrows), control value 32 B (never borrows), one
connection at pipeline depth 32. Borrows are held by slow readers with a clamped receive window.

| live borrows on the shard | borrowed GET ns/op | non-borrowed control ns/op |
|---:|---:|---:|
| 0 | 2903 | 1098 |
| ~300 | 2923 | 1105 |
| ~600 | 3121 | 1099 |
| ~1100 | **3185 (+9.7 %)** | 1101 |
| ~2100 | 3146 | 1104 |

The control is flat to 0.5 % across the whole range, so the growth is the registry and not the box.

### BEFORE — the quadratic half, isolated

Fix the MGET key count at 2048 and vary only the shard count, so the borrows-per-shard B changes
while the per-key IO-side segment work does not (executors fixed at 2). Reported figure is the
borrow arm minus an inline arm that differs only in value length (1032 B vs 1024 B, straddling the
cutover) — an interleaved A/B on one server.

| shards | borrows per shard | BEFORE Δ µs | AFTER Δ µs |
|---:|---:|---:|---:|
| 2 | 1024 | **5431.7** | 3423.5 |
| 4 | 512 | 3628.5 | 3536.2 |
| 8 | 256 | 3530.8 | 3602.3 |
| 16 | 128 | 3486.9 | 3545.5 |
| 32 | 64 | 3398.1 | 3594.5 |
| 64 | 32 | 3378.7 | 3596.7 |

BEFORE: +60.8 % from B=32 to B=1024. AFTER: −4.8 %, i.e. gone.

Per-key view of the same A/B at 2 shards (Δ ns per borrowed key): BEFORE 949 → 1827 over K=16 →
2048; AFTER 1020 → 1669, and the top of the curve flattens (K=1024: 1809 → 1670; K=2048: 1827 →
1669, total 3741 µs → 3418 µs, −8.6 %). Most of the ~1.3–1.8 µs per borrowed key is the segment
machinery (three segments and a 16-iovec sendmsg window per value), **not** the registry; the
registry is the part that GROWS, and it is the part that is now gone.

### Fix

`borrows_` stays exactly what it was — a vector, single-owner, no atomics — and gains an optional
open-addressed `ptr → slot` index (`borrow_idx_`). The index is never the source of truth:
`borrow_find()` falls back to the scan whenever it is empty, every allocation failure just releases
it, and it is only built past `kBorrowIndexMin = 32` entries so an ordinary connection with a
handful of borrows never pays a hash. `borrow()`, `unborrow()`, `is_borrowed()`, `retire_obj()` and
the atomic `retire_detached_obj()` all go through it; teardown still walks the vector.

### AFTER

| live borrows | BEFORE ns/op | AFTER ns/op | control AFTER |
|---:|---:|---:|---:|
| 0 | 2903 | 2887 | 1111 |
| ~300 | 2923 | 2934 | 1114 |
| ~600 | 3121 | 2930 | 1115 |
| ~1100 | 3185 | 2946 | 1116 |
| ~2100 | 3146 | 2942 | 1117 |

Growth over 0 → ~1100 live borrows: **+9.7 % → +2.0 %**.

---

## E6 — cross-shard dispatch scanning every configured thread — REPRODUCED, fixed

### What the code did

`src/core/io_loop.h`, plain (non-atomic) scatter arm: `uint32_t needed[kMaxThreads] = {}` zeroed
512 bytes per cross-shard op, and the room check then walked `for (tid = 0; tid < srv_->nthreads();
tid++)` looking for the two entries a 2-key MGET had actually set. The atomic arm twenty lines
below already used a participants list; the plain arm did not.

### BEFORE — per-op cost vs configured thread count, everything else pinned

16 shards, all of them homed on the same two executors via `--shard-home`, two IO threads on their
own cpus, and the extra threads are **idle fillers pinned to one separate cpu** (they own no shard
and no connection; at 128 threads they cost ~1.1 busy cores, all of it on that one filler cpu in
the other L3 domain — the four active threads keep a cpu each), so only `srv_->nthreads()` changes
between arms. `same` is a
2-key MGET on ONE shard: identical command, identical reply, takes `localfast` and never enters the
dispatch arm — the control.

| threads | BEFORE cross ns/op | BEFORE same ns/op | AFTER cross ns/op | AFTER same ns/op |
|---:|---:|---:|---:|---:|
| 4 | 1731.1 | 1302.4 | 1739.3 | 1309.0 |
| 8 | 1799.8 | 1296.8 | 1784.3 | 1336.5 |
| 16 | 1765.0 | 1286.8 | 1789.6 | 1301.0 |
| 32 | 1792.8 | 1294.5 | 1736.8 | 1318.9 |
| 64 | 1872.7 | 1353.4 | 1794.9 | 1356.0 |
| 128 | **1918.4 (+10.8 %)** | 1337.3 | **1792.8 (+3.1 %)** | 1318.8 |

The same-shard control moves +2.7 % / +0.7 % over the same range, so the growth is specific to the
dispatch. An independent placement (fillers as IO threads on a different cpu) reproduced it at
+11.4 %.

### Fix

The demand array became a **zero-on-entry member** of the IO loop and the arm builds a participant
list, so it costs one pass over the shards this op actually touches instead of a 512-byte memset
plus a walk of every configured thread. The zero-on-entry invariant is restored before every exit
from the arm, including the no-room `break`. This is the pattern the atomic arm already used.

At 128 threads: cross-shard per-op cost 1918 → 1793 ns, throughput 521k → 558k ops/s (+7.0 %).

---

## E7 — "segment mode is absorbing" — NOT REPRODUCED

The claim predicts that after one large borrowed reply puts a connection into segmented-send mode
it never returns to the cheap path, so every later small reply pays a malloc and an iovec slot.

Small-GET throughput on ONE connection, before and after a 2 MB borrowed read on that connection:

| | pipeline 32 | pipeline 1 |
|---|---:|---:|
| before the large read | 928,377 ops/s | 32,788 ops/s |
| after 1 × 2 MB borrowed read | 929,334 (**1.001**) | 32,860 (**1.002**) |
| after 50 further 2 MB reads | 934,397 (**1.007**) | 32,978 (**1.006**) |
| fresh-connection control | 925,035 (0.996) | 32,790 (1.000) |

The reads really were borrowed and really were segmented: the arm's shutdown accounting reads
`zc_sends=51 zc_bytes=106954752 zc_releases=51` — 51 sendmsg submissions carrying a BORROW iovec,
one per large read.

**Detector sensitivity (the control that must report non-zero).** A "no change" result only means
something if the instrument could have seen the change. Forcing *every* reply onto the segmented
borrow path (`--zc-min 64`, 128 B values) drops the identical probe from **834,087 to 341,714
ops/s — 2.44×**. So a connection genuinely stuck in segment mode would read as a 59 % throughput
collapse; the measurement reads 1.001.

Reading the code afterwards agrees: staging keys off `Client::has_pending_segments()`, and
`SegmentQueue::consume()` pops each segment as it completes, so the queue empties and the next
reply takes `commit_fill()` again. `segmented_send_` is only how the send completion interprets its
byte count. **No fix, no gate row** — per the lane rule, nothing here to guard.

---

## Tests added (only for what reproduced)

Every assertion is a RATIO or a growth bound between two measurements taken on the same connection
or the same rig minutes apart — never an absolute time — and every battery was checked against the
pre-fix binary to prove it can actually fail.

| battery | boot it needs | asserts | pre-fix | post-fix |
|---|---|---|---|---|
| `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
| `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |

Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
`borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
the cross pair really fanned out to two owner threads and the control stayed on one.

None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
gate's port/cores belong to the mainline operator. Run them as above.

## Evidence

```
DIFFER string:  4033 ops, 0 diffs -> PASS   (seeds 5, 13, 29)
DIFFER hexpire: 4288 ops, 0 diffs -> PASS   (seeds 5, 13, 29)
DIFFER xshard:  4276 ops, 0 diffs -> PASS   (seeds 5, 13, 29)

--atomic 0 and --atomic 1, both clean:
  hexpire torture ryow limits multi_exec dumprestore stream zc tracking lua_scripting resp3  PASS
  stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0

evict_battery: off noev lru vlru vttl lfu growth config   PASS
  (the `lru` "hot survives >= cold" leg is a small-sample statistical check and flaked once
   mid-session on both binaries; it passes 3/3 on re-run and never touches ExpireIndex)

ASAN + UBSAN build, 0 sanitizer reports:
  zc torture ryow hexpire limits multi_exec stream           PASS
  borrow_registry (2000 live borrows -> exercises the index) PASS   ratio 1.017
  ExpireIndex migration exercise (3 rounds of grow/PERSIST/DEL/PEXPIRE/active-expire) clean

EXPIREINDEX PASS   (--atomic 0 and --atomic 1)
BORROW-REGISTRY PASS (--atomic 0 and --atomic 1)
XSHARD-DISPATCH-SCALE PASS
```

## p32 GET/SET guard — the plain path is untouched

memtier, 8 threads × 4 connections, pipeline 32, 32 B values, keymax = 1,000,000 (dbsize asserted
== keymax before every GET run, so the hit rate is 100 % in both arms), 10 s, server on cores
48–55, loadgen on 56–63, arms interleaved base/fixed on one server at a time.

| round | base GET | fixed GET | base SET | fixed SET |
|---:|---:|---:|---:|---:|
| 1 | 6,498,639 | 6,683,729 | 6,352,593 | 6,338,104 |
| 2 | 6,624,407 | 6,609,853 | 6,186,392 | 6,073,088 |
| 3 | 6,399,403 | 6,248,426 | 6,185,447 | 6,308,253 |
| 4 | 6,644,057 | 6,651,214 | 6,275,724 | 6,093,031 |
| 5 | 6,622,337 | 6,454,537 | 6,426,859 | 6,303,479 |
| 6 | 6,669,955 | 6,591,245 | 6,236,774 | 5,973,915 |

Medians: GET 6,623,372 → 6,600,549 (**−0.34 %**), SET 6,256,249 → 6,198,255 (**−0.93 %**).
Best-of-six: GET **+0.21 %**, SET **−1.38 %**. Within-arm spread across rounds is 4–7 %, so every
one of those is noise. Nothing added to GET/SET: the E4 work is confined to the
deadline sidecar, the E6 work to the cross-shard dispatch arm, and the E5 index is behind the same
`outstanding_borrows_ == 0` gate the registry already had (and is not even built below 32 borrows).

## Scope not taken

* The **atomic** scatter arm (io_loop.h) and `publish_phase2` / `publish_pop_retry`
  (scatter_engine.inc) still zero `needed[kMaxThreads]` per op. They do not walk `nthreads()`, and
  E6 was reproduced and measured on the plain arm only — so per the lane rule they are left alone
  rather than "fixed" on the strength of an audit.
* The **sparse-sidecar sampling** behaviour is pre-existing and unchanged: a shard left with a few
  live deadlines in a large index samples 20 slots per pass and the executor parks between passes,
  so active expiry advances slowly (measured ~3,700 keys/s at 200k keys on BOTH the pre-fix and the
  post-fix binary). Release-on-empty only helps when the count reaches exactly zero. Not part of
  any E4–E7 claim; recorded here because the new battery is sensitive to it.
* `--zc-min 0` not disabling MGET's borrow path (above). A knob-semantics question, not an
  efficiency one.
