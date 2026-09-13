# NOTES-BARRIER — the connection parse barrier is one bool with six owners

Lane `t-barrier`. Brief: `NOTES-STOREORDER.md` §9 handed on a found-but-not-fixed item —
`blocking_retire()` and `blocking_scatter_retire()` clear the connection's scatter barrier
unconditionally, with no test that the barrier is theirs. Verify it from the source, decide whether
it is live or latent, and harden it.

**Outcome: verified, corrected in two details, and hardened.** The single bool becomes an owner
**bitmask**; `blocking_retire` now releases only the bit it acquired. The item is confirmed
**latent, not live** — and the reason is sharper than §9 states: a blocking op is provably
**alone in its ROB**, so the barrier at its retirement can have no other owner. Because exactly one
owner can hold the barrier today, the mask is bit-identical to the bool on every reachable path;
the change is a **behaviour-preserving refactor** whose only effect is that the two stop being
identical the moment a seventh owner, or a relaxed guard, makes overlap reachable.

**No build, no server, no bench was run in this lane** (main session holds the box). Everything
below is static reading. §5 describes the check and its geometry rather than reporting it.

---

## 1. The set / clear table

Grep basis: `grep -rn scatter_barrier src/`, whole tree, plus every caller of the accessors.
Line numbers are the tree **as found**, at `c0d7f27fd` — i.e. before this lane's diff, so the
table reads against the code the audit was made on. §9's numbers are off by 2–69 lines: they were taken
before the `t-merge14` merge train (`blocking.inc:1275/:1286` → `1277/1288`,
`multi.inc:1365` → `1434`).

### SET sites — 8 sites, 6 owner classes

| # | Owner class | Site | Guard at the moment of the set | What ends the parse pass |
|---|---|---|---|---|
| 1 | **Barriered scatter** (two-hop store / all-shards) | `src/core/io_loop.h:1530` (plain-scatter arm) | `scatter_dispatch.barrier && rob.in_flight() != 0` ⇒ `break` — **ROB empty required** (the `t-storeorder` fix, `io_loop.h:1478`) | `continue` → loop head `io_loop.h:1113` breaks |
| 2 | same, bundled atomic-write arm | `src/core/io_loop.h:1593` | same guard, same `if` | `continue` → loop head breaks |
| 3 | **Blocking command** (`BLPOP`/`BLMOVE`/`XREAD BLOCK`…) | `src/core/io_loop.h:1437` | `rob.in_flight() != 0` ⇒ `break` (`io_loop.h:1391`) — **ROB empty required** | explicit `break` |
| 4 | **Deferred `WAIT`** | `src/core/io_loop.h:1305` | `WAIT` is `ConnLocal｜OrderedLocal｜DeferredLocal` (`server_tail.cc:952`); the `OrderedLocal` gate at `io_loop.h:1288` is `rob.in_flight() != 0` ⇒ `break` — **ROB empty required** | explicit `break` |
| 5 | **`EXEC` fan-out** | `src/cmd/multi.inc:1434` | **none** — `multi_dispatch_entry` has no `in_flight` test | `return true` → `io_loop.h:1215` `continue` → loop head breaks |
| 6 | **Pub/sub transition** (`(P｜S)(UN)SUBSCRIBE`) | `src/core/pubsub.inc:850` (`pubsub_begin_pending`) | **none** | returns `Async` → `io_loop.h:1264` `if (scatter_barrier()) break` |
| 7 | **`CLIENT UNBLOCK`, remote owner** | `src/cmd/climon.cc:524` | **none** | `Async` → same `io_loop.h:1264` break |
| 8 | **`CLIENT LIST` / `CLIENT KILL` fan-out** | `src/cmd/climon.cc:680` (`climon_begin_client_scatter`) | **none** | `Async` → same `io_loop.h:1264` break |

### CLEAR sites — 3

| # | Site | Condition today | Scope |
|---|---|---|---|
| A | `src/core/io_loop.h:1849` (`flush_ready`) | `if (c->rob().quiesced())` | **all owners** — the correct backstop: with nothing in flight every production owner has completed by definition |
| B | `src/cmd/blocking.inc:1277` (`blocking_retire`) | **unconditional** | all owners ← **the reported defect** |
| C | `src/cmd/blocking.inc:1288` (`blocking_scatter_retire`) | **unconditional** | all owners ← **the reported defect** |

