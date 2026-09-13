# AUDIT-NETWB — networking / write-back (reply) / send path

Night audit lane `t-night-netwb`, base 775aeea48 (mainline). Scope: `src/net/rob.h` (send side),
`src/net/wb.h`, `src/net/conn.h`, `src/net/uring.h`, `src/net/epoll.h`, `src/net/tls.h`, the
WB/flush/send/submit/notify regions of `src/core/io_loop.h`, and the notify/wake paths of
`src/core/thread.h` / `src/core/signal.h`. Read in full: rob.h, wb.h, conn.h, uring.h, epoll.h,
tls.h, op.h. Read in region: io_loop.h run_loop 530-800, flush_ready 6391-6660,
collect_retire_work 5291-5330, wb_* 5983-6390, on_*_send_cqe 2160-2195, close_client/reap_dead
6864-6960, arm_recv/on_recv 1954-1980/3268-3310, epoll_pass 2087-2160; thread.h 380-530,
700-780, 840-880, 1010-1060; signal.h ReadyMask/NotifyMask/Channel 262-430; ex_loop.h
notify_sender 2815-2860 and its three call sites; the retire hooks (scatter_engine.inc
xshard_retire 3702, xshard_commands.inc assemble_mget 1535-1590, notify.inc 522, blocking.inc
1323, multi.inc 1552); climon.cc suppressed serve 536.

Anchors from the coordinator: kernel TCP ~17% of cycles at 64c p32; `notify_sender` 3.6% instr /
5.2% cycles at 1:1 mixed; `tcp_rcv_space_adjust` / `_copy_to_iter` / `skb_release_data` dominate
cross-CCX fills even at pure GET; an "eager" reply design once lost 11-22% by buying send width
twice.

Nothing was booted, benched or profiled in this lane; every claim below is from reading the code.
Expected effects are reasoned, not measured — the coordinator measures.

---

## 1. Per-reply instruction path (Q1) — ranked by expected impact

The trace (Split 2s, io_uring, plaintext, small GET at depth):

- **executor**: handler formats into `Op::Sink` (direct region for the batch head, `op.reply`
  otherwise, op.h:216-239) → `op.state.store(Done, release)` (ex_loop.h:2599) →
  `notify_sender(c)` (ex_loop.h:2815): acquire-load `ifid_thread_` [Client line 1, conn.h:778],
  `srv_->thread(target)`, relaxed-load `wb_slot_` [Client tail line, conn.h:796],
  **`std::atomic_thread_fence(seq_cst)` = one `mfence` per completed op** (ex_loop.h:2831),
  `ReadyMask::set` = relaxed load + `fetch_or` only on the empty→set edge (signal.h:266-271)
  [io ThreadCtx `ready_` line], `wake_if_parked` = load `ring_` + acquire-load `parked_`
  (thread.h:1018) [io ThreadCtx line].
- **io**: `collect_retire_work` (io_loop.h:5291) `ready().take(w)` = load-first exchange per
  non-zero word per pass (signal.h:277) → `enqueue_serve` → `flush_ready` phase 2
  (io_loop.h:6540-6590) → `WbEngine::serve<kEp>` → `serve_impl<false,false,kEp,true>`
  (wb.h:728) → `Rob::drain` (rob.h:362). Per op: acquire-load `state`, the sink lambda,
  `oversized()` (2 loads + 2 branches, rob.h:370 / op.h:112), `state.store(Free)` (rob.h:371);
  one `flush_` release store per batch (rob.h:377). Sink per op (wb.h:744-783): `zc_ptr` test,
  `has_pending_segments()` load+branch (769), `direct_len` → `commit_raw` (776),
  `reply.empty()` → `SmallBuf::append` = one `memcpy` of the spilled reply (781). Per batch:
  `flush_deferred_oob` (one predicted branch, 710-713), `nothing_to_write()` (3 loads),
  `pump()` (348): send_inflight / legacy / segments / swap decisions, `submit_legacy` (861):
  `ring_->sqe()`, `io_uring_prep_send`, tag, three Client stores, one stat. One
  `io_uring_submit_and_get_events` per rotation (io_loop.h:718/731, uring.h:184).

