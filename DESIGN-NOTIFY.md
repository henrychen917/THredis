# DESIGN-NOTIFY — completion notification: per-batch coalescing (A) and per-producer ready words (B)

Lane `t-fable-notify`, base f40469ea3 (mainline). Owner-approved ideas A and B of AUDIT-NETWB §6.
Measured motivation (1s armed profile, 32 cores, p32, 1:1 mixed): `notify_sender` = 3.6% of
instructions and 5.2% of cycles; it executes one `mfence` per completed op (ex_loop.h ~2968) and
every executor `fetch_or`s into the same two cache lines of the io thread's `ready_` mask
(signal.h ReadyMask, ≤512 conns per line), so those lines bounce across CCXs (cross-CCX fills/op:
2.2 pure GET → 14.7 pure SET).

Nothing in this lane is booted, benched or profiled (cores reserved). Every expected number below
is reasoned from the code and the audit anchors; the owner measures pure SET, 1:9, 1:1, p1 latency,
both thread modes, and the flip battery.

---

## 1. The protocol being preserved (as of f40469ea3)

**Producer (executor), per completed op** — ex_loop.h `execute()` tail, `multi` Final, and the
fused read-local chunk path; plus the free function `blocking_notify_sender` (blocking.inc:325):

1. `op.state.store(Done, release)` — the reply bytes become visible through this store.
2. `notify_sender(c)`: load `ifid_thread_`, load `wb_slot_` (both on the executor-facing Client
   line, conn.h:1920-1980). Fused self-path: direct `fused_completion_` call, no atomics.
3. Slot path: `std::atomic_thread_fence(seq_cst)` (**the per-op mfence**), then
   `ReadyMask::set(slot)` = relaxed load of the word, `fetch_or(seq_cst)` only on the
   empty→flagged edge. The RMW is a full barrier; whoever performs the edge owes the wake:
   `wake_if_parked` = load `ring_`, acquire-load `parked_`, `msg_ring` SQE if parked.
4. No slot (`kNoWbSlot`, table full): the exact channel path `notify_sender_to` — CAS claim on
   `retire_queued_`, `post_client` through `client_in_[producer]` + `client_notify_` bit + wake.

**Consumer (io thread), per pass** — io_loop.h `collect_retire_work` (~5365): drain the
`client_in_` channels by mask, then `ready().take(w)` for all 16 words (load-first exchange),
map `slot → Client*` through the sender-owned `slots_` table, `enqueue_serve` + `mark_active_known`.
Serving drains the ROB in order (`Rob::drain`, rob.h:454): stops at the first op not Done.

**Park** — `arm_blocked()` (thread.h:862): store `parked_ = true`, arm every channel, **seq_cst
fence**, then `any_io_inbound()` = `ready_.any()` ∨ notify masks ∨ channel depths. Only if all are
empty does the thread block (`submit_and_wait(1)`, 50 ms ceiling). Before that it runs the
mask-independent `sweep()` (io_loop.h:5301), which calls `collect_retire_work(true)` — every word
of the ready mask is scanned there too.

**Mask-independent lookers (house rule: a hint is never the only looker)**: the sweep before park;
the backstop pass (`kFlushBackstopEvery = 64`: every 64th `flush_ready` serves every active
connection regardless of hints, io_loop.h:6482); and the exact `client_in` channel path when a
connection has no slot.

**Why the fence exists** (defect 5, third appearance — comment at ex_loop.h ~2966): `set()` begins
with a relaxed LOAD; TSO lets that load run ahead of the Done store draining. Unfenced, the load
can read a stale 1 from a signal the sender is consuming right now: the producer skips its set, the
sender's drain reads the op before Done lands, and nobody ever signals again.

---

## 2. A — per-batch completion notification (executor side)

### Design

`ExLoopT` gains a small tail-resident record: `notify_batch_[]` of `(io, slot)` pairs sized to the
loop's largest batch (32 split / 128 fused pipeline), a count, and an `open` flag. The two batch
entry points, `exec_batch_prefetched` and `exec_batch_prefetched_buffered` (which also cover the
slowlog-armed `exec_batch_timed`), open the batch for their duration through an RAII scope and
flush at exit.