### Two corrections to §9

1. **`CLIENT PAUSE` does not set the barrier.** §9 names owner 6 as "CLIENT PAUSE/KILL". `climon.cc:524`
   is `CLIENT UNBLOCK` against a *remote* owner and `climon.cc:680` is the `CLIENT LIST`/`CLIENT KILL`
   fan-out. `CLIENT PAUSE` uses a **completely separate** mechanism — `climon_pause_holds()`
   (`climon.cc:369`) leaves the parsed frame at `rpos` and `flush_ready` declines to count it as work
   (`io_loop.h:1896`) so the ring parks instead of spinning. It never touches `scatter_barrier_`.
   Attributing it to the barrier would have sent the hardening after the wrong owner.
2. **The count of six is right, but there are eight sites**, and the two that matter for the
   reachability argument are the two that *do* carry a ROB-empty guard (#1/#2 and #3), which §9's
   phrasing ("a blocking command requires the ROB head to issue") gets right for #3 and silently
   generalises to the rest. Owners #5–#8 have **no** `in_flight` guard at all: `EXEC`, a subscribe,
   and `CLIENT KILL` can each set the barrier with older ops of the same connection still in flight.
   That is fine — but it means the invariant is *not* "the barrier is only ever set on an empty ROB",
   and any future reader who assumes that will be wrong.

---

## 2. Reachability verdict — LATENT, not live

**Verdict: not reachable today.** The reason §9 gives is directionally right but incomplete; the
airtight statement is:

> **A blocking op is always alone in its ROB.**

Both halves are enforced, at two sites 47 lines apart in a different file from the clear:

* **Nothing older.** `io_loop.h:1391` — `if (spec->flags & CmdFlags::Blocking) { … if (rob.in_flight() != 0) break; }`.
  The blocking op cannot even be *prepared* until every older op of the connection has retired.
* **Nothing younger.** The same dispatch sets the barrier (`io_loop.h:1437`) and `break`s; the parse
  loop's head (`io_loop.h:1113`) and `flush_ready`'s re-parse gate (`io_loop.h:1894`) both refuse to
  parse while it is set. No byte behind the blocking frame is even looked at.

Therefore, at the instant `blocking_retire` runs, `dispatch_id - flush_id == 1` and the single live
op *is* the blocking op. The barrier's only possible owner is the one being released. Clearing
all owners and clearing one owner are the same operation.

The same argument covers **`blocking_scatter_retire`**: `state->blocking_origin` is written at exactly
one site — `blocking.inc:1203`, inside `blocking_resume_move` — which converts an *already parked*
blocking op in place (`op.detach_blocking_state(); op.attach_scatter_state(...)`). It creates no new
ROB slot, so the "alone in its ROB" property is inherited unchanged.

And **`BlockingState` cannot reach the ROB by any other route**: `attach_blocking_state` has exactly
one caller, `io_loop.h:1418`, behind the guard above. In particular a blocking command lowered inside
`MULTI` never produces one — `EXEC` children are not ROB ops and the parent `EXEC` op carries
`multi_state`, not `blocking_state`.

### Interleavings I checked and rejected

| Candidate | Why it does not fire |
|---|---|
| Pipelined `EXEC` / `SUBSCRIBE` / `CLIENT KILL` **then** `BLPOP` in one write | The first command sets the barrier and ends the pass; the `BLPOP` bytes are never parsed until the barrier drops at quiescence, by which time the first owner is done |
| Pipelined `BLPOP` **then** `EXEC` | `BLPOP` sets the barrier; `EXEC` is never parsed |
| Second owner set from **outside** `parse_and_dispatch` while a blocking op is parked | There is no such path. `blocking_resume_move` (the only thing that runs on a barred connection, `io_loop.h:1845`) does **not** acquire the barrier — it inherits the one already held. Pub/sub and climon event delivery on the io thread only ever set the barrier on the *requesting* client, never on a victim; `CLIENT KILL`/`UNBLOCK` reach a victim through `mark_closing()` / `blocking_request_unblock()`, neither of which touches `scatter_barrier_` |
| Two connections | The barrier is per-`Client` and every site writes it through the connection's own io owner. No cross-connection write exists |
| A `Blocking`-flagged command that skips the guard | Only `XREAD` without `BLOCK`, which `goto nonblocking_dispatch`es before the guard and never attaches blocking state |
| Deferred `WAIT` coexisting with a blocking op | `WAIT` is `OrderedLocal`, so it too needs `in_flight()==0`, and it too ends the pass |

### Two things this audit found that make the item worse than "latent"

1. **The invariant that saves it lives nowhere near the code that depends on it.** The clear is in
   `src/cmd/blocking.inc`; the guard is in `src/core/io_loop.h`, in a different translation unit,
   behind a command-flag test, with a comment that explains it in terms of *program order* and never
   mentions the barrier's ownership. That is the exact shape of the defect `t-storeorder` fixed.
2. **`rob().quiesced()` is FALSE inside `blocking_retire`, always.** `Rob::drain` (`rob.h:88-103`)
   calls `sink(op)` for the op at `flush_id` and stores the advanced `flush_` **once, after the whole
   batch**. So the retire callback always observes `in_flight() >= 1`. This kills the second option
   §9 offers ("have `blocking_retire` clear it only when the ROB is quiescent") outright: written
   literally it would **never** clear the barrier and every blocking client would wedge until the
   `flush_ready` backstop ran. The only workable spelling of that alternative is
   `rob().in_flight() == 1` — a magic constant that silently encodes the drain's internal
   bookkeeping. See §4.

---

## 3. `blocked_` has the same shape (found, not fixed)

`client.set_blocked(false)` sits one line above the barrier clear in both retire functions and is
equally unconditional. `blocked_` is set by **two** owners — the blocking dispatch (`io_loop.h:1436`)
and deferred `WAIT` (`io_loop.h:1304`) — and is saved by the same "each requires `in_flight()==0`"
argument. Not converted here: unlike the barrier, `blocked_` is a `connection_flags_` bit with three
readers that mean different things by it (`blocking_resume_move`, `blocking_cancel_client`,
`blocking_request_unblock`, and `CLIENT UNBLOCK`'s 0/1 reply), and giving it owners changes the
`CLIENT UNBLOCK` reply surface. Recorded, deliberately untouched.

---

## 4. The change, and why a mask rather than a count or a quiescence test

`bool scatter_barrier_` becomes `uint8_t barrier_owners_`, a bitmask over `BarrierOwner`
(`src/net/conn.h`). Six production bits, one spare (`1u << 6`), one test bit. `scatter_barrier()`
still answers `barrier_owners_ != 0`, so the parse-loop gate at `io_loop.h:1113` is the same byte
load and the same test against zero it always was.

### Why not the owner COUNT §9 suggests

A count fits the same byte, and it looks equivalent. It is not, for one reason that is decisive
here: **four of the six owners have no owner-scoped release at all.** `WAIT`, `EXEC`, a pub/sub
transition and a `CLIENT` fan-out are released *en masse* by the quiescence backstop at
`io_loop.h:1879`, which does not know — and must not have to know — how many claims are
outstanding. Releasing a mask at quiescence is one AND. Releasing a *count* at quiescence means
either storing zero (which is the shared-flag bug again, just spelled with a wider type) or
teaching the backstop the claim arithmetic of every owner.

The failure modes are also strictly worse than the bool's. A count that leaks one claim never drops
the barrier: the connection stops parsing **forever** and the client hangs. A count that
double-releases wraps 0→255 and wedges just as hard. The bool at least always converged on
"released". A mask is idempotent in both directions — a second acquire by the same owner is a no-op,
a second release by the same owner is a no-op — so neither failure exists.

And a count answers the wrong question. It says "how many claims are outstanding", not "is this
barrier mine". The brief's own requirement — *whoever sets it must be the one whose release drops
it* — is an **identity** property, and only owner bits express it.

The one thing a mask cannot do is let a single owner class hold two simultaneous claims. No owner
does today (each acquires once per parked lifetime, and the parse pass stops behind it), and the
enum comment says so. An owner that ever needed re-entrancy would need its own second bit.

### Why not "clear only when the ROB is quiescent"

Because it cannot be written. `Rob::drain` (`rob.h:88`) calls the retire callback for the op at
`flush_id` and stores the advanced cursor **once, after the whole batch** — so
`rob().quiesced()` is false inside `blocking_retire`, always, for every op, including the last.
Spelled literally the barrier would never be cleared there and every blocking wake would wait for
the `flush_ready` backstop.

The workable spelling, `rob().in_flight() == 1`, is a magic constant three files away from the
bookkeeping it encodes — the same "invariant enforced somewhere else" disease this lane exists to
cure. And it still answers "is the connection otherwise idle", not "is this barrier mine". Those
two coincide today for exactly the reason the defect is latent, which is not a property to build on.

### Cost

Zero, and structurally so rather than by measurement (this lane runs nothing):

* **Footprint.** `uint8_t` replaces `bool` in the *full* 48..55 bool run. `connection_flags_` stays
  at offset 55 — its `static_assert` in `conn.h` is the guard — `tls_slot_` stays at 1980, and
  `sizeof(Client)` stays 1984. No new member, nothing at the cold tail, nothing moved.
* **Hot path.** `scatter_barrier()` was `movzx`/`test` on a byte and still is: `x != 0` on a
  `uint8_t` and a `bool` load compile to the same test. `GET`/`SET` never reach an acquire or a
  release; the six owners are `EXEC`, a subscribe, a blocking command, a deferred `WAIT`, a
  barriered scatter and a `CLIENT` fan-out.
* **Backstop.** `barrier_release_quiesced()` is `and byte ptr, 0x80` where it was `mov byte ptr, 0`.
  One instruction either way, on an arm that only executes while a barrier is already set.
* **Debug hook.** One relaxed load of a server word that is zero in production, on the blocking
  dispatch arm; and in `flush_ready` one byte test of the connection's own bit, *before* the
  server-wide load, inside an arm that already only runs when a barrier is up. Off ⇒ the load never
  happens.

### What deliberately did NOT change

Only `blocking_retire` and `blocking_scatter_retire` gained an owner-scoped release. The other
four owners keep the quiescence backstop as their release, byte for byte. Giving `WAIT` an explicit
release in `deferred_wait_pass`, for instance, would drop the barrier **before** its op retires and
let younger frames dispatch ahead of the `WAIT` reply's staging — a real behaviour change on a path
this lane cannot test. Every clear that happened before still happens, at the same moment; the only
difference is that `blocking_retire` no longer clears bits it does not own. On today's reachable
paths the two are the same store, which is the point: **this lane's diff is not supposed to change
any observable behaviour.**

---

## 5. Validation — DESCRIBED, not written and not run

This lane never built and never booted anything. What follows is the check to write, and — more
importantly — the geometry it must find and the way it must fail.

### The hardening is not observable without an injector, and that is the finding

The state the owner-scoped release exists to survive is *two owners at once*, and §2 proves no
command sequence produces one. A battery that only replays real traffic therefore cannot
distinguish this change from a no-op — it would pass on the pre-lane binary too, which is the
definition of proving nothing. So the check injects the second owner:

`DEBUG BARRIER-HOLD 0|1` (`t_server.cc`, gated by `--enable-debug-command yes`). While armed, every
blocking dispatch additionally pins `BarrierOwner::Debug`, which the quiescence backstop is written
not to release. Clearing the latch releases it on the connection's next `flush_ready` pass.

**A latch, not a duration, and that is load-bearing.** A barred connection's io thread has nothing
left to do and parks on `ring_.submit_and_wait(1)` — **unbounded** under io_uring (`io_loop.h:353`;
only the epoll engine has the 50 ms ceiling). A hold that expired on a clock would never be
noticed and the connection would hang instead of resuming. The releasing edge has to be an event.

### The check

Boot: `--enable-debug-command yes`, and `--shards N` with `N` equal to the executor count (see the
geometry note). Two connections, `A` and `B`, and `A` must be the one that blocks.

```
 A: DEBUG BARRIER-HOLD 1                       -> +OK
 A: <ONE write>  BLPOP kb 0  ;  ECHO probe     (probe is pipelined BEHIND the blocking command)
 B: LPUSH kb v                                 -> :1        (wakes A's BLPOP)
 A: read                                        -> the BLPOP reply, and NOTHING after it
    ---- assert: no "+probe" within 500 ms ----             <- the discriminator
 B: INFO STATS
    ---- assert: barrier_releases_held > 0 ----             <- the gate actually opened
    ---- assert: barrier_owner_overlaps > 0 ----            <- the counter can count
 B: DEBUG BARRIER-HOLD 0
 B: CLIENT LIST                                (fans an event to EVERY io thread -> wakes A's)
 A: read                                        -> "+probe"                                  
    ---- assert: probe arrives ----                         <- it was a HOLD, not a wedge
```

`CLIENT LIST` is the wake, and it is not incidental: `climon_begin_client_scatter` posts an event to
every io thread in `placement().ifid_threads()` and `pubsub_post` reaches a remote one through
`post_client`, the established notify/park protocol (`pubsub.inc:99`). Nothing else a *second*
connection can do is guaranteed to wake the *first* connection's io thread, and the first connection
cannot be parsed at all — that is what being barred means.

### Geometry — what it must find, and what it must never assume

| Axis | Requirement | Why, and what gets it wrong |
|---|---|---|
| **Connections** | **Exactly one** connection carries both owners; a second drives it | The barrier is a field of `Client`. N connections give N independent barriers and **zero** overlap. A battery that spreads blocking load over 32 connections to "increase the chance" has a chance of exactly zero. This is the single most important line in this section |
| **Which two owners** | `BarrierOwner::Blocking` (the releaser) **and** one other, held simultaneously | Today the only possible "other" is the injected `Debug` bit. That is the finding, not a shortcut |
| **Pipelining** | The probe must be in the **same `write()`** as the blocking command | It must already be at `rpos` when the barrier goes up. A second write also works but adds a race between the write and the barrier; one write removes it |
| **Executors** | **≥ 2** for the `BLMOVE` arm; `INFO`/`CONFIG GET` it and abort if fewer | A one-executor boot silently makes every move same-owner, and the `blocking_scatter_retire` arm then never runs the two-hop path it claims to cover |
| **Shards** | Boot `--shards == <executor count>` | Shards go to executors by flat round-robin in thread-id order (`server.h:195`), so with that equality **distinct shard ⇒ distinct owner** and `DEBUG SHARD` becomes a true owner oracle. At the default 16 shards, two distinct shards may share an owner and a "cross-owner" claim is unproven |
| **Keys** | `BLPOP` arm: any single key. **`BLMOVE` arm: `src` and `dst` on different owners** | `blocking_resume_move` builds the move through `xshard_prepare`, and `state->two_hop` — the property that makes it a *barriered* scatter — is only true cross-owner. A same-owner `BLMOVE` battery exercises a different path and proves nothing about this one |
| **Key discovery** | **Walk candidates and bucket them with `DEBUG SHARD`.** Never hard-code `k1`/`k2` | The hash seed is drawn from the kernel at every boot. `tests/storeorder.py:288` and `tests/execfix.py:159` are the pattern to copy |
| **io-thread pairing** | Do **not** assume `A` and `B` share an io thread, and do not assume they differ | `SO_REUSEPORT` decides. Neither the wake nor the resume depends on it (both go through the cross-io notify protocol), but a check that quietly relies on co-residency would be boot-dependent. Run several `A`/`B` pairs |

**Failing loudly.** Each of these aborts the run with a fatal, never a pass:
`DEBUG SHARD` refused (server not booted with `--enable-debug-command yes`);
fewer than two distinct owners found in the candidate walk;
no cross-owner `(src, dst)` pair found for the `BLMOVE` arm;
`barrier_releases_held` unchanged across the armed arm.

### Distinguishing "held by the remaining owner" from "never set at all"

One cell cannot do it. Four can, and all four run on the same binary in the same boot:

1. **Armed** (latch on). Probe must **not** answer; `barrier_releases_held` must increase by exactly
   the number of blocking retirements in the arm; `barrier_owner_overlaps` must increase by exactly
   the number of blocking dispatches. *Barrier was set, and a release did not drop it.*
2. **Negative control** (latch off, identical script). Probe **must** answer in the same read as the
   blocking reply; both counters must stay put. *The probe can pass — so cell 1's silence is caused
   by the hold and not by a broken probe, a closed socket, or a timeout of its own.*
3. **Resume** (clear latch, `CLIENT LIST`). Probe must answer. *Cell 1 was a hold, not a wedge.
   Without this, "correctly held" and "permanently broken" look identical.*
4. **Production overlap** (latch off, run `blocking.py`, `blockmulti.py`, `multi_exec.py`,
   `climon.py`, the pub/sub battery and `storeorder.py`). `barrier_owner_overlaps` must read
   **0**. *This is §2's reachability verdict as a live assertion.* It is only meaningful because
   cell 1 showed the counter can be moved — a "must be zero" reading of a counter nothing has ever
   been shown able to increment proves nothing.

**Engine-level positive control**, and the check is worthless without it: rebuild with
`blocking_retire`'s release replaced by the pre-lane `barrier_owners_ = 0`. Cell 1 must then FAIL —
the probe answers immediately and `barrier_releases_held` stays 0. A hardening whose control build
passes its own test has not been tested.

### Cost check

`DEBUG BARRIER-HOLD` off, so nothing is armed: the existing gate (`tests/gate.sh`) plus a `p32`/`p1`
`GET`/`SET` A/B against the merge base. The expectation is a wash inside noise — the diff adds no
instruction to any path a plain command takes — and anything outside that says a supposedly free
refactor was not free.

---

## 6. Also found, not fixed

* **`blocked_`** — §3. Same unconditional-clear shape, two owners, saved by the same argument.
* **Four owners with no owner-scoped release.** `WAIT`, `EXEC`, pub/sub and the `CLIENT` fan-out all
  rely on the quiescence backstop. That is correct — their parked lifetime *is* their ROB slot — but
  it means their `BarrierOwner` bits are named and never explicitly dropped. If one of them ever
  grows a completion path that fires before its op retires (the way `deferred_wait_pass` already
  does for `blocked_`), it needs a real release site, and the bit is now there to write it against.
* **`io_loop.h:1264`'s conditional break.** A pub/sub command that returns `Async` **without**
  setting the barrier lets the parse pass continue. `pubsub_start_publish` (`pubsub.inc:952`) is
  that case, and its comment argues it explicitly — publish requests enter each channel home's FIFO
  in parse order, so the home is the sole delivery-order authority. Read, believed, not touched;
  recorded because it is the one `Async` path where "returns Async" and "parks the connection" come
  apart, and a future reader will trip over it.
* **`scratchpad/aclprobe/holes.cc` and `scratchpad/tls_layout_probe.cc`** mirror the `Client`
  declaration order and still spell the field `bool scatter_barrier_`. Layout-identical (`uint8_t`
  and `bool` are both one byte, alignment 1) so the mirrors remain valid; left alone as scratch.

---

## 7. Files changed

| File | Change |
|---|---|
| `src/net/conn.h` | `BarrierOwner` enum; `barrier_acquire` / `barrier_held_by` / `barrier_release` / `barrier_release_quiesced`; `bool scatter_barrier_` → `uint8_t barrier_owners_` (same byte, same offset) |
| `src/core/io_loop.h` | `barrier_arm()` — the one acquire door, counts overlap; four acquire sites converted; quiescence clear → `barrier_release_quiesced()`; `DEBUG BARRIER-HOLD` acquire + release |
| `src/cmd/blocking.inc` | both retire paths release **only** `BarrierOwner::Blocking` and count a release that did not drop the barrier |
| `src/cmd/multi.inc`, `src/core/pubsub.inc`, `src/cmd/climon.cc` | acquire through `barrier_arm` with their own owner bit |
| `src/core/server.h` | `debug_barrier_hold` latch; `barrier_owner_overlaps` and `barrier_releases_held` counters |
| `src/cmd/t_server.cc` | `DEBUG BARRIER-HOLD 0\|1`; both counters in `INFO STATS` |