**Verdict.** The io send side is at its instruction floor for this design: ~25-30 instructions
per op in drain+stage and ~60 per send, one syscall per rotation, one send per connection per
rotation. The single per-reply copy (`op.reply` → fill buffer, wb.h:781) is structural — it is
what lets the ROB slot recycle at retire (rob.h:36-38); removing it means pinning Op slots until
the send CQE (architectural, §6-H). What is left is coherence and the kernel, which is where the
findings below sit.

### 1.1 [HIGH in atomic / cross-shard regimes; small in pure GET/SET] Executor touches three (four) Client lines per completion, two of which io dirties per op

Per completed op the executor reads `chunks_` (rob.h:526, Client offset 128), `ifid_thread_`
(conn.h:778, offset 64 = line 1) and `wb_slot_` (conn.h:796, offset 1924 = tail line); for
every cross-shard plain op it also reads `id_` (ex_loop.h:2571 `t.client->id()`; conn.h:777,
offset 56 = **line 0**, which io rewrites every pass: `rlen_`, `rpos_`, `wsent_`,
`send_inflight_`, `serve_pending_`, `in_active_`). The tail line is additionally dirtied per op by
io through `atomic_groups_io_` (conn.h:797; `++` at io_loop.h:4861 / multi.inc:1510 /
blocking.inc:1254, `--` at scatter_engine.inc:3728 / multi.inc:1916) for every atomic multi-key
command, and through `obuf_bytes_` (conn.h:802) on every reply append while a client output limit
is armed (conn.h:377, 381, 421-433). Each io write invalidates every executor's shared copy, so the
next `wb_slot_` load on every executor is a cross-CCX miss — exactly the "executors never read
Client atomic bookkeeping" hazard the conn.h:628-630 note fixed on the READ side, still open on
the WRITE side of the same line.

**Implemented (§5 change 3):** swap `id_` ↔ `obuf_bytes_` (line 0 ↔ tail) and `ifid_thread_` ↔
`atomic_groups_io_` (line 1 ↔ tail). Same sizes, same alignments, every locked offset preserved
(sizeof 1984, `connection_flags_` @55, `tls_slot_` @1980). Executors now read `ifid_thread_`,
`wb_slot_` and `id_` from ONE line that nothing writes per op in any regime (it changes at
accept/close/migration, WATCH/MULTI/AUTH); io's per-op writes land on io-private lines (line 0
already io-hot; line 1 read only by the parser). New `static_assert`s pin the invariant.
Expected: one fewer L2-hit load per completion in pure GET/SET (small), and one fewer cross-CCX
miss per completion in atomic multi-key and cross-shard regimes (the "chase" share of the
profile).

### 1.2 [executor, per completion] The `mfence` in notify_sender

`std::atomic_thread_fence(seq_cst)` (ex_loop.h:2831) exists because `ReadyMask::set` is
load-first (signal.h:266-271) and TSO would let that relaxed load run ahead of the Done store. At
p1 the load-first check can never help (io takes the bit every op), so p1 pays mfence + load +
lock-or where a lock-or alone would do; at depth it saves the contended RMW for ops 2..N of a
burst. Removing N-1 fences per executor batch means notifying once per distinct client per batch
— that changes WHEN io learns of completions and is architectural (§6-A). NOT touched:
ex_loop.h is ex-sched territory and the change is not implementation-level.

### 1.3 [both roles, per pass; ~100 cycles/op at p1 low-conn] `NotifyMask::take` is an unconditional locked exchange

`NotifyMask::take` (signal.h:311) is `exchange(0, acquire)` with no load-first guard, unlike
`ReadyMask::take` (signal.h:277). io calls it 4× per pass (`drain_clients` thread.h:719 × 2
words, `drain_client_transfers` thread.h:753 × 2), ex 4× per pass (`task_notify_` thread.h:562 /
628 / 670, `release_notify_` thread.h:736). A locked RMW on a zero word still takes the line
exclusive: ~20 cycles each, ~80-100 per pass on both roles. At p1 / low connection count a pass IS
one op, so this is ~100 cycles per op on both sides. The correctness argument is the one already
written for `ReadyMask::take` (signal.h:272-276): a bit set between the relaxed load and the
skipped exchange is not lost — it stays set for the next pass — and the park path re-checks masks
AND queue depths after `arm_blocked`'s seq_cst fence (thread.h:855-866, 1028-1045), so no wake can
be missed. **Implemented (§5 change 4).**

