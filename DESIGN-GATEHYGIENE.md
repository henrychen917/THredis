# Gate hygiene: the rows that assert a timing or statistical property

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Branch `t-gatehygiene`, from `ceb6b02f8` (train 13).

Three full-gate rows reddened intermittently on a correct build. None of them had a wrong
mechanism; each asserted a **timing or statistical** property alongside (or instead of) the
mechanism it exists to protect, and so measured the box rather than the server. The finals depend
on a gate that is trustworthy, which means a red row has to mean a defect.

The rule this lane applies:

> A row may *use* timing to deliver a mechanism. It may not *assert* timing unless the assertion
> is still true on an arbitrarily slow machine, or the row states and enforces the precondition
> that makes it true and refuses to be judged when that precondition is unmet.

---

## 1. `tests/atomic_ryow.py` — "cross-key atomics on one connection overlap"

### The defect

One `note()` bundled two different kinds of claim:

```python
note("cross-key atomics on one connection overlap",
     overlap_ok and stall_after > stall_before and pipe_rate > serial_rate * 1.10, ...)
```

* `overlap_ok and stall_after > stall_before` is a **mechanism** claim. 24 cross-key atomic groups
  pipelined on one connection are all admitted and answered `OK`, and `atomic_window_stalls`
  *moves* — which is only possible if a younger group was in flight while an older one was still
  deciding. The regression this section exists for (a return of the full connection barrier) makes
  `stall_after == stall_before` on any build at any speed.
* `pipe_rate > serial_rate * 1.10` is a **performance** claim. It is false on a correct build
  whenever the two arms are not slowed by the same factor.

The full gate runs this file a second time under ASAN. The sanitizer does not slow the pipelined
arm and the serial arm equally, and the ratio inverts: the gate measured **pipe 21,005/s against
serial 24,202/s** and reddened the row while every correctness check in that same run passed. The
same build passed 4 of 4 standalone.

### The fix

The two claims are two `note()`s. The mechanism half runs on **every** tier. The rate half runs
only when the caller says the server is a release build — `tests/gate.sh` passes
`--no-rate-assertions` on its ASAN row (`tests/gate.sh:1246`).

An **explicit flag**, not a sniffed `GATE_TIER` environment variable: a battery run by hand then
does what it was told rather than what it guessed about its server. When the flag is present the
row prints `SKIP` with the reason **and the measured rates**, so the number stays in the log on
both tiers.

---

## 2. `tests/flipctl.py` — "stable hold"

### The defect

```python
raise AssertionError("controller moved during stable hold: %r" % row)
```

The row asserts that the automatic FLIP controller does not move during a 30 s hold. It failed
about **one full-gate run in five** on the shipped controller, always straight after the long
torture and ASAN phase, and passed **6 of 6 interleaved** on both the shipped controller and the
redesign in a quiet window. So the trigger is the gate's own sequence, not the controller.

The row's claim is about the **controller**, and it is only about the controller if the offered
load really was stationary. The row assumed that instead of measuring it, and had no way to tell a
real move from a driver artefact after the fact — the failure text was one `INFO FLIPCTL` dict with
no trigger reason and no history.

### The fix (test side only — the controller is untouched; a separate lane owns it)

The row now **states its precondition and enforces it**.

* The driver counts **its own** completed commands (one single-element counter per worker), not the
  server's `total_commands`, which also counts this script's `INFO` polls.
* Each attempt spends `PRE_HOLD_SECONDS` (8 s) measuring that rate and replays the controller's own
  anchored rate rule over the trace — `rate_rule_fires()` mirrors `sample_anchored_rate()` plus the
  surge/collapse streaks in `src/core/flipctl.cc`: adjacent one-second tick windows average into
  one reading, a reading is out of band when it leaves `reference * (1 ± band)` where `band` is the
  band the controller itself reports (`flipctl_rate_band`), and a maneuver needs **two consecutive**
  out-of-band readings. Only a stationary pre-hold window opens the assertion window.
* **A move while the load is provably stationary still FAILS**, and the failure now carries
  `flipctl_last_trigger`, the controller's own `DEBUG FLIPCTL` dump taken **at the move**, the
  driver's per-second rates, and the per-second live-split trace — so a real move is
  distinguishable from a driver artefact in the log, without re-running anything.
