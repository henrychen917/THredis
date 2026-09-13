# NOTES-RECYCLE — the fused owner's post-grace KvObj block cache (`t-recycle`)

Base: `d7e244cec` (t-train6). Feature file: `src/store/kv_block_cache.h`.

## 1. The measurement that motivates it

Profiled on the shipped build (pure writes, pipeline 32, fused, 64 shards, 32 cores):

| arm | ops/s | instr/op |
|---|---|---|
| read-local ARMED | 19.19M | 4132 |
| read-local OFF | 22.05M | 3054 |

Arming costs 1078 instructions and 13% of write throughput, and `mallocx` is 3.73% of armed
cycles against **0%** unarmed. The reason is architectural and is NOT changed here: while
read-local is armed a published object is immutable, because readers hold no lock and must never
see a half-written object. Every armed write therefore allocates a fresh object, copies key and
value into it, publishes the slot, and retires the old object through QSBR. Unarmed writes take
the in-place `try_overwrite` path and allocate nothing.

## 2. What this lane changes

The retired block no longer goes back to the allocator. After the QSBR grace floor passes it, the
fused owner keeps it in a per-size-class free list and the next armed write of that class takes it
back. Nothing about lifetime, publication order or immutability moves.

## 3. The grace-floor safety argument

Three facts, each already load-bearing in this tree:

1. **The block is unreachable.** An armed write unlinks the old object from the slot *before*
   handing it to the owner's deferred-retire ring, and the ring releases an entry only once the
   QSBR grace floor has passed the batch's retirement stamp. Past that floor every reader that
   could have acquired the pointer has crossed a fused rotation tick, so no reader holds it. This
   is exactly the property the existing code relies on when it calls `kvobj_free_with_capacity()`
   at that same point — the cache is fed at the identical call site, not one instruction earlier.
2. **Only the owner may touch it.** Single-owner law: a thread only touches the store of a shard
   it owns, and reclamation runs on the physical owner thread. The cache is owner-private, never
   shared and never locked, so recycling adds no synchronisation and no cross-thread free.
3. **Immutable replacement survives.** A recycled block is handed to a LATER write, which
   publishes it as a *different* object than any reader can be holding — because the block only
   became available after every such reader was gone. Recycling reuses memory that is provably
   unreachable; it does not shorten any object's lifetime by a single instruction.

Corollary used throughout the implementation: **any path that is safe to `free()` on is safe to
recycle on.** `read_local_cache_put()` is called only from the two post-grace reclaim callbacks,
in place of the free they already performed, and returns false for anything it will not take —
whereupon the caller runs its unchanged destroy path, borrow retention included.

## 4. Design

**Per owner, not per shard — measured, not assumed.** The first implementation put the free list
in the shard's existing `AtomicPendingState::free_values[]` (the list the atomics engine already
uses for detached MVCC version blocks). It removed 100% of the allocator calls and 138 instr/op,
and still LOST throughput as the shard count rose:

| shards | base kops/s | per-SHARD cache kops/s | Δ | base cyc/op | per-shard cyc/op |
|---|---|---|---|---|---|
| 1  | 2343 | 2338 | −0.2% | 983 | 953 |
| 8  | 2304 | 2253 | **−2.2%** | 996 | 1025 |
| 64 | 2151 | 2073 | **−3.6%** | 1059 | 1101 |