### 1.4 [io, per op, tiny] `has_pending_segments()` reloaded per op

wb.h:769 reloads `segments_.size_` per op because opaque calls (the retire hook, `malloc` inside
`SmallBuf::grow`) sit inside the loop. A hoisted bool must be re-read after every retire hook
(`assemble_mget` stages segments WITHOUT leaving `zc_ptr` set — xshard_commands.inc:1562-1584 then
scatter_engine.inc:3731). ≤3 instructions/op (~0.3%), below the instrument's floor. **Left
alone.**

### 1.5 [io, per op, tiny] `oversized()` and the redundant `Free` store

`Rob::drain` pays 2 loads + 2 branches per op for `oversized()` (rob.h:370, op.h:112) and a
`state.store(Free)` (rob.h:371) that `Op::reset` repeats at acquire (op.h:72). Both are ≤1-2
instructions on L1-hot lines and both encode documented slot-state semantics. **Left alone.**

### 1.6 [io, copies] One borrowed reply turns the rest of the batch into mallocs

Once the segment queue is non-empty, every later reply in the same drain goes through
`append_buf_segment` = `malloc` + `memcpy` + 24-byte metadata (wb.h:770, conn.h:151-158) until
the queue drains. At p32 a single borrowed GET at the head of a batch costs the other 31 replies
31 mallocs/frees. Coalescing consecutive Buf segments into one growable tail block is only safe
for a tail segment not yet named by an in-flight iovec (`build_iov` takes ≤16 from the head at
pump time, conn.h:167-184) — it needs an "in-flight frontier" index in `SegmentQueue`.
Implementation-level, but not provable tonight without a test the gate can run; recorded as a
follow-up (§6-C carries the design note).

### 1.7 [kernel, copies] "zero-copy" stops at the user/kernel boundary

Borrowed values are handed to the kernel by pointer through an iovec (wb.h:759), and
`sendmsg` still copies them into skbs (`_copy_from_iter`). For ≥16KB values
`IORING_OP_SEND_ZC` / `sendmsg_zc` with the notification CQE as the borrow-release point would
remove that copy. Architectural (§6-D).

### 1.8 [single-owner, atomics] Nothing unnecessary found

All io-side Client flags are plain bools (conn.h:749-774); the Rob counters are atomics that
degrade to plain loads/stores on their owning thread (rob.h:19-24); the only cross-thread RMWs per
op are the ReadyMask edge (signal.h:270) and the Done store. No atomic in the send path can be
demoted, and no cross-thread write is issued that the reader does not need.

---

## 2. Syscall / io_uring shape (Q2)

- **Submissions per rotation: one.** `io_uring_submit_and_get_events` at io_loop.h:718/731
  (uring.h:184) carries every recv re-arm (one SQE per completed recv, `arm_recv`
  io_loop.h:1954-1971) and every send (one SQE per served connection, wb.h:367/866). Nothing is
  submitted per op. `sqe()` flushes early only when the 4096-entry SQ is full (uring.h:143-155).
- **Recv:** plain `IORING_OP_RECV` into the append-only read buffer, request = the whole free
  tail (≥2KB, initially 16KB, io_loop.h:1962-1967). Not multishot, no provided-buffer ring — those
  conflict with argv Slices pointing into a stable rbuf (conn.h:18-33); see §6-G.
- **Send:** `IORING_OP_SEND` for the contiguous buffer, `IORING_OP_SENDMSG` (≤16 iovecs,
  ≤2GB) once borrows are queued (wb.h:369). One send in flight per socket (wb.h:351). No
  `MSG_MORE` / `TCP_CORK`: a rotation's replies already leave in ONE send, so corking would add a
  syscall and remove nothing. `TCP_NODELAY` is set at adopt (io_loop.h:3215).
- **Per-SQE kernel cost we DO pay:** `fget`/`fput` on every recv and send SQE (unregistered
  fds). Registered files (`IORING_FILE_INDEX_ALLOC` via multishot-accept-direct + `close_direct`)
  remove two atomics on `struct file` per SQE but touch accept, close, kTLS `setsockopt` (needs a
  real fd) and FLIP migration between rings — §6-E.
