# AUDIT-PUBLISH — plain-SET publication under read-local (owner side)

Base: f40469ea3. Binary: `build/tomokv`, g++ 13.3.0, `-O2 -g -march=native -DTOMO_JEMALLOC`,
`TOMO_READ_LOCAL_SET_TAX_VARIANT=0` (the only shipped selector; every `settax_stats()` block is
compiled out). Offsets below come from `gdb -batch -ex 'ptype /o'` on that binary; instruction
counts from `objdump -d --no-show-raw-insn`. Nothing here was measured on a running server.

Target: `FlatStore::insert_into_read_local` = 7.2% of cycles on pure SET (owner's profile).

## 1. The path (plain `SET k v`, armed, key already present = steady-state pure SET)

| step | code | what it does on the armed path |
|---|---|---|
| 1 | `store_string` src/cmd/t_string.cc:140-160 | `expire_at_ms == -1` -> `try_overwrite` |
| 2 | `try_overwrite` src/store/flatstore.h:1194-1196 -> `try_overwrite_read_local` :2562-2566 | variant 0: `if constexpr (!kReadLocalSetTaxAtomicRaw) return NotPossible;` **No probe.** Generated: one `cmpb $0,0x209(%rbx)` (Shard+56+465 = `read_local_enabled_`) + `jne` straight to `kvobj_new_string` (store_string+0x160). The "SET probes twice" finding is unarmed-only (`find_without_touch` :2135 then `insert_into` :2464). |
| 3 | `make_set_string` :1237 -> `kvobj_new_string` kvobj.h:879 | allocate + build the immutable object (not audited here) |
| 4 | `map_insert` t_string.cc:132 -> `FlatStore::insert` :1568-1569 | `if (read_local_enabled_) return insert_read_local(h,o)` |
| 5 | `insert_read_local` :2807-2836 | rehash step / `maybe_start_grow` / snapshot & maxmemory gates (all predicted-false), `ReadLocalTableGuard table_move(*this, moves_from_old)` — **inactive** unless `rehashing() && find_in(1,...)`; then `insert_into_read_local(0,h,o,true)` |
| 6 | `insert_into_read_local` :2997-3063 | probe, publish slot word, retire old object, account, expire index |
| 7 | `retire_obj_read_local` :3116-3125 | `kvobj_size(old)`, `obj_bytes_ -=`, `read_local_store_state_required().retire_sink.retire(this, old, bytes, &read_local_reclaim_object)` |
| 8 | `ReadLocalRetireSink::retire` read_local_reclaim.h:33 -> `defer_thunk` -> `ReadLocalDeferredQueue::defer` src/core/read_local.h:174-192 | 3 abort checks, capacity check, 4 field stores into a 40-byte ring entry, `tail_/count_/unsealed_` |
| per pass | `drain_ready` read_local.h:201-222 | `seal_pending` :427-436 (one seq_cst `fetch_add` on the GLOBAL epoch + one stamp store PER ENTRY), `read_local_grace_floor` server.h:522-532 (loads EVERY thread's tick), then per ready entry: indirect `reclaim`, `entry = {}` (5 zero stores) |
| per pass | `read_local_reclaim_object` :3344-3350 -> `destroy_retired_obj` :3492-3503 -> `free_retired_obj_now` :3474 -> `kvobj_free` kvobj.h:1248 | **re-decodes the header** (`kvobj_capacity`) that step 7 already decoded, then `sdallocx` |

## 2. Every load / store / atomic / fence on the per-write path (overwrite case)

Line = 64-byte line of the owning `Shard` (each Shard is its own `make_unique<Shard>()`,
server.h:256; jemalloc class 1536 = 24 lines, so every Shard is 64-aligned and `FlatStore` sits at
Shard+56 (ShardLayoutLock::store_offset). Reader-loaded lines are those a foreign `read_local_probe`
:862-886 / `read_local_validate` :890 touches: FlatStore lines L1 (`atomic_pending_` @8 -> abs 64),
L4 (`tab_/cap_/mask_` @208-239 -> abs 264-295), L8 (`read_local_enabled_` @465 -> abs 521),
sidecar `read_local_extended` (@1316) and `probe_sequence` (@1352), slot words, object bytes.

| # | op | source | asm (base) | line | per-write? | reader-loaded line? |
|---|---|---|---|---|---|---|
| 1 | ld `o->flags`, `o->klen8` | :2999 `o->key()` | `movzbl 0x2(%rcx)`, `0x3(%rcx)` | new object (owner-hot) | yes | n/a (not yet published) |
| 2 | `mix64(h)` | :3000 `slot_start` :2133 | 2x `imul` + shifts (12 instr) | regs | yes | - |
| 3 | ld `tab_[t]`, `mask_[t]`, `cap_[t]` | :3000-3003 | `mov 0xd0(..)`, `0xe8(..)`, `0xe0(..)` | L4 | yes | **yes — but LOAD only; no RFO** |
| 4 | ld slot word(s) | :3003 `tab_[t][i]` | `mov (%r11),%rax` | slot line | yes (~2.2 probes at 70% load) | yes (shared, expected) |
| 5 | ld `cur->flags`, `cur->klen8`, key bytes | :3031 `cur->key() == key` | `movzbl 0x2(%r15)`, `0x3(%r15)`, **`call memcmp@plt`** (slice.h:33) | old object | yes | yes (immutable; no RFO) |
| 6 | ld `read_local_enabled_` + branch | `read_local_slot_store` :3175-3180 abort guard | `cmpb $0,0x1d1(%rbx); je abort` | L8 | yes | yes — load only |
| 7 | `make_word` assert | :3037 -> :2128-2131 | `shr $0x30; jne` | regs | yes | - |
| 8 | **st slot word (release)** | :3037 -> `__atomic_store_n(RELEASE)` :3180 | plain `mov %rax,(%r12)` — no fence | slot line | yes | **yes — the one legitimate cross-CCX RFO** |
| 9 | `kvobj_size(cur)` | :3117 | `call kvobj_size` (out of line, 0x190 B) which `call kvobj_external_bytes` | old object header (hot from #5) | yes | - |
| 10 | st `obj_bytes_ -=` | :3118 | `sub %rax,0x108(%rbx)` | **L5** (abs 320) | yes | **no** (L5 = obj_bytes_, pending_bytes_, borrows) |
| 11 | ld `atomic_pending_`, ld `->read_local_extended`, 2 branches | `read_local_store_state_required()` flatstore_atomic.inc:843-847 | `mov 0x8(%rbx); test; je abort; cmpb $0,0x524(%r10); je abort` | L1 + sidecar line [1280,1343] | yes | load only |
| 12 | ld `retire_sink.context/.defer`, **indirect call** | :3123 -> read_local_reclaim.h:33 | `mov 0x550(%r10),%rdi; call *0x558(%r10)` | sidecar line [1344,1407] | yes | load only |
| 13 | `defer`: 3 abort checks, cap check | read_local.h:176-177 | `test/je` x3, `cmp $0x1000` | queue object (owner) | yes | no |
| 14 | `defer`: 4 stores (owner,payload,aux,reclaim) + `tail_`,`count_`,`unsealed_` | read_local.h:178-187 | `vmovdqu`, 2x `mov`, `incl 0x38`, packed `vmovq` | ring entry (160 KB ring: 40 B x 4096) + queue object | yes | no |
| 15 | `kvobj_size(o)` | :3042 | second out-of-line `call kvobj_size` | new object (hot) | yes | - |
| 16 | st `obj_bytes_ +=` | :3043 | `add %rax,0x108(%rbx)` | L5 | yes | no |
| 17 | ld `o->flags & HasTtl`; `expires_.erase(h)` | :3047-3051 -> :204-209 | `testb $1,0x2(%r9)`; `call ExpireIndex::erase.isra` (loads `expires_.cap_[1]`, `cap_[0]`; with an index that was never allocated: 2 loads, 2 branches, `ret`) | L6/L7 (FlatStore 376-391) | yes | no |
| 18 | insert-new only: st `live_[t]++` (or `tombs_[t]--`) | :3011 | `incl 0xf0(%rdx)` | **L4** | insert-new / DEL only | **yes — see §3** |
| per pass | `seal_pending`: `epoch.fetch_add(1, seq_cst)` | read_local.h:432, server.h:518-521 | `lock xadd` | GLOBAL epoch line | once per pass with >=1 retire | shared by all owners (by design) |
| per pass | `seal_pending`: stamp store loop | read_local.h:433-436 | `mov %rdi,0x20(%r8,%rcx,8)` per entry (5 instr/entry) | ring | **per entry** (should be per batch) | no |
| per pass | `read_local_grace_floor` | server.h:522-532 | per thread: `threads_[i]` -> `->read_local_state_` (ThreadCtx+0x560) -> `tick` | N foreign lines | N loads per pass, all N even when the head entry is obviously not ready | foreign-written lines (true sharing) |
| per pass | drain: `entry.stamp >= floor` compare, indirect `reclaim`, `entry = {}` | read_local.h:214-219, 422-425 | `cmp %r14,0x20(..)`; `call *0x18(%rbx)`; 5x `movq $0` | ring | per entry | no |
| per pass | reclaim: `kvobj_capacity` recompute + `sdallocx` | kvobj.h:1248-1251 via :3474-3490 | `read_local_reclaim_object` 0x538 B, header decode inlined | old object header (cold again by now) | per entry | - |

Fences: none on the write path. The only fence-bearing code is `read_local_advance_generation`
:3390-3407 (`atomic_thread_fence(acq_rel)` after the odd store) and the `fetch_or/fetch_and` on the
pending bit :3431-3453 — both reached only through `ReadLocalTableGuard` (table moves, ownership
change, poison/pending brackets) or atomic-group publication. `insert_read_local`'s guard is
constructed inactive (`moves_from_old` false) on every plain SET that does not move a key out of an
old table. **Verified: no residual per-write bump of `probe_sequence`** (the word the mixfix left
for topology moves only, see the comment at :504-515). `grep -n probe_sequence` shows writers only
in `read_local_advance_generation`, `foreign_read_pending_witness_{open,close}`.

## 3. Shared-line analysis (what an owner store can invalidate in a foreign reader's cache)

FlatStore member offsets (gdb): `atomic_pending_` 8; `tab_` 208; `cap_` 224; `mask_` 232;
`live_` 240; `tombs_` 248; `rehash_pos_` 256; `field_ttl_gate_` 260; `obj_bytes_` 264;
`pending_bytes_` 272; `expires_` 344-423; `cached_now_ms_` 424; `cached_lru_clock_` 432;
`no_touch_` 433; `maxmemory_enabled_` 464; `read_local_enabled_` 465; `read_local_atomic_filter_`
466. Absolute = Shard base + 56 + offset; Shard base is 64-aligned (§2).

* **L4 [256,319] = FlatStore 200-263** = `atomic_gauge_underflows_` ptr, `tab_[2]`, `cap_[2]`,
  `mask_[2]`, `live_[2]`, `tombs_[2]`, `rehash_pos_`, `field_ttl_gate_`. Foreign readers load
  `tab_/cap_/mask_` from it on EVERY probe (`read_local_snapshot_topology` :3193). The owner
  writes `live_`/`tombs_` on every insert-new and every delete (:3011, :3082), `rehash_pos_` on
  every rehash step, `field_ttl_gate_` on HEXPIRE. **Plain overwrite SET writes nothing here** —
  so on the owner's pure-SET-over-existing-keys workload this line is quiet. On an insert/delete
  heavy workload with foreign GETs it ping-pongs once per mutation. Structural finding; fixing it
  means moving `live_/tombs_/rehash_pos_` off the topology line or mirroring topology into the
  sidecar, both layout changes — **not done in this lane** (layout locks).
* **L5 [320,383] = FlatStore 264-327** = `obj_bytes_`, `pending_bytes_`, `outstanding_borrows_`,
  `borrows_` vector head. Owner-private. Foreign readers never load it. The two per-SET stores
  (#10, #16) are therefore not RFOs against readers. NOTE: this is true only because every Shard is
  64-aligned; with a 32-mod-64 base `obj_bytes_` would share a line with `mask_`, and
  `cached_now_ms_` (stored once per executed op by `execute()`, ex_loop.h:2621) would share a line
  with `read_local_enabled_`. The alignment is an allocator accident, not a declared invariant.
* **L8 [512,575] = FlatStore 456-519**: `rehash_counter_` ptr, maxmemory/read-local latches,
  `random_state_`/`sample_cursor_` (eviction sampling only), snapshot flags. Plain SET: read only.
* **Sidecar** (`ReadLocalStoreState`, heap): readers load `read_local_extended` (1316) and
  `probe_sequence` (1352). The owner's per-SET traffic there is `retire_sink` (1360-1375) —
  loads only. Owner stores land on these lines only for table moves / atomic groups.
* **Slot line**: the release store is the single legitimate per-write RFO.
* **QSBR ring / queue object**: owner-private (inside `ReadLocalExState<true>::Impl`, heap).
* **Global epoch line**: one `lock xadd` per pass per owner, all owners share it (design).
* **Tick lines**: `read_local_grace_floor` loads N of them per pass (each written by its thread at
  every rotation boundary — always a cross-CCX fill when that thread is on another CCX).

Conclusion: on plain overwrite SET the owner's only store to a reader-loaded line is the slot word.
Everything else the owner touches on reader lines is a load. The suspects are per-pass, not
per-write: the N tick loads and the global RMW.

## 4. Instruction estimate (base binary, overwrite path, 1 probe, key < 255, no TTL)

From `objdump` of `insert_into_read_local` (0x45460, 1363 B) — executed path counted by hand:

| segment | instr |
|---|---|
| prologue + header loads + `mix64` + topology loads (0x45460-0x4553b) | 54 |
| probe hit: slot load, ptr/tag/flags/klen compare (0x45557-0x455bf) | 29 |
| key compare block incl. `memcmp@plt` setup (0x456c0-0x45701) | 16 + ~25 in libc |
| match tail: TTL/expired test, `make_word` assert, enabled guard, **slot store**, `call kvobj_size(cur)`, `obj_bytes_-=`, sidecar guards, `call *defer`, `call kvobj_size(o)`, `obj_bytes_+=`, `expires_.erase` dispatch (0x455c5-0x4569b, 0x45930, 0x45920) | ~48 |
| epilogue | 9 |
| callee `kvobj_size` x2 (each: klen branch, enc switch, `lzcnt/shlx` class, `call kvobj_external_bytes` -> `ret 0`) | ~70 |
| callee `defer_thunk` (prologue 8, 3 guards 6, cap check 3, entry + counters 12, epilogue 6) | ~45 |
| callee `ExpireIndex::erase.isra` on a never-allocated index | ~15 |
| **total inside insert_into_read_local** | **~310** |
| `insert_read_local` wrapper fast path (rehash/grow/snapshot/maxmemory gates, inactive guard) | ~45 |
| per-pass amortized per retired object: seal stamp loop 5 + drain compare/call/zeroing ~20 + `read_local_reclaim_object` executed path (`kvobj_capacity` recompute ~30, borrow test, `sdallocx` call ~15) | ~70 |
| **publication + reclaim total** | **~425 instr/op (~10% of 4171)** |

Removable under the laws (see §5): ~30 (header re-decode at free) + ~12 (per-entry stamp store +
compare + zeroing) + ~10 (guards: `!entries_||!reclaim||!payload`, sidecar shape, slot-store
latch) + ~10 (unconditional `kvobj_external_bytes` calls for non-Extern objects) = **~60 instr/op,
about 1.4% of instr/op**. Cycles: the two dependent misses (slot line, old object) dominate the
7.2%; they are inherent to "find the key and publish". Fills: the per-pass tick scan is the only
owner-side item that can move the cross-CCX count; it is N loads per pass regardless of whether
anything is ready.

Not armed-specific but visible in this function's profile (report only, storage lane's domain):
`Slice::operator==` -> `memcmp@plt` for every key compare (find_in has the same); two out-of-line
`kvobj_size` calls per SET (accounting contract, same on the unarmed path).

## 5. Cuts (each its own commit; invariant stated in the message)

A. Reclaim reuses the capacity computed at retire. `retire_obj_read_local` already decodes the
   header for `obj_bytes_`; pass `capacity` as the ring `auxiliary` and free with
   `kvobj_free_with_capacity` — exactly what unarmed `retire_obj` :3508-3521 does. The borrowed
   path recomputes `capacity + external` for `pending_bytes_` (rare). `kvobj_external_bytes` is
   called only for `Enc::Extern` at the two armed accounting sites.
B. QSBR ring: one stamp per sealed batch instead of per entry; no per-entry zeroing at drain;
   entry shrinks 40 -> 32 B. Reclaim decisions unchanged (a batch is ready iff its shared stamp is
   below the floor — the same test each entry made individually, since every entry in a suffix
   received the same stamp).
C. Grace floor scan stops at the first blocking participant and re-tests that participant first
   on the next pass. Same reclaim decisions, strictly fewer foreign tick loads.
D. Drop guards that are impossible by construction on the per-write path: `read_local_slot_store`'s
   latch re-test, `read_local_store_state_required()`'s shape test on the owner retire path,
   `defer`'s three argument tests.

Not cut: `expires_.erase(h)` on non-TTL SET (6 instr on a never-allocated index; same on unarmed
path; skipping it would rest on an exactness invariant of the TTL index that the field-TTL code
explicitly does NOT promise); `make_word` assert (2 instr, boot-verified invariant, shared path);
`kvobj_size` for the new object (accounting contract).