* A hold whose load left that band is **re-rolled**, up to 3 attempts inside a wall budget, and then
  **SKIPPED** with every number printed. A row must not go red for something the driver did on a box
  that was busy elsewhere.

The surge phase's counters are re-read at the end of the hold instead of being assumed to sit at
their boot values: a re-rolled hold may legitimately have spent a rate trigger on the driver's own
wobble, and the surge phase's claim is about the **delta** the surge produces.

`tests/gate.sh` gives the row 480 s rather than 300 s. The typical run is unchanged; the worst case
adds three re-rolls and their re-anchor waits.

---

## 3. `tests/evict_battery.py` — the survival rows

### Verified present on this base

Commit `61c0a0416` ("evict battery: make the two survival rows deterministic, and name the property
that made them flake") is in `ceb6b02f8`. Both survival rows are exact assertions and both state
the property they prove:

* `lruclock` pins `maxmemory-samples 64` for the section (`tests/evict_battery.py:227`) **and says
  why that is a pure gain under LRU and would be the opposite under LFU** — sampling a candidate
  under LRU only reads its clock byte, sampling one under LFU *decrements* its counter
  (`flatstore.h choose_victim`), because five header bits leave no room for a wall-clock decay.
* `lfu` deliberately does **not** pin it, carries a `DO NOT "fix" a flake here with CONFIG SET
  maxmemory-samples 64` comment with the measurement behind it (4/20 survivors at samples=64 with
  burst reads, against 17-20 at the default 5), and instead made the row deterministic by reading
  the hot set **through** the pressure rather than in a burst before each fill.
* Each survival row is now paired with a directly-asserted bound that names the mechanism: the hot
  median `OBJECT FREQ >= 8` against a cold ceiling of 6 (`:185`), and `untouched < re-read` with an
  absolute companion (`:301`).

### The other sampling rows in the same file, given the same treatment

Neither is a gate row today (the gate runs only the `lfu` and `lruclock` sections), but both
asserted a count out of a sampled victim choice:

* `lru` — `check("allkeys-lru: plateau near ceiling (<10k of 14k offered)", dbsize() < 10000)` used a
  constant somebody measured once, and sat next to `check("allkeys-lru: writes keep landing past
  limit", True)` — literally the constant `True`, a row that could not fail. Replaced by an
  **identity** (`live + evicted == offered`, exact: `fill()` does not tolerate OOM, nothing carries
  a TTL, no key is written twice) plus a plateau bound **derived from `MM`** rather than observed.
* `vttl` — `soon < late` is already the right *form* (relative, no magic constant, inverts outright
  if the policy stops preferring the nearest expiry). What it lacked was the non-vacuity companion:
  if the pressure never reached the `soon` bucket both counts would sit at 50 and the comparison
  would be deciding on noise. Added `soon < 50`.

---
## 4. Audit — every other assertion of this class in `tests/`

Swept all 125 files under `tests/` plus `gate.sh`. "MECHANISM" means the timing or statistics only
*deliver* the mechanism and the assertion is still true on an arbitrarily slow machine. "PERF" means
the assertion is false when the box is slow or loaded even though the code is correct.

**None of these were changed by this lane.** They are listed so the next lane can pick them up in
priority order; the three above were the ones actually reddening runs.

### The one structural finding: what runs twice

`tests/gate.sh` runs exactly **five** batteries in the release tier and again under ASAN. An ASAN
re-run is where a ratio inverts (the sanitizer does not slow both arms of a ratio equally, and it
inflates every allocation footprint):

| battery | release | ASAN | exposure |
| --- | --- | --- | --- |
| `torture.py` | `:375` | `:1233` | mild — only `landed > 0` (`:117`) |
| `ryow.py` | `:377` | `:1235` | none — no timing or statistical assertion |
| `atomic_torn.py` | `:402` | `:1237` | **highest unmitigated risk** — five race-window-hit rows |
| `atomic_ryow.py` | `:404` | `:1246` | **fixed by this lane** (`--no-rate-assertions`) |
| `zc.py` | `:1286` | `:1291` | none |

`atomic_torn.py` is the one to do next. `:877` (`atomic_window_stalls` must advance out of a
64-frame burst against a 2-deep window) and `:940` (`atomic_inflight` must be seen `<= 3` within 3 s
and then stay there across a 0.4 s poll) are the two most likely to invert under ASAN. `:695`,
`:696`, `:697` are race-window-hit gates whose cold path the file itself already gives a 1.5 s retry
loop.

### Class A — rate assertions

| file:line | assertion | verdict |
| --- | --- | --- |
| `atomic_ryow.py:256` | `pipe_rate > serial_rate * 1.10` | PERF — **fixed**, now behind `--no-rate-assertions` |
| `borrow_registry.py:261` | `growth <= 1.05` | PERF — 5% budget on a wall-clock ns/op ratio |
| `borrow_registry.py:263` | `control <= 1.05` | PERF — the file's own skip path at `:247` exists because this fires on environment drift |
| `borrow_registry.py:265` | `borrow_ref > plain_ref * 1.5` | PERF — and *not* covered by that skip, so it scores even when the instrument was disowned |
| `expireindex.py:179` | `growth <= 8.0` | PERF — ratio of two `perf_counter_ns` medians |
| `expireindex.py:224` | `ratio <= 1.25` | PERF — tightest ratio bound in the tree, on two DEL timings µs apart |
| `infofix.py:273,285` | `instantaneous_ops_per_sec == 0` | PERF — a sampled EWMA asserted to be exactly zero |
| `infofix.py:282` | `instantaneous_ops_per_sec > 0` after a burst | PERF — the burst must land inside the sampler's live window |
| `xacct.py:117` | `stream_ratio < 4.0` | PERF — 4x budget on a sub-µs op |
| `xacct.py:166` | `multi_ratio < 15.0` | PERF — empirical, not proved |
| `xmove.py:231` | `ratio < 5.0` | PERF — asserts O(1) by timing rather than by a counter |
| `xshard_dispatch_scale.sh:123` | `se > 0 and be > 0` | PERF — a *sign* test on a difference of two medians |
| `xshard_dispatch_scale.sh:127` | `ratio <= limit` | PERF — ratio of differences of medians across two boots |
| `gate.sh:919` | `MM_RATE >= 120000` | PERF — but deliberately a 5x-margin **floor** (healthy ~600k, the regression class lands under 70k) and documented as such. Keep. |
| `gate.sh:1395,1397` | `d >= -3.0` vs `gate_refs.txt` | PERF — NIC cells against refs the file itself flags as pinned on a different kernel |

### Class B — duration assertions

Safe lower bounds (MECHANISM — a slower box only overshoots): `blocking.py:143`,
`blockmulti.py:212`, `climon2.py:377`, `debug.py:108`, `evict_battery.py:256,280`,
`expwide.py:255,366,413,503`, `lbsignals.py:91,147,157,208` (60 s ceilings), `spinprobe.py:200`,
`stream.py:253`.

Ceilings that are performance claims:

| file:line | assertion | note |
| --- | --- | --- |
| `blocking.py:138,153` | `elapsed <= 0.75` | the 0.10 floor is the mechanism; 0.75 fails if the executor heartbeat is delayed |
| `blockmulti.py:137,165` | `elapsed < 0.25` | proves "non-blocking" by wall clock instead of by a counter |
| `blockmulti.py:190` | `elapsed < 0.10` | 100 ms for one satisfied `WAIT 0 500` |
| `blockmulti.py:200` | `elapsed <= 0.90` | observed ceiling |
| `blockmulti.py:229` | `elapsed < 0.25` | `WAIT` inside EXEC |
| `bplus.py:366` | `COMMIT_CHECKPOINT_BUDGET` 0.60 s | called "deterministic budget"; it is wall clock |
| `climon2.py:305` | `len(lines) >= 4` from `drain(0.6)` | a count out of a fixed window |
| `climon2.py:373` | `read_ms < 250` | a slow GET reads as a pause bug |
| `climon2.py:390` | `released_ms < 2000` | window spans two sleeps and two admin round trips |
| `execatomic.py:289`, `execiso.py:372` | `elapsed < HOLD_US / 4e6` | the *unarmed* control must finish in a quarter of the park budget |
| `limits.py:284` | `elapsed <= 3.5` | observed reaper-latency ceiling |
| `netio.py:218` | `worst < 25 ms` | a p100 doorbell-wake bound; one descheduled io thread in 20 rounds fails it |
| `read_local_lane.py:147` | `slow < 20.0 s` | generous, still a ceiling |
| `slowlog.py:154,263` | `< 400 ms` / `< 400` | the floors are mechanism, the ceilings are measured bounds |

### Class C — counts from sampling

| file:line | assertion | note |
| --- | --- | --- |
| `edgetime.py:707,750,753` | exact 8/8 present, 0 nils in 200 rolls, inverted repro detector | PERF — one unlucky roll flips the verdict |
| `evict_battery.py:202,209,212,234,236,283,319,325,328` | the lfu/lruclock survival, FREQ-band and lane-hit-rate rows | PERF by shape, but **each is now deterministic by construction and states its bound** (`61c0a0416`; see section 3) |
| `evict_battery.py:134` | `live < CEILING * 5 // 4` | MECHANISM after this lane — the gap is 8.4k vs 14k offered |
| `evict_battery.py:160,162` | `soon < 50`, `soon < late` | PERF by shape; relative, with the non-vacuity companion added by this lane |
| `expireindex.py:243` | `live >= n * 0.9` | 90% threshold on a population racing its own 3 s TTLs |
| `expireindex.py:250` | `moved >= n * 0.99` | the active/lazy reap split is sampled; 0.99 came from observation |
| `lru_slow.sh:55` | `hot > cold` | pure victim-sampling comparison, no non-vacuity floor |
| `multi_exec.py:391` | `reads > 100 and commits > 10` | op-count floors out of a fixed wall-clock window |
| `notify.py:499` | eviction must notify within 64 SETs | depends on the sampler picking a victim in 64 tries |
| `read_local_lane.py:162,174` | `>= 0.95 * gets` lane hit rate | the file's own header warns the lane loses a rate race against the drain |
| `session_monotonic.py:243` | `len(span) >= 2` | a geometry precondition drawn from the kernel's per-boot hash seed |
| `snap_cut_battery.py:248,250` | `writes > 10000`, `reads > 200` | absolute counts from a fixed-duration storm; an ASAN box cannot reach them |
| `stream.py:312,328` | byte deltas and `per_stream < 512` | inverts outright under ASAN redzones |
| `torture.py:117` | `landed > 0` | MECHANISM — only a total loss fails it |

### Class D — scheduling outcomes

The densest file is `flipctl.py` itself (14 rows). Beyond the stable hold this lane fixed, the
remaining ones are the `wait_for(..., 30/60/90)` bounds where the timeout *is* the discriminating
property (`:301`, `:339`, `:465`, `:476`, `:499`), the two `sleep(8)` "settled and held" windows
(`:483`, `:502`), the exactly-one-trigger claims (`:312`, `:465`), the ramp row (`:293`) and the
off-rail anchor (`:315`). `:268` (an idle server starts no maneuver) is a genuine mechanism check.

Race-window-hit gates — rows that fail when scheduling luck did **not** open the window they need,
i.e. the vacuity guard is itself a timing assertion:

`aof_frame_order.py:213`, `atomic_torn.py:695,696,697,877,940`, `edgetime.py:710`,
`execatomic.py:246`, `execiso.py:292,346`, `expwide.py:365,412`, `flip_under_load.py:291`,
`multi_exec.py:391`, `multirace.py:443,467`, `multires.py:298`, `read_local_lane.py:160`,
`session_monotonic.py:316,325`, `snap_cut_battery.py:258`, `snap_typed_race.py:237`,
`watchlive_gate.sh:115,120`, `gate.sh:871` (three FLIPs must each APPLY under memtier saturation —
the exact outcome `flip_under_load.py` counts as a legal refusal).

Budget-style scheduling rows: `debug.py:101,118,126` (300 ms), `lbsignals.py:106,138,139,153,156,160`
(counter-moved-by-at-most-N and counter-did-not-change while load ran), `spinprobe.py:184,224,271`
(no role change in a quiet window; <=64 loop iterations per idle second; <=24 iterations attributed
to one parked partial frame — its own 3-attempt retry loop at `:241` exists because it is
scheduling-sensitive).

Genuine mechanism checks in this class: `debug.py:191,193`, `differ_gate.sh:263`,
`lbsignals.py:101`, `multirace.py:474`, `notify.py:188,520,596`, `snap_typed_race.py:227`,
`xacct.py:163`.

### Clean

The six `.cc` unit tests (`flipctl_unit`, `cmdlookup_unit`, `read_local_ring_unit`,
`read_local_write_ring_unit`, `foreign_read_safety_test`, `config_parser_test`) use no clock, no
threads and no sampling. `benchfeat.py`, `broaden_bench.cc` and `matrix.sh` are pure reporters and
assert nothing on a measured quantity.

---

## 5. Row count

Unchanged: `EXPECT_QUICK=324`, `EXPECT_FULL=340`. No row was added or retired — `atomic_ryow.py`
splits one `note()` into two *inside* one battery, which is one gate row either way, and the
flipctl stable hold is one sub-assertion of one gate row.

---

## 6. Round 2 — what the acceptance runs themselves found

Two back-to-back full gates were the bar. Run 1 came back **340/340, 0 FAIL**. Run 2 reddened
twice, both in this lane's own class, and one of them **re-diagnosed the row I had just fixed**.

### 6.1 The stable hold's real trigger is the FINGERPRINT, not the rate

The new failure message paid for itself on its second outing. The run-2 move was:

```
last_trigger=fingerprint-shift
last_shift_distance=0.251848123   last_shift_band=0.020000000    (12.6x the band)
driver rate, all 34 samples of the window:  5998 .. 6002 /s      (0.07% spread)
```

The standing hypothesis — the driver's rate wobbles, the controller fires, the row goes red — is
**wrong**. The driver's offered rate is rock steady, the new precondition correctly certified it
stationary, and the detector that moved was the other one.

The signature the fingerprint detector watches includes **pass depth** — how many frames an io
thread happened to batch into one parse pass (`FlipFingerprintWindow::pass_depth`,
`src/core/flipctl.cc sample_fingerprint`). That is a property of how the box scheduled the load,
not of the load. This driver is strict request/response with a fixed command mix, a fixed
connection set and fixed key/value shapes; it cannot hold pass depth still. And because the
learned signature jitter on this workload is **0**, the band is the configured 2% and any
pass-depth movement at all clears it.

So the row now classifies the move by its trigger — which is exactly what recording
`flipctl_last_trigger` was for:

| `last_trigger` | verdict |
| --- | --- |
| `forced` | **FAIL** — nothing but the DEBUG hook can produce it |
| `anchor-rate-surge` / `anchor-rate-collapse` | **FAIL** when the driver's rate was stationary |
| `fingerprint-shift`, distance **inside** its own band | **FAIL** — a controller defect; no box artefact explains a detector firing inside its band |
| `fingerprint-shift`, distance **outside** its band | RE-ROLL, then SKIP with the distance, the band and the ratio |
| anything else | **FAIL** (conservative) |

The last row is the only case this test cannot adjudicate, and its skip line is the report the
controller lane needs. Everything else still fails — proven by induction below.

### 6.2 `tests/flip.py:287` — "in-flight test observed a real moving FLIP"

A race-window-hit gate, and the only assertion in that file that could lose its race. An observer
polls `FLIP` hoping to land a report between the reverse actuation starting and finishing; on a
loaded box the whole flip completes inside the observer's first round trip. **1 of 542 checks**,
with nothing wrong — every deterministic proof that the actuation happened (`flip_completed`,
`flip_clients_transferred`, `flip_conservation_checks`, and the ordered in-flight replies) passed
in the same run.

The observation is now re-rolled up to 4 times: a missed window restores the split and runs the
same actuation again, and **the pipelined in-flight traffic is issued on the attempt that won**, so
what it straddles is a flip that was demonstrably moving. Only an exhausted budget reports the
window as not opened, and it prints why instead of turning the row red.

---

## 7. Induced-failure evidence

Every fix had to be shown still to bite. All arms on the release build, cores 136-143.

### Row 1 — the rate assertion

`--atomic-window 1` **alone does not redden it**, and that is a finding about the row rather than
about the fix: pipelining still saves 24 client round trips, so the ratio stays comfortably over
the 1.10 bar even with admission serialized.

| arm | window | per-group cost | pipe | serial | ratio | rate row |
| --- | --- | --- | --- | --- | --- | --- |
| A positive control | 2 | none | 102,987/s | 40,552/s | 2.54 | ok |
| B ASAN-tier form (`--no-rate-assertions`) | 2 | none | 112,702/s | 43,278/s | 2.60 | SKIP, rates printed |
| C first attempt | **1** | none | 77,577/s | 41,643/s | **1.86** | ok — does not discriminate |
| D control | 2 | 2 ms `ATOMIC-COMMIT-DELAY` | 985/s | 489/s | 2.01 | ok |
| E **induced** | **1** | 2 ms `ATOMIC-COMMIT-DELAY` | 494/s | 490/s | **1.01** | **FAIL** |

Give each group a real 2 ms between ticket draw and publication and the arithmetic is decided by
how many groups the window lets decide **at once** instead of by the round trips pipelining saves.
D and E differ only in admission concurrency, on one server, in one run: 2.01 against 1.01, and the
row goes red. The mechanism half tracked the property in every arm (`atomic_window_stalls` 11 at
window 2, 23 at window 1).

### Row 2 — the stable hold

`DEBUG FLIPCTL TRIGGER` fired 5 s into an assertion window that had opened on a certified
stationary load:

```
stable hold: 8s pre-hold window stationary (driver 6001,6001,6000,6000,5999,5999,5999,6000/s,
                                            band 0.0200); assertion window open for 30s
...
AssertionError: controller moved during the stable hold: {... 'flipctl_last_trigger': 'forced' ...}
  last_trigger=forced  band=0.020000  anchor_rate=6000.674
  driver per-second rates: 6002, 6001, 6000 x9, 5999
  per-second trace:
    prehold anchored    anchored   driver=6002/s live=6:2 anchor=6:2 triggers=1 last=boot
    ... (8 prehold + 4 hold samples, all anchored, all 6:2) ...
    hold t=+5.0s maneuvering measuring driver=5999/s live=6:2 anchor=6:2 triggers=2 last=forced
  DEBUG FLIPCTL at the move: ... last_trigger=forced  triggers=2 boot=1 forced=1
```

Exit 1. A move while the load is provably stationary still FAILS, and it still FAILS after the
trigger classification of 6.1 was added.

### Row 3 — the eviction survival rows

The file's own documented inverse: keep the hot set "hot" from a `CLIENT NO-TOUCH` connection so
the reads never touch the LFU metadata. Same binary, same section, back-to-back boots.

| arm | eviction fired | hot 20 survive | hot median `OBJECT FREQ` | verdict |
| --- | --- | --- | --- | --- |
| positive control | 10,532 | **20/20** | 11 (`9,10x5,11x8,13x3,14,15`) | 7 ok, 0 FAIL |
| **induced** (NO-TOUCH hot reads) | 10,532 | **5/20** | **3** (`2,3,3,5,5`) | 5 ok, **2 FAIL** |

Both survival rows go red together, which is what the file predicts for a server whose reads do
not touch.

---

## 8. A server defect the acceptance runs found — NOT a timing row, NOT this lane's to fix

Full-gate run 3 came back 339 ok / 1 FAIL. The red row was **not** of this lane's class: the
armed-fused differential matrix collapsed because **the server segfaulted**.

```
row:      Redis 7.4 differential matrix (armed fused + read-local)
geometry: --thread-mode fused --read-local 1, --atomic 1, 16 shards, 8 threads
first:    differ scan (atomic=1 seed=19)  -> EOFError
then:     every following leg -> ConnectionRefused (the process was gone)
server log ends at "listening on 127.0.0.1:8440" -- no crash text, no shutdown dump

kernel (journalctl -k):
  tomokv[3081117]: segfault at c000108 ip 00005912d5cf68b5 sp 00007a24d07fd7f0 error 4
  tomokv[3081119]: segfault at c000108 ip 00005912d5cf68b5 sp 00007a24cf3fd800 error 4
   in tomokv[1968b5,5912d5b71000+36f000]
```

**TWO threads faulted at the same instruction on the same address**, `0xc000108`, error 4 (a
user-mode read of an unmapped page). That is the signature of two threads dereferencing one
corrupted shared pointer, not of an ordinary null deref. Offset `0x1968b5` resolves to
`tomo::reply_bulk<tomo::Op::Sink>` (`src/net/resp.h:258`); the alternative reading of the base
arithmetic lands in `serialize_notify_flags` -> `std::string::push_back`, which on an SSO-short
string can only fault if the frame or the object was already corrupt. Either way it is the **reply
path**, in the geometry where the read-local lane serves the read.

Frequency: **one occurrence in three full gates** (each runs this matrix once). It was the only
segfault on the box all night.

This lane changed nothing under `src/`, so this is pre-existing on `ceb6b02f8` and belongs to the
lane that owns the armed local-read lane. It is recorded here because it is a P0-shaped finding
and because it is now the only known source of a random red row in the full gate.

---

## 9. Acceptance

Five full gates on cores 136-143, `GATE_PORT=8440`, `GATE_QUIET_FILE` set so the run pauses itself
whenever the box owner needs it. NIC cells skipped (no rig on this scratchpad), so the expected
count is `EXPECT_FULL` = **340**.

| run | result | note |
| --- | --- | --- |
| 1 | **340 ok, 0 FAIL** | first pass of the three fixes |
| 2 | 338 ok, 2 FAIL | `flip.py` in-flight race + the flipctl **fingerprint** move — both diagnosed by the new failure text and fixed in section 6 |
| 3 | 339 ok, 1 FAIL | **server segfault** in the armed-fused differ matrix (section 8) — not this lane's class, nothing under `src/` was touched |
| 4 | **340 ok, 0 FAIL** | first of the required pair |
| 5 | **340 ok, 0 FAIL** | second of the required pair, back to back |
| 6 | **340 ok, 0 FAIL** | third consecutive clean run, taken as extra confirmation |

Runs 4, 5 and 6 are **three** consecutive clean runs — the bar asks for two — all at the expected
row count with `PROGRAM-STATE ledger (340/340 checks) ok`.

The stable hold opened its assertion window on attempt 1 of 3 in every post-fix run, each time
printing the pre-hold measurement that licensed it, e.g.

```
stable hold: 8s pre-hold window stationary (driver 6000,6000,6002,6001,6001,6001,6001,6000/s,
                                            band 0.0200); assertion window open for 30s
stable hold: 30s on a measured-stationary load, no trigger or split movement (attempt 1 of 3)
```

`flip.py` needed no in-flight re-roll in any post-fix run (0 re-roll lines), so the budget is
headroom rather than a routine cost. The flipctl row costs 120 s, unchanged from before this lane.

---

## 10. Incoming timing lane on `78c3e5391` — 2026-09-07

**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
SKIP rows. The account below records that lane's original implementation and offline validation;
it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
are retired: repeatability checks did not replace the deleted manual ownership instrument. The
configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.

The rest of this section is historical lane evidence, including its now-superseded dispatch,
shrink, admission-calibration, and ledger statements.

This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
in `multirace.py` and the RENAME OFF control are unchanged.

| area | change | failure induction (also documented beside the checks) |
| --- | --- | --- |
| `differ.py`, blocking | Wait for both blocked-client gauges to reach zero before final comparison. Assert each server's gauge increment independently, against a settled baseline. | Suppress registration increments to fail movement; suppress disconnect decrements to fail the bounded zero control. |
| `atomic_torn.py`, promotion/leases | Keep promotion movement and credit safety mandatory. Separate the original 1.5s promotion and 3s shrink/reclaim budgets behind `--release-build`. Sample the credit bound across at least 32 observations with completed writer traffic, and require zero inflight/debt plus the exact final credit pool. | Disable promotion or lease returns; over-issue credits after shrink. An observed excess or exhausted mechanism watchdog fails on every build. |
| `slowlog.py` | Derive recording-thread capacity from INFO (`fused_threads`, or `io_threads + ex_threads`), and issue more writes than its aggregate capacity. | Disable ring eviction or max-len trimming. |
| `xshard_dispatch_scale.sh` / `.py` | Disable balancing during the fixed-owner experiment; verify ownership remained fixed. Use distinct reply payloads, check every pair's topology, alternate boot order, and retain every ratio. Only score release timings when repeated arms and same-shard controls agree within the supplied 2% box contract. Every qualified pair must meet the unchanged 1.20 bound; no best-pair selection. | Misroute a key or change a second pair's fan-out to fail mechanism checks; restore the all-thread scan to fail a stable release measurement. Unqualified timings say SKIP with all numbers, not that scaling passed. |
| `pubsub.py` | Drain old pattern registrations before checking the next NUMPAT population; keep exact live-pattern assertions. | Leak the disconnected RESP3 subscriber's pattern; a constant-zero NUMPAT still fails the live arms. |
| `borrow_registry.py` | Replace the 1.5x timing proxy for different paths with `zc_sends`/`zc_releases` deltas and a plain-GET negative control before holders exist. Keep parked-borrow/drain and byte-exact reply checks on every build; cost budgets require `--release-build`. | Disable borrowing, borrow below the cutover, or omit releases to fail mechanisms; restore the linear lookup to fail the qualified release growth budget. |
| `expireindex.py` | Assert exact 2 -> 1 -> 0 TTL accounting, DEL replies, and the surviving value on every repetition. Keep INFO outside both timed DEL intervals. Cost ratios require `--release-build`. | Break expiry unregistration or delete accounting to fail mechanisms; restore the historical-capacity memset to fail the release timing bound. |
| `infofix.py` and its differential counterpart | Poll INFO-only idle samples until zero; feed verified PINGs until the sampler reports work. Also require RESETSTAT to clear those PING counters, so natural rate decay cannot substitute for a reset. | Pin the rate at zero/nonzero, or omit the command-counter reset baseline. |
| `stream.py` | Check exact retained entries through trimming, first-entry creation and embedded-to-macro growth on every build. Report all footprint measurements; score the original byte ceilings only with `--release-build`. Require positive measured allocation instead of clamping negative deltas to zero. | Drop/wrongly trim entries or omit memory accounting to fail mechanisms; inflate first-entry/node allocations to fail release footprint bounds. |

The release gate explicitly passes `--release-build` to the affected batteries. The ASAN atomic
row omits it. Standalone runs against an instrumented binary should likewise omit it; all
mechanism assertions remain active and performance measurements still appear in the log.

### Outstanding atomic window calibration — not claimed fixed

Three window-hit checks remain strict at their existing arming geometry:

* `atomic_torn.py:718`, commit-delay held a read cut;
* `atomic_torn.py:719`, predecessor resolution;
* `atomic_torn.py:923`, admission stalls and resumes.

Their own per-attempt hit/miss measurements were not supplied. The RENAME control's measured
5/6 hit rate does not establish a bound for these different mechanisms. No fresh-connection
retry count was guessed, no window-hit assertion was made release-only, and their observation
windows were not widened. Fresh-state retry hygiene for these three checks still needs the
maintainer's measurements at `--shards 16 --ratio 6:2`, cores `0-7`, separately for release and
ASAN. Record each attempt's counter deltas and any real value/worker failure; such failures must
stop retrying immediately. This is the unfinished part of item 1, not an acceptance claim.

### Static and offline checks

Python AST parsing, `bash -n` and `git diff --check` passed. Source-extracted helper checks with
synthetic inputs covered successful and exhausted gauge/pattern polls, 8/16-thread INFO
geometries, release-flag behavior, exact TTL transitions, stuck sampled rates, and excess/no-work
lease samples. The dispatch result evaluator rejected a synthetic stable 1.30 ratio, incorrect
second-pair geometry, negative excess and a missing pair, while explicitly skipping unstable
timings. These are harness checks, **not** server fault-injection or PRE/POST measurements.

### Ledger

No top-level row was added or retired; the `ok`/`bad` label sequence is unchanged. In this diff
the quick exit is at `gate.sh:1262`. The release atomic row (`:434`), stream feature-loop calls
(`:457`, `:516`), expiry (`:674`), borrow (`:682`) and dispatch (`:688`) rows are before it; the
ASAN atomic row (`:1275`) is after it. Internal assertions do not add ledger rows.
`EXPECT_QUICK=327` and `EXPECT_FULL=344` remain unchanged, and no count adjustment is needed.