- **`tcp_rcv_space_adjust` / `_copy_to_iter` / `skb_release_data` dominating cross-CCX fills**
  is the RX softirq running on a different CCX than the io thread that `recv`s: the `tcp_sock`
  and skb lines are dirtied by softirq, then pulled across by the syscall. That is steering, not
  code — aRFS/RFS (`SO_INCOMING_CPU`), or io_uring NAPI busy-poll (`io_uring_register_napi`,
  kernel ≥6.7) which runs the RX poll on the io thread itself — §6-F. It needs the NIC rig, a
  knob, and an owner ruling on idle CPU.
- **epoll engine** (wb.h:402-439): `sendmsg`/`send` with `MSG_DONTWAIT` per connection per
  serve, loop to EAGAIN, EPOLLOUT edge armed once (epoll.h:25-36). `epoll_request_close`
  (io_loop.h:5161-5165) is an O(n) dedup scan per request → O(n²) in a mass-RST pass; cold,
  noted, not changed.
- **Verdict:** no syscall per op remains that could be per batch.

---

## 3. Stability (Q3)

### 3.1 DEFECT — FIXED (§5 change 1): CLIENT REPLY OFF/SKIP + cross-shard MGET leaks a partial array to the wire

`serve_suppressing_impl` (wb.h:289-322) runs the retire hook FIRST — `xshard_retire`
(scatter_engine.inc:3702-3733) → `assemble_mget` (xshard_commands.inc:1562-1584) seals the fill
buffer and appends the `*N` / `$len` Buf segments, the BORROW segment and the CRLF segment to
the connection's queue — and only THEN tests `op.reply_skip()` (wb.h:299), dropping just the op's
tail (`op.direct` / `op.reply`). The already-staged header + bulks are pumped (wb.h:331-338), so
under CLIENT REPLY OFF the peer receives an incomplete array; when replies resume, the next `+OK`
is consumed as the array's missing element and every later reply on that connection is shifted
by one. Reachable whenever an MGET spans ≥2 shards and any value is ≥ min(zc-min, 1024)
(scatter_engine.inc:2665, ValueSlot::kInline = 1024) — i.e. with default knobs.

Fix: record the segment-queue frontier before the hook and truncate back to it afterwards for a
skipped op, keeping the one seal segment of OLDER bytes (detectable as the fill buffer going
staged → empty across the hook), releasing BORROWs and freeing Buf blocks exactly once
(`SegmentQueue::truncate`, `Client::truncate_segments`). The truncated segments were appended
after the last pump and can therefore not be named by an in-flight sendmsg.

Regression test: `tests/replyoff_xshard.py HOST PORT` — asserts the exact wire bytes
(`+OK\r\n+PONG\r\n`, nothing else) after `CLIENT REPLY OFF; MGET k0..k31; CLIENT REPLY ON;
PING`, the same for the SKIP form, and that the mechanism FIRED: the suppressed MGET moves
`zc_releases` without moving `zc_sends`, while the REPLY ON control moves `zc_sends`.

### 3.2 DEFECT — FIXED (§5 change 2): 32-bit `wsent_` wraps at 4GB of unread replies and corrupts the stream

`Client::wsent_` is `uint32_t` (conn.h:748) and `commit_write(uint32_t)` (conn.h:386)
accumulates it across resubmits, while `SmallBuf::len_` is `size_t`. A client that pipelines
large GETs and never reads grows one fill/send buffer without bound: the ROB window bounds
in-flight ops, not staged bytes, and `client-output-buffer-limit normal` defaults to 0 =
unlimited (config.h:41; redis parity). Past 4GB in one buffer `wsent_` wraps: `write_drained()`
(conn.h:393) is never true and `submit_legacy` (wb.h:868) resends from the wrapped offset — the
connection streams garbage forever instead of merely holding memory. Fix, layout-neutral:
`wsent_` → `uint64_t`, `fill_` → `uint8_t` (a 0/1 index), same eight bytes at offsets 32-43;
`commit_write(size_t)`. Bit-identical below 4GB. Not gate-testable (needs >4GB staged on one
connection); manual repro: `SET k <1MB>`, pipeline 5000 `GET k` without reading, sleep, then read
and compare the stream against 5000 × value.

### 3.3 OK — the two borrow-release paths (the S8 class)

