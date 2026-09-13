# P0: the armed-fused crash — attribution, real crash site, and the instrument

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

## 1. The reported evidence was two different failures, not one

The gate-hygiene lane's third acceptance gate reported a segfault in the reply path
(`tomo::reply_bulk<tomo::Op::Sink>`, `src/net/resp.h:258`) taking out the armed-fused
differential matrix. That reading does not survive the binary.

`journalctl -k` at 02:19:17 on 2026-09-07:

```
tomokv[3081117]: segfault at c000108 ip 00005912d5cf68b5 sp 00007a24d07fd7f0 error 4
tomokv[3081119]: segfault at c000108 ip 00005912d5cf68b5 sp 00007a24cf3fd800 error 4
 in tomokv[1968b5,5912d5b71000+36f000] likely on CPU 171 (core 43, socket 0)
 likely on CPU 169 (core 41, socket 0)
Code: ... 48 8b 3c ca 89 c8 48 85 ff 0f 84 4b ff ff ff <48> 39 5f 08 0f 85 01 24 e8 ff ...
```

Three independent facts identify the process:

* The kernel's first number is the FILE OFFSET of the faulting instruction, not `ip - vm_start`
  (verified against an unrelated libgtk segfault in the same journal). For these PIE binaries the
  exec segment has `VirtAddr == Offset == 0x011000`, so the ELF vaddr is `0x1968b5`.
* On every `ceb6b02f8` build `0x1968b5` is the SECOND BYTE of `push %r13` in `reply_bulk`'s
  prologue — not an instruction boundary, and a `push` faults as a WRITE (error 6), not error 4.
* The kernel's `Code:` bytes match `/home/user/Projects/wt-ringdiet/build/tomokv` **byte for byte**,
  where `0x1968b5` IS an instruction boundary.

`/var/log/apport.log` closes it:

```
02:19:17,812  pid 3081108  signal 11  /home/user/Projects/wt-ringdiet/build/tomokv
              --port 8431 ... --thread-mode fused --atomic 1 --read-local 1
02:19:50,288  pid 3080931  signal  6  /home/user/Projects/wt-gatehygiene/build/tomokv
              --port 8440 ... --thread-mode fused --read-local 1 --atomic 1
```