`notify_sender(c)`, slot path, while a batch is open: **record `(ifid_thread, wb_slot)` and
return** — no fence, no mask load, no wake decision. The two loads it still does are the same two
executor-facing-line loads it did before. It records the io id and slot, never the `Client*`: the
flush runs up to a whole batch later and a `Client*` is only guaranteed alive until the io's second
reap prologue after close (io_loop.h `reap_dead`), a window a 128-op batch can exceed on loopback.
A stale `(io, slot)` at flush is benign (§4.4).

`flush_notify_batch()` at batch end:

1. **one** `std::atomic_thread_fence(seq_cst)`;
2. for each recorded entry, skipping adjacent duplicates (a connection's ops arrive in runs, so this
   catches nearly all repeats; a non-adjacent repeat costs one L1-hit load and is idempotent):
   `ReadyMask::set(slot)` on the target io; remember which ios saw an empty→flagged transition;
3. for each such io, **one** `wake_if_parked` (§4.3 shows one wake decision per io after all of its
   sets is equivalent to today's one per transition).

If the record fills (cannot happen with ≤1 completion per task, but the guard makes correctness
independent of sizing) it flushes in place and continues.

Outside an open batch — `service_stale_forwards`, `service_xshard_retries`, `service_ordered_
deferred`, `service_atomic_deferred`, `service_multi_retries`, snapshot tasks, the fused read-local
chunk path, `blocking_notify_sender` — `notify_sender` behaves exactly as today (fence + set + wake
per op). The fused self-path (producer == consumer) is untouched: a direct call, no atomics, as
cheap as today.

**One-op batch degrades exactly to today**: the record holds one entry; the flush does fence →
set → wake-if-transition, the same three steps in the same order, with no other store between the
Done store and the fence that could matter.

### What it buys (reasoned)

Per 32-op batch: 31 fewer `mfence` (≈35 cycles each per the audit, plus the store-buffer drain each
one forces while a cross-CCX Done store is still in flight — the drain is the part that shows up
as 5.2% cycles on 3.6% instructions). Per op the recorded path is ~10 plain instructions versus
~18 with a fence; the flush adds ~12 per distinct client run and ~5 per woken io. At p32 with
client runs of 3-6 ops the instruction count per write is roughly neutral to −3; the cycle saving
is the fences. The io sees one hint per client per batch instead of one per op, so fewer partial
drains and wider sends at depth.

Cost: the first reply of a batch is signalled at batch end — bounded by the batch (32 ops ≈ 5-15 µs
in 2s; 128 ops ≈ 30-60 µs in the 1s pipeline). At p1 a batch is one or a few ops: no change.

---

## 3. B — per-producer ready words (consumer-owned, single-writer)

### Design

`ReadyMask` (16 shared words in two lines inside `ThreadCtx`) is replaced by `ReadyWords`: **one
64-byte line per producer thread**, in a heap sidecar of `nthreads` lines owned by the consumer
`ThreadCtx`. Word `p` is written only by executor thread `p` (`fetch_or`) and cleared only by the
consumer (`exchange(0)`). Bit `b` of word `p` means: *some connection of this io whose slot ≡ b
(mod 64) has completions from producer p*. The slot table stays exact (`slots_`, 1024 entries).

**Producer**: `set(p, slot)` = relaxed load of `lines_[p]`, `fetch_or(1 << (slot & 63), seq_cst)`
on the empty→flagged edge; returns whether this call performed the edge. Same read-first guard,
same fence discipline as today (the caller's seq_cst fence precedes the load).

**Consumer** (`collect_retire_work`): scan **every** producer word `p < nthreads` (load-first
`take`, seq_cst exchange when non-zero). For each set bit `b`, resolve the residue class
`{b, b+64, b+128, …} ∩ [0, slots_.size())` by checking each candidate's ROB head:
`Rob::head_done()` = `dispatch != flush && slot(flush).state == Done`. Only a head-Done connection
can retire anything, so only those are served. Both ROB frontiers are io-owned lines (L1 hits); the
head Op line is loaded only for candidates with work in flight, and a true positive would load it
in the serve anyway. With 28-70 connections per io (512-2048 conns / 18-30 ios) a residue class has
1-2 members.

`ReadyMask::clear(slot)` at slot release is gone: a residue bit cannot be cleared per slot, and it
does not need to be — a stale bit resolves against the ROB of whatever occupies the slot (nothing,
or a connection with no Done head) and is dropped. `client_in` remains the exact fallback for
slot-less connections; the sweep still scans every producer word; the backstop pass still serves
every active connection.

### Shape: why one line per producer and not 16 words × 8 bytes

The audit's "16 × 8 bytes, the same 128 bytes" makes each *word* single-writer but leaves 8 writers
per *line*. Every `fetch_or` takes the line exclusive and invalidates the other seven producers'
copies and the consumer's; their next read-first loads miss again. That is the bounce B exists to
remove, reduced from E writers to 8, not eliminated. It also cannot be single-writer at all when the
executor count exceeds 16: 34 executors at 64c 30:34, and every thread in 1s mode. So:

- **Indexing by producer thread id, `nthreads` lines.** No ordinal table, nothing to rebuild at
  FLIP: a thread that becomes io simply stops writing its line; a thread that becomes ex starts.
  Correctness never depends on which producer set a bit (the consumer resolves by ROB, not by
  producer), so a stale or shared index could only cost contention, never a reply.
- **Memory**: `nthreads × 64 B` per thread — 2 KB at 32 threads, 4 KB at 64, 8 KB at the
  `kMaxThreads` ceiling — versus 128 B today. Trivial.
- **Consumer scan**: `nthreads` loads per pass versus 16. A line no producer wrote since the last
  pass is in the consumer's L1/L2 in S state (1-4 ns); lines that were written carry information
  and had to be fetched under any design. Expected +16..+48 L1-hit loads per io pass at 32-64
  threads, ≈ 1-5 ns per op at 30 ops per pass. At p1 the io is loadgen-bound and the pass is
  dominated by the ring wait; this is the p1 risk to watch (§7).
- **Layout lock**: `ThreadCtx` stays 1408 and every member keeps its offset — `ReadyWords` is a
  64-aligned 128-byte member occupying exactly the bytes `ReadyMask` did (sidecar pointer, line
  count, documented reserve). `Client` (1984) and `Rob<64>` (192) are untouched; `Rob::head_done()`
  is a method, not a field.

### Coherence accounting (reasoned)

Today, per io pass with E active producers: E RMW fills on the shared line plus up to E×(E−1)
re-fills from the other producers' read-first loads after each invalidation, on 1-2 lines. With
E = 14 (32c 18:14) that is up to ~200 line transfers per pass, per io, for ~20 ops: the order of
the 14.7 fills/op measured on pure SET is consistent with the notify lines being a large share of
it (the value bytes and the Op line account for the rest).

With B: 2 transfers per (active producer, io) pair per pass — producer takes the line exclusive
for its RMW, consumer takes it back for its exchange — independent of E. E = 14: 28 per pass for
~20 ops ≈ 1.4 fills/op, no line ever shared by two writers.

With A on top: a producer writes an io's line at most once per batch per residue bit, not once per
op, and the consumer's exchange follows once per pass.

---

## 4. The lost-wakeup argument (A and B together)

### 4.1 Model

x86-TSO, which is what the tree already relies on (the mfence comments, `-march=native`). Locked
RMWs and `mfence` are full barriers, totally ordered with each other; a store is globally visible
before any later full barrier of the same thread completes; a load that executes after a remote
locked RMW has globally committed observes it. In C++ terms every RMW on a ready word is seq_cst
(the consumer's `take` exchange is made seq_cst in this lane — identical code, `xchg`, and it makes
the fence rules below apply without appeal to hardware), the producer's fence is seq_cst, the
consumer's park fence is seq_cst.

Definitions. For a completed op `o` of connection `c` on io `I`, slot `s`, producer `P`:
`D(o)` = the Done store; `F` = P's seq_cst fence after it (per op today, per batch with A);
`L` = P's relaxed load of the word in `set()`; `R` = P's RMW if `L` saw the bit clear;
`X_k` = the consumer's k-th exchange (take) of that word; `V_k` = the consumer's loads that resolve
`X_k` (the ROB-head loads and the serve's `Rob::drain` acquire-load of `state`).

### 4.2 Claim 1 — visibility: every completion is seen by a take that follows it

*If some `X_k` follows `L` in real time, `V_k` observes `D(o)`.* `D(o)` precedes `F` precedes `L`
in program order; `F` is a full barrier, so `D(o)` is globally visible before `L` executes; `X_k`
is after `L`, and `X_k` is a full barrier, so `V_k` (after `X_k`) observes `D(o)`. In C++: `F` and
`X_k` are both seq_cst; `F` precedes `X_k` in the single total order S (otherwise `L`, sequenced
after `F`, would read the value `X_k` wrote — 0 — by the fence/seq_cst rule, and the argument in
4.2b applies instead); with `F` before `X_k` in S, the fence-to-seq_cst rule gives `V_k` the
effects of `D(o)`.

*Either `L` reads the bit clear, or a later take exists.* (a) If `L` reads the bit clear, `R`
sets it (seq_cst RMW). The bit stays set until some `X_k` after `R` clears it; `X_k` after `R`
after `L` → Claim 1's first sentence applies to `X_k`. (b) If `L` reads the bit set, the last
modification before `L` in the word's modification order is a set by some producer (an `R'`), not
a take, because a take writes 0 in that bit. So the bit is set at `L`, stays set until a take
`X_k` occurs, and that `X_k` is after `R'` after… — is it after `L`? `L` read the value `R'`
produced, so `L` did not observe `X_k`'s write; a take that had committed before `L` would have
been observed by `L` (TSO: `L` executes after `F`, a full barrier, so it reads from the coherence
point, not from a stale buffer). Hence `X_k` is after `L`, and by the first sentence `V_k` sees
`D(o)`. That is exactly the read-first guard's contract: *a bit found set will be taken later, and
that take is ordered after our Done*.

*Liveness of the take.* A set bit is taken because the consumer scans every word every pass
(`collect_retire_work`, hot path), every word in the sweep before parking, and re-checks
`ready().any()` behind the seq_cst park fence (`arm_blocked` → `any_io_inbound`); and if it is
parked, Claim 3 delivers the wake. B changes the word being scanned (producer line instead of slot
word) but not the fact that *every* word is scanned on *every* pass and before *every* park.

### 4.3 Claim 2 — resolution: a taken hint reaches the connection's drain

Today: `X_k` maps the bit to one slot, `slots_[s]` → `c`, `enqueue_serve(c)`; the serve drains the
Done prefix. With B: `X_k` maps bit `b` to the residue class; for each candidate the consumer checks
`head_done()` after `X_k` (a full barrier), so by Claim 1 it sees `D(o)` if `o` is at the head.

- `o` at the head: `head_done()` is true → serve → `Rob::drain` retires `o` and every Done op behind
  it. Reply order is the ROB order; unchanged.
- `o` not at the head (an older op `o'` of `c` is still in flight on another executor): the hint is
  dropped. `o'`'s completion performs its own `D(o')`, fence, `set()` — Claims 1 and 2 apply to it,
  and that resolution drains through `o'` and `o`. No completion is ever the last one for its
  connection without being the head at the time of its own resolution or being covered by the
  head's later resolution — because the ROB only ever has one head, and the head's completion is
  itself a completion with a set().
- Residue false positive (another connection in the class): its `head_done()` is false unless it
  has work, in which case serving it is correct anyway.
- Stale slot (§4.4): `slots_[s]` null or a new connection: dropped or harmless.

Both resolutions are strictly less work than today's for a connection with nothing Done at head
(today: an empty serve), and identical otherwise.

### 4.4 Claim 3 — Dekker: no sleeping with a set bit (the park/arm protocol)

Consumer: `arm_blocked()` = `parked_ = true` (store), armed channels, **seq_cst fence**, then the
loads of `any_io_inbound()` (every ready word). Producer, on a transition: `R` (seq_cst RMW), then
`wake_if_parked` = load `parked_`.

Order `R` and the consumer's fence in the full-barrier total order:
- `R` before the fence: the consumer's word loads, after the fence, see the bit → it does not park.
- fence before `R`: `parked_ = true` is globally visible before the fence completes, hence before
  `R` completes, hence before the producer's `parked_` load executes → the producer sends the wake.
  The wake is an SQE on the producer's ring, submitted at the end of the producer's pass (or sooner
  if its SQ fills); the consumer's `submit_and_wait(1)` returns on it.

**With A, one `parked_` load per io after all of its RMWs.** For every transition `R_i` of that io in
the flush, `R_i` precedes the `parked_` load in program order and each `R_i` is a full barrier, so
the two-case argument holds for each `R_i` individually with the same `parked_` load; if the load
reads false, the consumer's fence is after the load, hence after every `R_i`, and its word loads
see every bit. One wake per io per flush is therefore exactly as safe as one per transition; it is
also why the wake must be decided *after* the RMWs, never memoised across a flush boundary (a
cached "already woke this io" could straddle a park/unpark cycle of the consumer and lose the
second park).

Sets that found the bit already set owe no wake: the producer that performed the edge owes it, and
Claim 1(b) shows the consumer will take that bit after our load.

### 4.5 Two batches, two notifications

Ops of `c` complete in batch 1 and batch 2: each flush does its own fence and its own `set()`.
If the consumer took the bit between the flushes, flush 2 performs a new edge (and owes a wake);
if not, flush 2 finds the bit set and Claim 1(b) orders the pending take after flush 2's fence, so
its resolution sees both batches' Done stores. Nothing coalesces across a batch boundary.

### 4.6 Stale (io, slot) at flush (A) and stale bits (B)

Between `D(o)` and the flush, `c` may be retired (by another hint or the backstop), closed, its
slot released, and the slot reassigned; or `c` may have migrated to another io. The flush then sets
a bit for a slot that names nothing or a different connection — today's teardown fence "no executor
can set this slot again once the ROB is quiescent" no longer holds, so `ReadyMask::clear` at release
is deleted rather than relied upon. Consequences: a null slot is skipped; a reused slot is resolved
against the new connection's ROB (B) or served once and found empty (A alone); a migrated
connection's Done op was retired before migration (`migration_protocol_idle` requires
`rob_.quiesced()`), so it needed no hint. No reply is lost and no pointer is dereferenced.

### 4.7 The exact fallback and the other lookers are unchanged

`notify_sender_to` (claim + `client_in` post) still runs per op for slot-less connections; the
sweep before park drains channels unmasked and takes every ready word; the backstop pass serves
every active connection every 64 flush passes. None of them is touched by A or B, so the house
rule "a hint is never the only looker" holds with the same lookers as before.

---

## 5. Modes and FLIP

**2s (split)**: producers are the executor threads, consumers the io threads. A: batches are the
32-op `drain_tasks` batches. B: an io thread's sidecar has `nthreads` lines; only executor lines are
ever non-zero; the io scans all of them.

**1s (fused)**: every thread is producer and consumer. The self-path (`target == self_->id()`)
stays a direct call with no atomics — same instructions as today. Cross-thread completions are
recorded per 32- or 128-op batch and flushed once. The fused loop's park (`any_fused_inbound`)
scans every producer word behind the same fence.

**FLIP (2s ratio change)**: `ExInstall` re-masks the task lanes (`remask_task_inbox_quiesced`,
ex_loop.h:1639) with IO dispatch parked and every lane quiesced. The ready words need no rebuild:
they are indexed by producer *thread id*, not by executor ordinal, and their meaning does not depend
on the producer set. A thread converting Ex→Io stops writing its own line in every consumer; its
own sidecar (as a consumer) may hold stale bits from a previous io tenure — they are taken and
resolved on the first pass of the new tenure (benign, §4.6). A thread converting Io→Ex migrates
every connection away (`release_wb_slot` per connection) and starts writing lines as a producer.
Client migration io→io: old io's bit stale-and-benign, new io assigns a new slot; the ROB was
quiescent at the owner edge, so no completion straddles the move.

No knobs. The batch size is the existing loop geometry; the residue width is the word.

---

## 6. Expected effect per write (owner measures)

| quantity | today | after A | after A+B |
|---|---|---|---|
| `mfence` per completed op (ex) | 1 | 1/batch (1/32 or 1/128) | same |
| `ReadyMask::set` loads per op (ex) | 1 (+RMW on edge) | 1 per client run per batch | same, on a private line |
| shared-line RMW writers per io line | E (14-34) | E | 1 |
| ready-line transfers per io pass | ~E + E(E−1) worst | fewer edges | 2 per active producer |
| io take loads per pass | 16 | 16 | `nthreads` (32-64), quiet lines L1 hits |
| io resolution per hint | slot → client | same | 1-2 ROB-head probes |
| first-reply signalling delay | 0 | ≤ 1 batch | same |

Instructions per write on the executor: roughly −3 at p32 (fewer wake decisions, one fence), not the
headline; cycles: −31×(35 + drain) per 32-op batch. Cross-CCX fills per write attributable to the
ready lines: from O(E) per pass down to 2 per active producer per pass.

---

## 7. Risks

1. **p1 latency (both modes)**: the io take loop grows from 16 to `nthreads` loads per pass; at p1
   the pass is short. Expected ≤ 50-100 ns per pass at 32-64 threads (S-state hits), invisible
   against the ring wait, but it is the one place B adds per-pass work on the io. Mitigation if it
   shows: bound the scan to `placement().total_threads()` (already the case) — an ordinal-indexed
   variant would be the next step and needs the ExInstall rebuild the tid variant avoids.
2. **Batch-end signalling**: first replies of a batch wait for the batch. At p32 this is inside the
   io's own rotation; at p1 batches are ~1 op. The 128-op fused pipeline batch is the worst case
   (≈30-60 µs); if p99 moves there, flushing every 32 recorded entries is a one-constant change.
3. **2s flip**: nothing to rebuild, but stale bits from a previous io tenure cost one extra
   resolution pass on the first pass after conversion; `io_inbound_quiesced()` converges the same
   way it does today (every pass takes every word).
4. **Residue false positives** grow with connections per io: at 1024 slots a bit names 16 slots;
   each probe is two L1 loads plus, for an in-flight candidate, one Op-line load. At the benchmark
   geometry (28-70 slots per io) classes have 1-2 members.
5. **Wake count**: unchanged rule (owed by the edge performer, decided after all RMWs); a parked io
   receives at most one wake per producer flush rather than one per client per batch.

---

## 8. Verification in this lane

- `make` release build pinned to cores 40-47, zero new warnings.
- `tests/notify_unit.cc` (server-less, `make unit`): `ReadyWords` semantics (edge-once-per-take,
  load-first take, residue mapping, any/producers), a two-thread producer/consumer stress with a
  simulated park/arm protocol asserting every completion is resolved and no sleep happens with a set
  bit, and the A-style batch flush (fence once, edges per io, wake decided after all RMWs).
- Not run here: boot, memtier, perf, gate. The owner runs pure SET, 1:9, 1:1, p1 latency in both
  modes and the flip battery.

## 9. Files

- `src/core/signal.h` — `ReadyWords` replaces `ReadyMask` (B).
- `src/core/thread.h` — `ReadyWords ready_` (same 128 bytes, same offset), `init` allocates the
  sidecar, `release_wb_slot` no longer clears a bit, `wb_slot_count()`, `any_*`/`io_inbound_
  quiesced` scan producer words (B).
- `src/core/ex_loop.h` — notify batch record + flush, `notify_sender` records while a batch is open,
  RAII scope in both batch entry points (A); producer-indexed `set` (B).
- `src/core/io_loop.h` — `collect_retire_work` takes every producer word and resolves residue
  classes by ROB head (B).
- `src/net/rob.h` — `Rob::head_done()` (B; no layout change).
- `src/cmd/blocking.inc` — `blocking_notify_sender` passes the producer id (B).
- `tests/notify_unit.cc`, `Makefile` `unit` target.