A BORROW is released either by `consume_segments` at the send completion (wb.h:558, epoll
wb.h:424) or by `teardown` (wb.h:640-644), which SKIPS while a send is in flight and is re-run
from `on_dead_send_complete` (wb.h:655-668) / `reap_dead` (io_loop.h:6944) once the CQE lands;
`release_all` pops only what `consume` did not (conn.h:203-211, `pop_front` removes consumed
entries). `safe_to_release` (conn.h:680-686) refuses `::close` while `send_inflight_`, and
`reap_dead` holds the corpse while `send_inflight()` / `recv_armed()` (io_loop.h:6936-6941).
The TLS no-borrow arms release at staging and never queue a BORROW (wb.h:756, 821). No
double-release or leak path found.

### 3.4 OK — partial send / EAGAIN

uring resumes from `(head, offset_)` (`SegmentQueue::consume`, conn.h:187-201) or from `wsent_`;
`-EAGAIN` / `-EINTR` resubmit (wb.h:544); epoll stops staged and waits for the EPOLLOUT edge
(wb.h:397-401, io_loop.h:2136-2137). A short write on the legacy buffer: `commit_write` then
`write_drained()` false → resubmit (wb.h:561-579).

### 3.5 OK — connection close mid-batch

`close_client` is idempotent (io_loop.h:6870), `shutdown(SHUT_RDWR)` breaks in-flight kernel
ops (io_loop.h:6895), and a retire hook cannot reach `teardown` mid-drain because
`safe_to_release` needs ROB quiescence and `flush_` only advances after the drain (rob.h:377). A
closing connection keeps being served so its ROB drains (io_loop.h:6549-6551). One wasted SQE:
`on_send_complete` on a closing conn re-pumps (wb.h:583) → EPIPE → a second, idempotent
`close_client`. Harmless, and REQUIRED for the CLIENT KILL-self path, which closes through
`mark_closing` without `shutdown` so the reply can still leave.

### 3.6 OK — output-buffer limit

`serve_impl<TrackOutput>` (wb.h:788-795) returns before pump once `client_obuf_check`
(io_loop.h:6757-6797) has closed the client; staged Buf blocks and BORROWs are returned by
`teardown` at release. The unified WB batch tombstones its slot (io_loop.h:6790-6795).

### 3.7 OK — shutdown accounting

main.cc:565-590 walks `clients()` and counts connections with `!nothing_to_write()` as
`unsent_bytes_pending` — a CONNECTION count, not bytes; the name misleads (main.cc is outside
this lane, noted only). Dead clients left `clients()` at close (io_loop.h:6913), so the count is
live connections only.

### 3.8 OK — marker `zc_ptr`s never reach the borrow branch

Every retire hook clears its marker before the staging branch: `detach_scatter_state`
(scatter_engine.inc:3731), `detach_blocking_state` (blocking.inc:1352), `notify_take_batch`
(notify.inc:546-547), MULTI via `multi_retire`. `wb_prefetch` guards `zc_shard < 0`
(io_loop.h:6058). A hook that forgot would hand `append_borrow_segment(ptr, 0, -1)` to the queue
and later `worker_of_shard(-1)`; a debug assert in the borrow branch would catch it but sits on
the hot path — not added.

### 3.9 NOTE — `append_buf(a, an, b, bn)` truncates to uint32

conn.h:151-158 casts `an + bn` to `uint32_t`; only reachable with a >4GB single-op reply (an
MGET of millions of keys). Not fixed.

---

## 4. Cleanliness (Q4) — implemented as §5 change 5 unless marked

- `Client::has_atomic_groups_io()` (conn.h:647) duplicates `has_atomic_group_io()` (631); zero
  callers → deleted.
- `SegmentQueue::pending_bytes()` (conn.h:251-255) recomputes `byte_size()` (131-138); one
  caller → replaced, deleted.
- `kMaxSendBytes` defined three times (conn.h:789, wb.h:864, wb.h:883) → one namespace constant.
- `WbEngine::serve` doc (wb.h:153-166): the "WHO MAY CALL … In Wb mode … In EX MODE, ANYONE"
  paragraph is pre-2s history contradicted by the next sentence, and the summary sentence is
  duplicated → rewritten.
- `serve_suppressing_impl` released a skipped borrow via `release_fn_` directly (wb.h:300-301),
  bypassing `release()` so `zc_releases` undercounted → folded into change 1 (same lines).