Two processes, 33 seconds apart, different lanes (ringdiet's DESIGN names its cores as 40,41 +
42,43/170,171 — exactly the CPUs the kernel named; gate-hygiene's differ target ran on 136-143).

**The gate row was broken by SIGABRT (signal 6), not SIGSEGV.** tomokv installs no SIGSEGV
handler, and no kernel segfault line exists for pid 3080931 — so `std::abort()` is the only
remaining cause.

## 2. The real crash site: the armed-write block cache

`addr2line 0x1968b5` on the ringdiet binary:

```
tomo::KvBlockCache::take(unsigned long)          src/store/kv_block_cache.h:88
  inlined in FlatStore::read_local_cache_take     src/store/flatstore_atomic.inc:1453
  inlined in FlatStore::make_set_int              src/store/flatstore.h:1345
```

The faulting instruction is `cmp %rbx,0x8(%rdi)` where `%rdi = heads[cls]`, just proven non-null
by the `test %rdi,%rdi; je` two instructions earlier. Fault address `0xc000108` ⇒
`heads[cls] == 0xc000100`: a WILD FREE-LIST HEAD. `0xc000100` is the size and shape of a KvObj
header word, not of a heap pointer.

`heads[cls]` is only ever assigned `block->next` (take/release_all) or a block being put. A wild
head therefore means **a block's `next` was overwritten while that block was on the free list** —
i.e. the block was simultaneously on the cache list and live to someone else.

`KvBlockCache::take()` and `put()` both contain `std::abort()` tripwires for exactly this class of
corruption (`block->allocation != allocation`, `!class_nodes[cls]`, `heads[cls] == memory`). The
gate-hygiene SIGABRT is consistent with one of them firing on `ceb6b02f8` where ringdiet's build
instead reached the wild pointer first. **Both deaths are one defect, seen at two different
distances from the corruption.**

## 3. What has been excluded

* **The reply path (`reply_bulk`, Sink buffer state).** Excluded by the instruction bytes. Nothing
  in the evidence ever pointed at it.
* **The LB / flip shard-ownership handoff.** `ReadLocalDeferredQueue::reclaim_entry` passes the OLD
  owner's `sink_`, but `read_local_reclaim_object` IGNORES that argument and re-reads the STORE's
  CURRENT `retire_sink.block_cache` (`flatstore.h:3456-3475`, `flatstore_atomic.inc:1430`), so a
  shard rebound to a new owner would have the old owner put into the new owner's cache. That hole
  is closed at the protocol level: `ExLoopT::flip_quiesced()` (`ex_loop.h:1779`) refuses the
  ExDrain ack while `read_local_impl().deferred` is non-empty, so the old owner's retire ring is
  drained before `rebind_read_local_retire_sink` runs. It remains a latent trap — the callback's
  correctness depends on a distant protocol invariant rather than on the sink it is handed.

## 4. The instrument (debug build only)

`kv_block_cache.h` gains `-DTOMO_RL_CACHE_DEBUG` invariants that state the cache's two laws as
assertions instead of leaving them as comments:

* **owner-private** — an owner tid latched on first use; `take`/`put` from another thread abort.
  `release_all` reports the crossing without aborting, because the graceful-shutdown path
  legitimately drains every owner's queue from the main thread AFTER `pool.join()`
  (`genthread.cc:105`) — the first thing the instrument found, and benign.
* **resident exactly once** — an O(1) residency set; a double `put` or a `take` of a
  non-resident block aborts naming the block.
* **list matches its counter** — every `take`/`put` walks the class list and checks that each
  block carries the class's request size and that the length equals `class_nodes[cls]`. A
  clobbered `next` is caught on the very next cache operation instead of at some later
  dereference.

The shipped binary is unaffected: `build/tomokv`'s `.text` is bit-identical to the
gate-hygiene binary that aborted (md5 `b00af053d0f9c1a626c7fe1fc71f0485`).

## 5. Reproduction in flight

Two loops of the exact failing gate row (`differ_gate.sh` with
`GATE_DIFFER_GEOMETRY=armed-fused`), one against the untouched release binary (cores 32-39,
ports 8200/8201) and one against the instrumented binary (cores 160-167, ports 8202/8203).

## 6. REPRODUCED — on the release binary, in seconds

`tests/rlcache_churn.py` is a directed stressor for this path. The differential matrix could not be
the instrument: it walks broad command surface at modest depth and rarely cycles one block through
put -> grace -> take more than a handful of times. The stressor does the opposite — a 64-key space
rewritten continuously at sizes chosen so each key's displaced block is exactly the class its next
write asks for, with READERS AND WRITERS ON SEPARATE CONNECTIONS.

That last point is what makes it work. The first version mixed GETs and SETs on one connection and
measured **210** local reads across 6M GETs: a connection carrying writes fences its own reads
through the RYOW ring, so the armed lane was never exercised. Split onto separate connections over
one key space, the same stressor produces **5.3 million local reads in 25 seconds** — readers
holding foreign pointers into objects the owners are concurrently retiring, which is the hazard.

```
geometry:  --thread-mode fused --read-local 1 --atomic 1 --shards 16 (8 fused threads)
load:      64 connections, half readers half writers, 64-key space, 40 s
result:    release build/tomokv  ->  SIGABRT after ~7 s
```

**Which abort.** The faulting frame is `FlatStore::read_local_reclaim_object`, and the `call
abort@plt` at `0x5bf88` is reached only from the `je` at `0x5be6d`:

```
5be66:  mov (%r12,%rax,8),%rdx     ; heads[cls]
5be6a:  cmp %rdx,%rbx              ; == memory ?
5be6d:  je  5bf88                  ; -> abort
5be78:  mov %rdx,(%rbx)            ; block->next = heads[cls]
5be7b:  mov %rbp,0x8(%rbx)         ; block->allocation = allocation
```

That is `KvBlockCache::put`'s immediate double-return guard: **the same block returned to the
cache twice in a row.** It is the tripwire the header's own comment describes, and it is the abort
that killed the gate-hygiene differ target.

## 7. ROOT CAUSE — the block cache is NOT owner-private

The debug build names it directly, on the ordinary write path:

```
RLCACHE-VIOLATION owner: take on cache 0x75c8f6447080 from tid 3136850, owner tid 3136854

KvBlockCache::take                         kv_block_cache.h
  FlatStore::read_local_cache_take         flatstore_atomic.inc:1453
    FlatStore::make_set_string             flatstore.h:1301
      store_string / cmd_set<false>        t_string.cc:158 / 550
        ExLoopT<true>::execute<false,false> ex_loop.h:2955
          exec_batch / drain_tasks          ex_loop.h:2758 / 2100
            fused_pass_impl                 ex_loop.h:547
```

A fused thread executing an ordinary SET task reached **another fused thread's** block cache. The
cache is reached as `read_local_store_state_armed().retire_sink.block_cache` — a pointer stored on
the SHARD's store, not on the executing thread. So the identity of the cache a write uses is
whatever the shard was last bound to, and nothing on the write path checks that this is still the
thread doing the writing.

Two threads then splice one unlocked free list: a lost update leaves a block linked twice, `put`
sees itself as the head and aborts (`ceb6b02f8`), or `take` hands out a still-linked block whose
KvObj header then overwrites the list `next` and the following `take` dereferences a wild head
(`0xc000100` — the ringdiet segfault).

The same stale pointer is `retire_sink.defer`, which pushes into `ReadLocalDeferredQueue`'s
single-producer retire ring. The cache is simply where the damage shows first.

## 8. THE MECHANISM, from the event log

With the shard-transfer edge and every sink rebind logged, one reproduction says it outright
(`store 0x7f6c0c882638` is shard 1; tid-index is the thread's own id, tid is the OS thread):

```
line 148: RLBIND transfer  store ...882638 sid 1  src tid-index 1 -> dst tid-index 7
line 161: RLBIND rebind    store ...882638 cache 0x..5247080 -> 0x..4a47080  tid 3155464   (= index 7)
line 173: RLBIND transfer  store ...882638 sid 1  src tid-index 7 -> dst tid-index 1
          ... rebindall for indices 3,7,5,4,0 ... but NOT for index 1 ...
line 188: RLCACHE-VIOLATION take on cache 0x..4a47080 from tid 3155458, owner tid 3155464,
                            reached through store ...882638
```

Shard 1 moved to thread index 1 (`tid 3155458`) at line 173. Its store's sink still named thread
index 7's cache (`0x..4a47080`, bound at line 161). **Thread index 1 then executed `cmd_set` on
shard 1 through thread index 7's block cache**, and no rebind for that store appears in between.

`Server::transfer_shard_quiesced` moves the shard: the `shards()` vectors, the bucket router, and
`shard_owner_`. It does NOT move the read-local retire sink. That was left to the destination's own
later executor pass (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`, ex_loop.h
460/662), and NOTHING orders that pass before the destination's first write to the shard it has
just been given. `rebind_read_local_retire_sink`'s own comment claims it is called "before the new
owner executes store work" — that half of the sentence was never enforced.

**Control.** `--key-lb 0 --client-lb 0` removes shard moves and nothing else:

| arm                                   | runs | deaths |
|---------------------------------------|------|--------|
| armed fused, LB on (default)          |  3   |   1    |
| armed fused, LB off (`--key-lb 0`)    |  5   |   0    |

## 9. THE FIX — the sink moves with ownership

`Server::adopt_read_local_retire_sink(shard, destination)` rebinds the shard's store to the
destination thread's sink INSIDE both quiesced transfer functions, immediately before
`router_.commit_transfer()` — the same critical section that moves the shard. Both callers already
hold every executor at the quiesced safe point (`lb_commit_shard_plan` commits only once every EX
thread has acked ExDrain, and `flip_quiesced()` grants that ack only with an EMPTY retire ring), so
the source has nothing outstanding for this shard and the destination has not started. There is no
window left for the sink to disagree with ownership.

The lazy `rebind_owned_shards` path is kept; it is now always a no-op, and is the cheap standing
check that the eager rebind did its job.

| arm (64 conns, 64-key space, 40 s runs)          | runs | deaths |
|--------------------------------------------------|------|--------|
| `ceb6b02f8` pristine release                      |  2   |   1    |
| with the fix                                      |  4   |   0    |

## 10. Is there a third path? — the audit of every ownership edge

**Shard ownership moves through exactly two functions**, both quiesced, both now carrying the sink:

| edge | callers | carries the retire sink? |
|------|---------|--------------------------|
| `Server::transfer_shard_quiesced` | `lb_commit_shard_plan` (server.h), the LB mover (io_loop.h:2985) | yes, now |
| `Server::transfer_bucket_range_quiesced` | flip / role retirement (a whole executor's shards at once) | yes, now |
| `Server::set_worker_of_shard` | boot only (server.h:368) | n/a |

`shard_owner_[]` is written in exactly those three places and nowhere else.

**A SECOND per-owner pointer on the shard is still rebound lazily, and should move to the edge for
the same reason.** `Shard::notify_pending_` is a `bool*` into the OWNER's `notify_keyless_pending_`:

* the FLIP path rebinds it at a barrier — `FlipStage::ExInstall`, ex_loop.h:1832, while IO dispatch
  is still parked. **Safe.**
* the LB path rebinds it lazily, in `lb_control_pass` on the first pass after the stage ends
  (ex_loop.h:1857). **Same window the retire sink had.**

Severity is far lower and it is not the same defect: the stale pointer is written, not spliced, so
there is no corruption — a `bool` is set on the wrong thread, the old owner's drain walks only its
own shards, and the event would be stranded. It self-heals because the rebind then forces one drain
(`notify_keyless_pending_ = true`). It is a correctness wart of the same shape, not a P0, and it is
left for its own change with its own notify-battery evidence rather than bundled here.

**Client moves do not have this shape.** `IoLoop`'s migration path re-derives or explicitly hands
over every per-owner structure at the edge: the WB slot is released on the source
(`release_wb_slot`) and reassigned by the destination at adopt (`assign_wb_slot`), with
`kWbMigrationInstalling` as the sentinel that keeps early readiness inert in between; the local
catalog, the routing state and the pending pub/sub events are extracted and posted; and
`set_ifid_thread(destination)` is the single ownership edge. Nothing is left pointing at the source.

**Thread retirement** (an executor losing its role in a flip) moves its shards through
`transfer_bucket_range_quiesced`, so it is covered by the same fix.

## 11. Two harness findings the new row produced

**`--lb-cooldown-ms 0` disables shard movement entirely.** Measured at 64 shards, 8 fused threads,
15 s of this battery's load, counting `tomokv_keylb_bucket_moves`:

| boot flag                | shard moves |
|--------------------------|-------------|
| (defaults)               |  6          |
| `--lb-tick-ms 100`       | 50          |
| `--lb-move-cap 4`        | 20          |
| `--lb-imbalance-pct 1`   |  5          |
| `--lb-cooldown-ms 0`     |  **0**      |

Every other "move more" knob moves more; this one stops the balancer dead. Under the program's
knob philosophy `0` reads as "off", and for a COOLDOWN "off" should mean *no* cooldown — move
freely. It means "never". That is how the first version of this gate row silently lost its own
precondition, and it is a live trap for anyone tuning the balancer. Not fixed here (it belongs to
the LB lane), but named.

**`tests/slowlog.py` has a constant that assumes eight recording threads.** `gate.sh` sizes its
thread count from its core set (`NCORES`, then `GATE_RATIO=6:$((NCORES-6))`), and the battery
asserts `SLOWLOG LEN <= 8` after shrinking `slowlog-max-len` to 1 — a per-executor ring bound
written for a 6:2 boot. On a 16-CPU core set that boot is 6:10 and the bound is wrong. Verified
against the PRISTINE `ceb6b02f8` binary, nothing of this lane's involved:

| pristine `ceb6b02f8`, `tests/slowlog.py` | verdict |
|------------------------------------------|---------|
| `--ratio 6:10` (16-CPU core set)          | FAIL — `shrinking max-len trims immediately: got 10` |
| `--ratio 6:2`  (8-CPU core set)           | PASS |

So the full gate must be run on an 8-CPU core set until that constant is derived from the live
thread count. This lane gated on `GATE_CORES=32-39`.