The cause is cache footprint: a per-shard list puts its head array and byte gauge on two extra
cache lines *per shard*, on a path whose allocator alternative (jemalloc's tcache) is per-THREAD
and therefore always hot. Moving the cache to the fused owner restores that property — one head
array, hot in L1 for every write whatever shard the key lands on — and the numbers invert (§6).
It also makes shard migration trivial: an ownership handoff simply changes which owner's cache
serves the shard next, and no cache is ever cross-thread.

**Reached by a direct pointer, not a callback.** `ReadLocalRetireSink` (the store's type-erased
handle on its owner) grows one member: `KvBlockCache* block_cache`. A third *function* pointer
would have handed back part of the allocator call the cache exists to remove; a data pointer is
one load and a fully inlined list pop. The sink is 3 pointers instead of 2 — it lives in the heap
`ReadLocalStoreState`/`ReadLocalThreadState`, so no locked layout moves.

**Eligibility.** Only inline (`Enc::Raw` / `Enc::Int`) `Type::String` blocks. An `Enc::Extern`
object owns a second allocation whose class is independent of the first, and a collection's block
is not what a SET builds. A Raw block whose value is still borrowed by an io thread is refused, so
`destroy_retired_obj`'s borrow retention keeps working unchanged.

**Bound — derived, no knob.** Two clauses in `KvBlockCache`, plus one supplied per put:

* `kMaxNodesPerClass = kReadLocalRetireRingCapacity` (4096) — *one grace drain's worth*. A drain
  releases at most one retire ring of objects, so a class capped below that refuses memory the
  very next owner pass asks for again. Measured (§6): a 32-node cap leaves 37–50% of writes still
  calling `mallocx` and gives back most of the instruction win, to save 2 KB per thread.
* `kMaxBytes = kReadLocalRetireRingCapacity * kEmbedThreshold` (768 KiB per owner thread) — *one
  grace drain of maximum-inline-value objects*. The retire ring already permits exactly that many
  retired-but-unreclaimed objects to be resident per owner, so the cache adds at most the
  transient the QSBR machinery already tolerates.
* per-put ceiling `obj_bytes_` — the writing shard's own live object footprint. A shard may not
  push the owner's cache above what that shard is itself holding in objects, so an empty or
  shrinking keyspace shrinks the cache instead of pinning a high-water mark. After FLUSHALL every
  shard reads 0 and nothing is admitted at all.

No knob was added. `0 = off with no allocation` is satisfied structurally: the cache is a member
of `ReadLocalDeferredQueue`, which only exists when read-local is armed, and the unarmed write
path is unchanged (one boot-latched, predicted-false branch in `make_set_string`/`make_set_int`,
the same idiom `insert_into`, `erase_in` and `try_overwrite` already use).

## 5. Memory accounting — the decision and why

**Pooled bytes are NOT counted as used.** `obj_bytes_`, `used_memory`, `used_memory_dataset`,
`MEMORY STATS` and the `maxmemory` budget report exactly the numbers they reported before this
lane: a cached block holds no key, and counting free memory as used would evict user data to make
room for a cache. This follows the tree's own precedent — the atomics engine's per-shard value
pool has always been outside `atomic_version_bytes_`.

What changes is honesty about physical memory, in two places:

* INFO Memory gains `mem_block_cache:<bytes>` — the **sum over fused owner threads**, while the
  bound in §4 is **per owner**, so the server-wide ceiling is `threads x 768 KiB` (10.5 MB on the
  soak's 14-thread boot, 48 MB on a 64-thread one). `allocator_allocated` and `used_memory_rss`
  already included these bytes (they are allocated); now the operator can see how many of them are
  cache. Read cross-thread under the same exception `read_local_stats()` and
  `snapshot_preimages()` already take.
* Under `maxmemory` the cache is released at the moment a write is about to be REFUSED
  (`refuse_over_budget`, reached from `make_room_for` and `budget_admit` when the policy cannot
  evict or eviction did not get the shard under budget). It does not change `accounted_bytes()`
  and therefore cannot rescue the write; it is the last physical memory the server can hand back
  before saying no. It is deliberately NOT released on every over-budget write while eviction is
  succeeding: the cache is already bounded by the shard's own live footprint, so releasing it
  there would only make the next write allocate again — the same allocator call count as before
  the cache existed, plus the class walk. That is the maxmemory-full SET cell, and it is exactly
  the regression the placement avoids.

## 6. Results (single fused thread on core 32, driver on core 33, armed, atomic 1)

**Method.** Call counts are exact, from an `LD_PRELOAD` interposer that counts
`mallocx`/`sdallocx`/`nallocx` into a `MAP_SHARED` file and forwards through
`dlsym(RTLD_NEXT, ...)`. Only those three are interposed, deliberately: they are jemalloc-specific
and are never called by libc or the loader, so lazy resolution cannot recurse (interposing
`malloc`/`free` has to bootstrap around `dlsym`'s own allocations, and a previous lane's gdb
breakpoints over-counted badly through inlining and glibc aliasing). `alloc_raw() -> mallocx` and
`free_sized() -> sdallocx` ARE the two calls a KvObj block makes; plain `std::free` appears only
for hash TABLES, which are per resize, not per write. Counters are differenced across a measured
phase after a warm phase, so boot, connection setup and table growth cancel and what remains is the
per-op slope. The load is this lane's own driver (`drv2`: a single connection, commands prebuilt
once so the driver costs a memcpy per op rather than an snprintf, exact op counts, no memtier).
Instructions are `perf stat -e instructions,cycles -p <server pid>` over exactly the measured
phase, divided by its op count. Server: one fused thread pinned to core 32; driver pinned to core
33; `--shards 64 --thread-mode 1s --read-local 1 --atomic 1 --save ""` unless stated.

### Allocator calls and instructions per armed write — 200k ops, 20k keys, 32B values

Measured on the exact shipped binary (`t-recycle` head) against `t-train6`, two independent runs
of the whole matrix (`run A` / `run B`):

| cell | mallocx/op base → recycle | sdallocx/op base → recycle | Δ instr/op, run A | Δ instr/op, run B |
|---|---|---|---|---|
| SET overwrite, klen 16 | 1.0000 → **0.0000** | 1.0000 → **0.0000** | 4043.9 → 3936.1 (**−107.8**) | 4076.7 → 3959.2 (**−117.5**) |
| SET overwrite, klen 24 | 1.0000 → **0.0000** | 1.0000 → **0.0000** | 4052.3 → 3951.0 (**−101.3**) | 4063.9 → 3970.2 (**−93.7**) |
| SET overwrite, klen 40 | 1.0000 → **0.0000** | 1.0000 → **0.0000** | 4089.7 → 3975.6 (**−114.1**) | 4127.6 → 3996.9 (**−130.7**) |
| SET first insert, klen 16 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4255.9 → 4247.9 (−8.0) | 4261.4 → 4258.8 (−2.6) |
| SET first insert, klen 24 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4282.7 → 4273.0 (−9.7) | 4276.3 → 4270.2 (−6.1) |
| SET first insert, klen 40 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4330.4 → 4326.6 (−3.8) | 4319.8 → 4329.3 (**+9.5**) |

**The adverse cell, stated plainly.** First insert is where the cache cannot help: nothing has been
retired yet, `mem_block_cache` reads 0, every write allocates, and the change is a lookup that
always misses. Across the two runs the six first-insert deltas are −8.0, −9.7, −3.8, −2.6, −6.1 and
**+9.5** — they straddle zero, and the largest adverse one is 0.22% of a 4300-instruction op. The
overwrite deltas never straddle zero: −93.7 to −130.7, i.e. −2.3% to −3.2%, six for six.
`nallocx/op` is 0.0000 everywhere, so the closed-form `good_size` is what runs, not a PLT call.

### Throughput and cycles, per-owner cache (400k ops, 3 reps, interleaved arms)

| shards | base kops/s | recycle kops/s | Δ | base instr/op | recycle instr/op | base cyc/op | recycle cyc/op |
|---|---|---|---|---|---|---|---|
| 1  | 2031.2 | 2040.6 | +0.5% | 3262.3 | 3152.4 | 1000.0 | 980.8 |
| 8  | 1989.8 | 2015.7 | +1.3% | 3280.1 | 3169.8 | 1010.7 | 995.5 |
| 64 | 1932.3 | 1975.2 | +2.2% | 3368.3 | 3258.5 | 1065.3 | 1040.1 |

Interleaved (base, recycle, base, recycle, ... one rep each, 64 shards) to rule out box drift
between runs: kops/s 1945.2/1953.7, 1962.5/1974.9, 2025.2/2017.0 — recycle ahead in two pairs of
three, and the absolute level drifts 4% across the six runs, which is why the wall-clock column is
the weak instrument here. This is a single-connection geometry and is loadgen-limited; instr/op is
the strong instrument and it is flat to the fourth digit across every rep. A saturated
multi-connection bench is owed (§11).

### Why the per-class cap is 4096 and not 32 (64 shards, 400k ops, 2 reps)

| per-class cap | pipeline | mallocx/op | instr/op | mem_block_cache |
|---|---|---|---|---|
| 32   | 64  | 0.5001 | 3336.3 | 2048 B |
| 4096 | 64  | **0.0000** | 3271.9 | 4096 B |
| 32   | 256 | 0.3750 | 3163.6 | 2048 B |
| 4096 | 256 | **0.0000** | 3115.6 | 6144 B |

A 32-node cap saves 2–4 KB per owner thread and gives back 40–60% of the mechanism. The bound
that matters is the byte cap, and the drain-sized per-class cap is what makes the reuse rate 100%.

## 7. Every drain / lifecycle path, traced

| path | what happens | why it is right |
|---|---|---|
| **grace drain** (`ReadLocalDeferredQueue::drain_ready` → `read_local_reclaim_object`) | block offered to the cache; refused blocks go to `destroy_retired_obj` unchanged | the feed point; §3 |
| **atomics detach** (`read_local_reclaim_atomic_object`) | tries the shard's MVCC value pool first (unchanged), then this cache, then destroy | pre-existing order preserved |
| **borrowed value** | `read_local_cache_put` refuses; `destroy_retired_obj` retains it in `borrows_[].retired` and `pending_bytes_` | an io thread may still be handing those bytes to the kernel |
| **eviction / maxmemory** (`make_room_for`, `budget_admit`) | `read_local_cache_release_all()` on the refusal edge only (`refuse_over_budget`) | hand back the last physical memory before refusing a write; never on the succeeding-eviction path, which would thrash the cache for no call-count gain |
| **growth / rehash** (`start_rehash_read_local`, `rehash_step`) | untouched: tables are `std::free`d through `retire_table_read_local`, never cached | tables are not KvObj blocks |
| **FLUSHALL / FLUSHDB** (`clear_read_local`) | explicit `read_local_cache_release_all()`; and every subsequent post-grace put sees `obj_bytes_ == 0` and is refused | the keyspace the cache served is gone |
| **snapshot / BGSAVE** (`snapshot_mark_read_local`, `clear_during_snapshot_read_local`) | unchanged; retires still flow through the ring and are cached only at the point they were previously freed | snapshot is thread/cut-barrier based, no `fork()`, so no COW concern |
| **shard resize / ownership handoff** (`rebind_read_local_retire_sink`) | the store's sink now names the NEW owner's cache; blocks already in the old owner's cache stay there for its remaining shards | no cache ever becomes cross-thread; jemalloc handles the eventual cross-arena free (see `alloc.h`) |
| **shutdown** (`drain_shutdown`, `~ReadLocalDeferredQueue`) | `release_all()` at both, before the store destructors free live objects | cached blocks are live to nobody |
| **failed insert** (`discard_set_value`) | NOT cached — freed immediately | every caller is here because the write was refused (OOM / maxmemory); holding memory back under that pressure is the wrong move |

## 8. Layout locks

Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
Config 624 — all eight are compile-time `static_assert`s and all eight held; no assert was edited.
The cache lives in `ReadLocalDeferredQueue`, which is inside the heap-allocated `ReadLocalExState`
`Impl`; the one new sink member lives in the heap `ReadLocalStoreState` / `ReadLocalThreadState`.
`AtomicPendingState` keeps `sizeof == 1352` and `offsetof(read_local_extended) == 1316`.

## 9. Validation

Build: `taskset -c 32-39,160-167 make -j8` — 0 errors, 0 warnings. `make unit` — 3/3 green
(config parser, flip controller, read-local retire ring). Selectors 0/1/3 all `-fsyntax-only`
clean; selector 2 now fails the build with the message that says why.

### Functional batteries (one boot per battery, port 8067, `timeout 900`)

**1s armed** (`--shards 64 --thread-mode 1s --read-local 1 --atomic 1 --save "" --enable-debug-command yes`)
— 14/16: s6, ryow, bplus, atomic_hazards, multi_exec, edgetime, dumprestore, and evict_battery
sections off/noev/lru/vlru/vttl/growth/config all ok. Two red rows, both **reproduced identically
on the shipped `t-train6` binary at the same geometry**:

| row | this branch | t-train6 | verdict |
|---|---|---|---|
| expwide | 101 checks, 1 failure (`S1 MGET: the hook really widened the fan-out`) | 101 checks, **same 1 failure** | the known pre-existing 1s+read-local row |
| evict_battery lfu | `hot 20 mostly survive` FAIL 4 | **FAIL 3** (and FAIL 0 on a re-run of this branch) | fails on both, count varies per run; passes at the gate's own 2s geometry on both |

**2s** (`--shards 16 --ratio 6:2 --atomic 1`) — **16/16 green**, including `flip`, `expwide` and
every evict section. So both 1s reds are geometry, not this change.

**Supplementary** (each battery's own geometry, chosen for the lifecycle paths §7 traced):
lbsignals ok, expireindex ok, flatstore_alloc_fail ok, flush_capture run ok. Two reds, again
identical on `t-train6`: `borrow_registry` passes at its documented 2s geometry on both binaries
and fails on both under 1s+read-local (the armed read path does not take the borrow path, so the
battery's own precondition row goes red); `flush_capture verify` fails on both at plain 2s
(`capture done 0.1s; preimages=0` — BGSAVE completes before the FLUSH can land mid-capture on this
box, so the battery's premise never holds; it is not one of gate.sh's rows).

### Differential vs pinned vanilla Redis 7.4

`tests/differ_gate.sh ./build/tomokv 8067 8068 32-39,160-167 6:2` — 41 suites x atomic{0,1} x
seeds{7,19} plus the 4 multi repeats:

**DIFFER GATE: pass=168 fail=0 (5m56s)** on the exact shipped binary. Run three times in total
across the lane's last three source states, 168/0 each time.

Both battery passes above were also re-run in full on the shipped binary after the last source
edit (the maxmemory refusal-edge placement): 1s 14/16 with the same two rows, 2s 16/16.

## 10. Soak: the cache cannot grow without bound

~12 minutes, 329 samples at 2 s, one boot: `--shards 64 --thread-mode 1s --read-local 1
--atomic 1 --save ""`, 14 fused owners on cores 32-38,160-166, driver on core 39 (this lane's own
driver, no memtier). Two identical cycles of five write-heavy phases, then a third fixed phase and
an idle tail. `block_cache` is the INFO sum over all 14 owners; the per-owner ceiling is 768 KiB,
so the server-wide ceiling here is 10.5 MB.

| phase | what it does | peak RSS (KB) | peak used_memory | **peak block_cache** | keys |
|---|---|---|---|---|---|
| A fixed 32B overwrite | 1 size class, 20k keys | 53,124 | 1.52 MB | **18,688** | 20,000 |
| B value sizes 8..184 | 10 size classes at once | 69,956 | 4.72 MB | **1,053,840** | 20,000 |
| C 2M unique inserts | growth + rehash | 214,376 | 123 MB | 1,053,584 | 1,589,408 |
| D churn (SET + DEL/16) | retire storm | 240,212 | 153 MB | **5,554,256** | 2,018,750 |
| E FLUSHALL | keyspace gone | 222,300 | 0 | **0** | 0 |
| A (cycle 2) | | 199,520 | 1.52 MB | **19,200** | 20,000 |
| B (cycle 2) | | 105,096 | 4.72 MB | **1,054,176** | 20,000 |
| C (cycle 2) | | 70,960 | 4.72 MB | 1,047,952 | 20,000 |
| D (cycle 2) | | 240,756 | 153 MB | **5,548,944** | 2,018,750 |
| E (cycle 2) | | 228,492 | 0 | **0** | 0 |
| F final fixed 32B | back to 1 class | 205,040 | 1.52 MB | **19,328** | 20,000 |
| G idle 20 s | no traffic | 205,040 | 1.52 MB | 21,376 | 20,000 |

What the two cycles prove:

* **Occupancy is a function of the workload, not of elapsed time.** Every phase's peak repeats in
  cycle 2 to within 0.5% (A 18,688 → 19,200; B 1,053,840 → 1,054,176; D 5,554,256 → 5,548,944).
  Nothing accumulates from one cycle to the next.
* **It tracks the working set's size-class spread, as designed.** One class → 19 KB across 14
  owners (~1.4 KB each). Ten classes → 1.05 MB. A DEL-heavy retire storm over a 153 MB keyspace →
  5.55 MB, which is 53% of the 10.5 MB server-wide ceiling and 3.6% of the data it is serving.
* **FLUSHALL collapses it to exactly 0** (both cycles), and `allocator_allocated` falls with it
  (201 MB → 28.9 MB), so the release really returned the blocks rather than merely forgetting them.
* **RSS follows `used_memory`, not the cache.** It peaks with the 153 MB phase and decays back
  toward 200 MB as jemalloc purges; the F and G phases sit flat at 205 MB with a 1.5 MB keyspace
  and a 21 KB cache, i.e. the cache is 0.01% of resident there.
* The idle tail holds 21,376 bytes with no traffic at all: the cache does not drain on idle by
  design (nothing has asked it to), and 21 KB across 14 owners is the plateau, not a ramp.

## 11. Risks a throughput benchmark should check

1. **First-insert-dominated and delete-dominated workloads** pay the lookup and cache nothing.
   Expect parity; anything worse than −0.3% on a pure-insert cell is a defect, not a trade.
2. **Many-size-class workloads** (memtier with a value-size distribution) spread the cache across
   classes and lower the per-class reuse rate. Bench a mixed value-size SET cell, not only a fixed
   one, and watch `mem_block_cache` and `mallocx/op` together.
3. **`--shards` sensitivity is now the opposite of the first design's**, so re-check 16/64/128
   shards: the per-owner cache should be flat in shard count, and if it is not, the head array has
   fallen out of L1 for some reason worth naming.
4. **maxmemory-on cells**: the release call sits on the refusal edge, not on the succeeding
   eviction path, precisely so an evicting SET cell keeps the cache. Bench a maxmemory-full
   `allkeys-lru` SET cell anyway — it is the one regime where the cache could in principle thrash,
   and the counters to watch together are `mallocx/op` and `evicted_keys`.
5. **atomics-heavy cells**: the MVCC value pool is consulted first on the detach path, so an
   atomics workload's blocks mostly never reach this cache; confirm no interaction at `--atomic 1`
   with high write contention.
6. **`release_all` walks all 48 class heads** (6 cache lines) even when one is populated. It is on
   the refusal edge and on FLUSH only, so it was left simple; if a future profile finds it, break
   the loop once `bytes` reaches zero.
7. **Idle RSS** after a large keyspace is deleted: bounded by 768 KiB per fused owner thread
   (48 MiB at 64 owners) and released on FLUSH and on maxmemory pressure, but a long-running mixed
   workload should be watched for the plateau (§10 soak).