- `Ring::msg_to` comment "tells a WB thread" (uring.h:310-311) → stale wording fixed.
- NOT changed: `Ring::note_pending()` no-op (uring.h:281-283) — the owner kept it deliberately
  as the SQE-producer marker; the vestigial `{ }` scope in `on_send_complete` (wb.h:540-581, an
  ex-lock-guard block) — whitespace-only churn, not worth the diff.

---

## 5. Changes implemented (one commit each; wire-identical unless it is the defect being fixed)

1. **wb.h / conn.h — suppressed serve discards hook-staged segments** (+ `tests/replyoff_xshard.py`).
   Fixes 3.1. Cold path only (`serve_suppressing_impl` is reached only for CLIENT REPLY
   OFF/SKIP connections); hot `serve_impl` untouched.
2. **conn.h — 64-bit `wsent_`, layout-neutral.** Fixes 3.2. `fill_` narrows to `uint8_t`;
   offsets 32-47 keep their meaning; bit-identical below 4GB.
3. **conn.h — executor-facing line consolidation.** 1.1. `id_` and `ifid_thread_` move onto
   the tail line beside `wb_slot_`; `obuf_bytes_` moves to line 0 and `atomic_groups_io_` to line
   1 (both io-private). Locked offsets unchanged; new static_asserts encode "all executor-read
   scalars share one line" and "io per-op counters live on io-private lines".
4. **signal.h — `NotifyMask::take` load-first.** 1.3. Mirrors `ReadyMask::take`; removes up to
   eight locked RMWs per (io + ex) pass.
5. **cleanliness** — §4.

Risk table: (1) new code on a cold path with a directed test; (2) type widening only; (3) pure
relayout under static_asserts; (4) protocol-preserving by the same argument already shipped for
ReadyMask; (5) deletions of unused code and comments.

Commit ledger (branch `t-night-netwb`, base 775aeea48; each built pinned with zero new
warnings — the one warning in every build is the pre-existing unused `store` in ex_loop.h:1066):

| # | commit | change |
|---|--------|--------|
| 0 | 737ac9b0b | this audit |
| 1 | adbfb0366 | wb.h/conn.h: suppressed serve discards hook-staged segments + tests/replyoff_xshard.py |
| 2 | c17d31e7f | conn.h: 64-bit `wsent_` (layout-neutral) |
| 3 | 4c933844f | conn.h: executor-facing line consolidation + coherence static_asserts |
| 4 | f91e3504e | signal.h: `NotifyMask::take` load-first |
| 5 | 799d45cdc | cleanliness (conn.h, wb.h, uring.h) |

Files touched: AUDIT-NETWB.md, src/net/conn.h, src/net/wb.h, src/net/uring.h,
src/core/signal.h (NotifyMask only — the notify path thread.h consumes), tests/replyoff_xshard.py.
tests/gate.sh was deliberately NOT edited: its EXPECT_QUICK ledger is shared across lanes; the
coordinator wires the new battery (one row per atomic mode on a zero-copy boot) at merge time.

---

## 6. ARCHITECTURAL / ALGORITHMIC IDEAS (not implemented)

Each: the idea, why it should pay, and one line on how it respects the owner's philosophy
(single-owner writes / no shared-writer index; reads never obstructed by writes — immutable
replacement + QSBR, no reader retries or seqlocks; no in-place overwrite while read-local is
armed; numeric knobs 0=off / -1=auto with self-derived thresholds; main commands zero-regression;
hardcode-or-delete; one file per feature).

**A. Per-batch completion notification (executor side).** Store Done per op, then ONE
`seq_cst` fence and one `ReadyMask::set` per distinct client per executor batch instead of an
`mfence` per op (ex_loop.h:2831). Saves N-1 fences per batch (~35 cycles each) and, at depth,
fewer partial drains on io (io is woken once per burst, not once per op — fewer, wider sends,
which is the direction the "eager" lesson points). Cost: the first replies of a batch are
signalled at batch end (bounded by the executor batch size). Philosophy: unchanged single-owner
edges (the ReadyMask bit is still the only cross-thread write and still idempotent); no reader is
obstructed; no knob — either it wins in the three regimes or it is deleted.

**B. Per-producer ready words instead of one shared ReadyMask.** Today all executors `fetch_or`
into the SAME two cache lines of an io thread's `ready_` (signal.h:262-296: 1024 slots over
16 words, ≤512 conns land on one 64-byte line), so at 16 executors the line bounces between
producers per burst. A word per (producer, consumer) pair — 16 × 8 bytes, the same 128 bytes —
makes every word single-writer; the bit meaning becomes a residue class of slots (slot mod 64)
that io resolves by checking those clients' ROB heads, with `client_in` as the existing exact
fallback. Philosophy: single-owner writes by construction (no shared-writer index); io's
`exchange(0)` remains the only consumer write; a hint never the only looker (the sweep still
scans); no knob.

**C. Coalesce consecutive Buf segments behind an in-flight frontier** (follow-up to 1.6;
implementation-level once a gate test exists). Track the segment index handed to the last
`sendmsg`; a Buf tail beyond it may be `realloc`-appended instead of allocating a new block per
reply. Philosophy: the queue stays single-owner (io only); bytes named by the kernel are never
moved (frontier rule); no knob.

**D. `IORING_OP_SEND_ZC` for borrowed values above a threshold.** The notification CQE (second
completion) becomes the borrow-release point; below the threshold the copying send stays.
Removes `_copy_from_iter` for ≥16KB values. Philosophy: the release still happens on the owning io
thread through the existing `release_fn_` channel (single-owner); readers of the store are
untouched (the borrow already holds the value immutable under QSBR); the threshold rides the
existing `--zc-min` numeric knob (0=off) rather than adding one.

**E. Registered files (direct descriptors).** `multishot accept direct` + `close_direct` drop
`fget`/`fput` (two atomics on `struct file`) per recv and per send SQE. Constraints: kTLS
`setsockopt` needs a real fd (TLS accept keeps the fd path), FLIP migration moves a connection
between rings (a direct slot is per ring → dup/re-register on migrate). Philosophy: no shared
state added; per-ring slot tables are single-owner; boot-only numeric knob (0=off) or
hardcode-or-delete after the three-regime A/B.

**F. RX steering to the io thread's CCX.** aRFS/RFS via `SO_INCOMING_CPU`, or io_uring NAPI
busy-poll (`io_uring_register_napi`, kernel ≥6.7) so the io thread runs the NAPI poll for its
sockets and the `tcp_sock`/skb lines stay local — targets the `tcp_rcv_space_adjust` /
`_copy_to_iter` / `skb_release_data` cross-CCX fills directly. Philosophy: zero code on the per-op
path; numeric knob (busy-poll µs, 0=off, -1=auto from the measured park interval); needs the NIC
rig and an owner ruling on idle CPU because busy-poll changes the park behaviour.

**G. Multishot recv with provided-buffer rings.** One recv SQE per connection lifetime and
kernel-chosen buffers. Conflicts with the append-only read buffer that argv Slices point into
(conn.h:18-33): every parsed op would need its bytes pinned in a provided buffer until retire,
i.e. a different read-buffer lifetime model. Philosophy: only acceptable if Slices remain
stable while ops are in flight (no in-place buffer reuse under a reader) — otherwise it is a
reader-obstruction design and is out.

**H. Pin Op slots until the send CQE (iovec of replies, no memcpy at retire).** Removes the one
per-reply copy (wb.h:781) at the cost of ROB slots staying occupied through the send and up to 64
iovecs per send. The design already rejected this direction once (rob.h:30-38: contiguous chunks
exist precisely so retire can recycle in place). Philosophy: single-owner unchanged, but it
trades the ROB's bounded footprint for send width — must be measured against the "eager" lesson
before being considered again.

---

## 7. Deliberately left alone, and why

- `parse_and_dispatch`, the read-local enqueue/prepare/capture/demotion regions, the command
  registry, ex-sched, QSBR, flatstore: other lanes.
- `Rob::drain` micro-details (1.5), the per-op `has_pending_segments()` reload (1.4): below the
  instrument's floor; the Free-store carries documented semantics.
- `Ring::note_pending()`: owner-kept marker.
- `on_send_complete`'s extra scope block: cosmetic.
- `SmallBuf::append` small-size fast path (avoids the libc `memcpy` PLT call for the 5-20 byte
  replies that dominate SET/INCR/DEL): `src/base/slice.h` is shared with executor-side reply
  formatting and outside this lane's files; worth a separate, measured lane.
- The `epoll_request_close` O(n²) dedup scan: cold, epoll-only.
