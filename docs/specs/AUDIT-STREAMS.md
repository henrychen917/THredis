# Streams audit — governing document for a TomoKV-cpp Streams build

Scope: what a Streams type lane must be in `tomokv-cpp-perthread`, decided from the source of
Redis 8.9.241 (`R:`), Dragonfly (`D:`), and this tree (`T:`). Line numbers describe the inspected
trees on 2026-08-26. Nothing in any repository was modified to produce this.

Reference prefixes follow `docs/INSPIRATION.md`:
`R:` = `/home/user/Projects/redis`, `D:` = `/home/user/Projects/dragonfly`,
`T:` = `/home/user/Projects/tomokv-cpp-perthread`.

---

## 0. Executive result

| # | Decision | Verdict |
|---:|---|---|
| 1 | rax-of-listpacks macro-node layout | **ADAPT.** Keep the two-tier macro-node *idea* (delta-encoded packed log + an index over node base IDs). **Replace** rax with a sorted `base_id → node` vector and listpack with `Compact`. Note the honest framing (§1.2b): with `keyFixedLen=16` Redis's stream rax has already converged onto ~a sorted array (~34 B/node, 4 hops, ~0.34 B/*entry*), so this is **not** a per-entry memory argument — it is a code-mass, seek/trim instruction-count, and small-stream-floor argument. |
| 2 | Small streams as a compact byte-log in the `KvObj` block | **ADOPT, with a stated ceiling and a kill switch.** The append-at-tail / trim-at-head access pattern is an exact fit for `Compact`'s gap-at-the-ends buffer. But 56 B of irreducible stream header state versus 16 B of `EmbeddedCompact` aux (`T:src/store/kvobj.h:143-176`) leaves ~87 usable bytes — **2–5 tiny entries**. Worth it only because Redis's per-stream floor is a 4 KiB listpack pre-allocation (`R:src/t_stream.c:645-649`), a 30× overcharge in that range. Per the hardcode-or-delete rule, §6.5 item 2 is the measurement that keeps or kills this tier. |
| 3 | Redis's tombstone XDEL (flag in place, never compact) | **ADOPT.** It is what makes `XDEL` O(1) and it is load-bearing for consumer-group correctness (`max_deleted_entry_id`, lag). |
| 4 | The `~` approximate-trim knob | **ADAPT.** `Compact::pop_front` is O(1) (`T:src/store/typeval.h:245-255`), so *exact* head-trim is already cheap. Accept `~`/`LIMIT` for compatibility, implement `~` as exact. See §6.4 — this is a differ-visible divergence and must be normalized. |
| 5 | Blocking XREAD via the existing waiter registry | **ADAPT, do not extend.** XREAD is a *gather* over many keys, not a *select-one* pop. Lower it as an ordinary `MultiShard` scatter and use the blocking registry only as a *park + re-run* trigger, reusing the `BlockingPhase::MoveRequested` → IO-side `xshard_prepare` re-entry that `BLMOVE` already has (`T:src/cmd/blocking.inc:941-997`). |
| 6 | The waiter FIFO's stop-at-first-not-ready rule | **MUST CHANGE for streams.** A not-ready stream waiter has to be *skipped*, not block the queue. Dragonfly hit exactly this and answered it with a tri-state predicate (`D:src/server/tx_base.h:116-121`); its boolean-era bug is still filed at `D:src/server/stream_family.cc:3037-3039`. |
| 7 | Redis's `stream_node_max_bytes` / `stream_node_max_entries` | **ADOPT the names and defaults** (4096 / 100), as `--stream-node-max-bytes` / `--stream-node-max-entries` in `Config`. Dragonfly hardcoded them and drifted into an inconsistency (`D:src/server/stream_family.cc:704-705` vs `:134,:144`); do not repeat that. |
| 8 | Redis 8.x additions (IDMP, `XDELEX`, `XACKDEL`, `XNACK`, `KEEPREF`/`DELREF`/`ACKED`) | **REJECT for phases 1–2.** Not in Dragonfly either (`D:src/server/stream_family.cc:4274-4300`). Phase 1 must reserve the *argument grammar* slots so adding them later is not a re-parse. |

Riskiest parts, ranked: (1) multi-key blocking XREAD gather semantics, (2) the tombstone/`first_id`/
`entries_added` invariant web, (3) `XAUTOCLAIM` cursor + deleted-ID reporting, (4) auto-ID
determinism against the differ oracle. §8.

---

## 1. The structures that matter

### 1.1 The ID scheme (ms-seq)

`streamID` is two `uint64_t` (`R:src/stream.h:13-16`). It is a 128-bit number and every ordering
operation treats it as one:

- **Comparison** is lexicographic on (ms, seq) — `streamCompareID` `R:src/t_stream.c:459-467`.
- **Storage key** is the same 128 bits in **big-endian**, so byte-lexicographic order equals numeric
  order — `streamEncodeID`/`streamDecodeID` `R:src/t_stream.c:441-456`. That encoding exists *only*
  to make a radix tree work; it is not a semantic requirement.
- **Successor/predecessor** are exact 128-bit ±1 with saturation reported as `C_ERR` —
  `streamIncrID` `R:src/t_stream.c:129-144`, `streamDecrID` `R:src/t_stream.c:149-164`. Exclusive
  range bounds `(`…`)` are implemented by calling these, not by a comparison flag
  (`R:src/t_stream.c:2708-2717`).
- **Auto-ID** is `streamNextID` `R:src/t_stream.c:170-179`: if wall-clock ms > `last_id.ms`, the new
  ID is `(now, 0)`; otherwise it is `last_id` with `seq+1`. **The clock never rewinds the ID** — the
  stored `last_id` is the sole authority. This is what makes single-owner streams safe under a
  per-executor cached clock (§4).
- **`ms-*` form** means "use this ms, auto-generate seq". Parsed by setting `*seq_given = 0`
  (`R:src/t_stream.c:2455-2458`) and handled at `R:src/t_stream.c:510-527`. Note the subtlety: with
  `seq_given == 0` and `use_id->ms == last_id.ms`, the result is `last_id.seq + 1`, and
  `last_id.seq == UINT64_MAX` is `EDOM`.
- **Parse grammar** — `streamGenericParseIDOrReply` `R:src/t_stream.c:2425-2473`. Three layers:
  - `strict` rejects bare `-`/`+` (used by XADD, XSETID, XREAD IDs, XDEL, XACK).
  - non-strict maps `-` → `0-0` and `+` → `UINT64_MAX-UINT64_MAX` (XRANGE bounds).
  - `missing_seq` fills an absent `-seq`: **0 for a range start, `UINT64_MAX` for a range end**
    (`R:src/t_stream.c:2706,2712`). Getting this backwards silently truncates XRANGE.
  - Buffer bound is 128 bytes; anything longer is an error (`R:src/t_stream.c:2427`).
- **Sentinels used as IDs.** XREAD encodes `>` as `UINT64_MAX-UINT64_MAX` in the id vector
  (`R:src/t_stream.c:2977-2978`); `$` is resolved to `s->last_id` at parse time
  (`R:src/t_stream.c:2938-2944`); `+` to the last *valid* (non-tombstone) ID minus one
  (`R:src/t_stream.c:2955-2965`).

### 1.2 The rax + listpack macro node

**Layout.** `stream.rax` maps a 16-byte big-endian *master ID* to one listpack holding many entries
(`R:src/stream.h:36-53`). The listpack's first record is a synthetic **master entry**
(`R:src/t_stream.c:585-605`):

```
+-------+---------+------------+---------+-----+---------+---+
| count | deleted | num-fields | field_1 | ... | field_N | 0 |
+-------+---------+------------+---------+-----+---------+---+
```

and every real entry after it is (`R:src/t_stream.c:705-723`):

```
+-----+---------+---------+----------+-------+-------+-/-+-------+-------+--------+
|flags| ms-delta| seq-delta|num-fields|field-1|value-1|...|field-N|value-N|lp-count|
+-----+---------+---------+----------+-------+-------+-/-+-------+-------+--------+
```

with the `num-fields` + field names elided entirely when `STREAM_ITEM_FLAG_SAMEFIELDS` is set
(`R:src/t_stream.c:21`, set at `:662` and `:698`). Three compressions stack: IDs are deltas from the
node's rax key; repeated field *names* collapse to one bit; and `lp-count` (the trailing element
count) is what makes reverse iteration possible without a per-entry back-pointer
(`R:src/t_stream.c:736-744`, consumed at `:1444-1447`, `:1463-1469`).

**Node roll-over** is the only place the knobs are read (`R:src/t_stream.c:610-633`): a new node
starts when `lp_bytes + totelelen >= stream_node_max_bytes` **or** when
`live + deleted >= stream_node_max_entries`. On roll-over the sealed node is `lpShrinkToFit`-ed
(`:626`). A new node pre-allocates `STREAM_LISTPACK_MAX_PRE_ALLOCATE` = 4096 B (`R:src/t_stream.c:30`,
`:645-649`) — clamped by `stream_node_max_bytes` only if that is *strictly less* than 4096, which the
default 4096 is not. **Every stream key therefore costs a 4 KiB listpack from its first XADD.**

**Deletion is a tombstone.** `streamIteratorRemoveEntry` `R:src/t_stream.c:1576-1632` sets
`STREAM_ITEM_FLAG_DELETED` in place, decrements the node's `count`, increments its `deleted`, and
frees the whole node only when the last live entry goes. There is no compaction — the `TODO` at
`:1630-1631` and `:1019-1025` are both still open in 8.9. `s->length` counts live entries only;
`count + deleted` is what the roll-over test uses.

**Trim** — `streamTrim` `R:src/t_stream.c:851-1050`. Whole-node removal when the node is entirely
below the threshold (`:884-909`); otherwise, if `approx` (`~`), it **stops** rather than partially
trim (`:916`); with `=` it walks entries in the head node marking tombstones (`:935-996`). `~` also
carries an implicit `LIMIT` of `100 * stream_node_max_entries` (`R:src/t_stream.c:1057`, `:1069`).

**Iteration** — `streamIteratorStart` `R:src/t_stream.c:1346-1395` seeks the rax with `<=` on the
start key (because the start ID lands *inside* a node whose key is `<=` it) and falls back to `^`/`$`;
`streamIteratorGetID` `R:src/t_stream.c:1400-1546` is a two-level loop: rax node, then listpack
cursor. Reverse mode walks `lpPrev` `lp_count` times per entry.

### 1.2b What rax actually degenerates to here — measured, not assumed

This is the load-bearing analysis for decision 1, and the naive version of the argument ("a radix
tree over monotone integers degenerates into a deep trie") is **wrong**. The real behaviour is more
favourable to rax than that, and the case for replacing it has to be made on other grounds.

Redis does not use the classic rax for streams. `streamNew` passes `keyFixedLen = sizeof(streamID)`
(`R:src/t_stream.c:73` → `raxNewEx`, `R:src/rax.c:195-217`), enabling a **leaf-inlining** path: at
depth `keyFixedLen-1` a node's child-pointer slots hold *values* instead of `raxNode*`
(`raxSlotsAreValues`, `R:src/rax.c:428-430`; layout comment `R:src/rax.c:411-422`). That removes one
16-byte leaf allocation per key. Every fixed-key stream tree opts in — macro nodes
(`R:src/t_stream.c:73`), group PEL (`:310`, `:3427`), consumer PEL (`:3509`); only the *name*-keyed
trees (`cgroups` `:3419`, `consumers` `:3431`) use the variable-length form.

`raxNode` is a 4-byte bitfield header (`iskey`/`isnull`/`iscompr`/`size:29`, `R:src/rax.h:88-92`)
plus inline data; size is `raxNodeCurrentLength` (`R:src/rax.c:150-155`), i.e.
`4 + size + pad + 8·(iscompr ? 1 : size) + 8·(iskey && !isnull)`.

For 16-byte big-endian stream IDs the byte profile is:

```
byte:   0  1  2  3  4  5  6  7 | 8  9 10 11 12 13 14 15
        00 00 01 8F xx xx HH HH| 00 00 00 00 00 00 ss ss
        <---- shared prefix ---><-hot-><--- near-zero suffix --->
```

Two regimes, both shallow:

- **Regime A — one distinct `ms` per macro node.** A compressed 6-byte root, a small branch on
  byte 6, ~4 wide (256-way) branches on byte 7, then **one 24-byte compressed tail node per key**
  with the value inlined. For 1000 keys: **~1006 nodes, ~33.5 KB, ~34 B/key, 4 node hops** — not 16.
- **Regime B — several macro nodes inside one `ms`** (high throughput; only byte 15 differs). No new
  node at all: the divergence lands exactly on `raxSlotsAreValues` (`R:src/rax.c:1155`) and
  `raxAddSlot` (`R:src/rax.c:456-514`) widens the existing leaf parent by one edge byte + one value
  slot. **~9 B/key, zero allocations, zero extra depth.**

Three sequential-key micro-optimizations are already in: last-child-first probe with an early
"greater than max edge" exit and a `memchr` fallback (`R:src/rax.c:656-675`); append-position
short-circuit skipping the sorted-insert scan and its memmoves in `raxAddChild`
(`R:src/rax.c:324-330`) and `raxAddSlot` (`R:src/rax.c:479-492`); and size-class slack reuse instead
of realloc (`R:src/rax.c:286`, `:471`). The iterator is fully stack-resident for 16-byte keys — the
128-byte static key buffer (`R:src/rax.h:167,181`) and 32-slot static ancestor stack
(`R:src/rax.h:141-149`) are never exceeded — and `raxNext` is a true incremental cursor, amortized
O(1) (`R:src/rax.c:1761-1902`), not a re-seek. There is **no** bulk/append API and no last-key
cursor (`R:src/rax.h:123-136`); every insert is a root-down walk, but that walk is ~4 nodes and it
runs only once per `stream_node_max_entries` XADDs.

**The index API this scopes to is tiny.** Across all of `t_stream.c` the `raxSeek` operator histogram
is `"^"` ×36, `">="` ×9, `"$"` ×4, `"<="` ×2 — `">"` and `"<"` are never used by streams (operator
dispatch at `R:src/rax.c:2051-2066`). Four primitives — `front()`, `lower_bound()`, `back()`, and
"greatest ≤ k" — cover every stream, PEL, and consumer-PEL access in the entire file. That is the
whole replacement surface.

**So: Redis's stream rax has already converged onto approximately a sorted array of 24-byte records
with a small index on top.** The honest scorecard against a sorted
`std::vector<{streamID base; uint32_t node}>`:

| | rax (`keyFixedLen=16`) | sorted vector |
|---|---|---|
| Bytes per macro node | ~34 (A) / ~9 (B) | 20, contiguous |
| Bytes per stream *entry* (100/node) | **~0.34** | ~0.20 |
| Seek by ID | ~4 dependent loads + 16 byte compares | `lower_bound`, log₂N contiguous probes |
| Append at tail | root-down walk, sometimes `raxAddChild`/`raxAddSlot` realloc | `push_back`, amortized O(1) |
| `$` / `^` | `raxSeekGreatest` descent / `>=` recursion | `back()` / `front()` |
| Head-trim (XTRIM) | `raxRemove` + upward cleanup + recompression + **re-seek** (`R:src/t_stream.c:904-905`) | `erase(begin())` or a head cursor |
| Iterator vs mutation | strictly unsafe; `RAX_ITER_SAFE` (`R:src/rax.h:172`) is a **dead flag**, never referenced — hence the re-seek discipline | integer index, trivially revalidated |
| Waste | ~8 B/key of literal zeros (the `seq` suffix is *after* the divergence point, so radix compression cannot share it) — ~24% of the rax footprint in regime A | same 8 zero bytes if stored naively; the point is it costs no extra node |
| Code mass to import | ~3000 lines | ~0 (`std::vector`) |

**Conclusion, corrected.** Per-*entry* rax overhead is ~0.34 B — negligible. **The replace decision is
therefore not primarily a memory argument for large streams.** It rests on three things that do hold:
(1) importing ~3000 lines of rax plus ~1800 lines of listpack into a tree that already owns `Compact`
and forbids second representations of the same idea; (2) instruction count on the seek and trim paths
(a pointer-chasing descent plus a mandatory post-mutation re-seek, versus a contiguous binary search
and an integer cursor); and (3) the small-stream floor of §1.4, which *is* a large memory argument.
Dragonfly reached the adjacent conclusion from the other direction: it kept rax but routed every node
access through an indirection (`getNodeLp`, `D:src/redis/stream.h:29`,
`D:src/core/stream_node.h:18-67`) with a tag bit already reserved for a replacement node
representation.

### 1.3 The knobs, in exact format

| Redis | Default | Where read | TomoKV spelling |
|---|---:|---|---|
| `stream-node-max-bytes` | 4096 | `R:src/config.c:3495`; used `R:src/t_stream.c:612-615,646` | `--stream-node-max-bytes N` |
| `stream-node-max-entries` | 100 | `R:src/config.c:3471`; used `R:src/t_stream.c:617-621`, `:1057`, `:1069` | `--stream-node-max-entries N` |

Both are `MODIFIABLE_CONFIG` in Redis, so both must be live via `CONFIG SET`, exactly like the eight
existing compact limits (`T:src/core/config.h:88`, `T:DESIGN-TYPES.md:130-145`). Follow the house
knob rule (`T:src/core/config.h:7-8`): numeric, `0` = off and allocates nothing. `0` on either knob
means "no roll-over on this axis", matching `R:src/t_stream.c:613,617`.

These land in `Config` beside `TypeLimits`, not inside it. `TypeLimits`
(`T:src/store/typeval.h:28-33`) is four named `CompactLimit{max_entries, max_value}` members whose
semantics are *compact→expanded promotion thresholds*; a stream node budget is a *node roll-over*
budget, a different thing. Add a `StreamLimits{node_max_bytes, node_max_entries}` member next to
`type_limits`, and wire both into `t_server.cc`'s `init_config` table, which is built from `Config`
(`T:src/core/config.h:4-5`).

Not adopted: `STREAM_LISTPACK_MAX_PRE_ALLOCATE` (`R:src/t_stream.c:30`). Note the *actual* reason it
exists, which is subtler than "listpack reallocs on every append" — it does not: `lpInsert` skips the
realloc while the current jemalloc size class has slack (`R:src/listpack.c:1105-1109`). The 4 KiB
request exists to land the node in a **large size class immediately**, so the class-slack check
succeeds for the node's whole life; the slack is then reclaimed by `lpShrinkToFit` at roll-over
(`R:src/t_stream.c:626`). `Compact::ensure_space` gets the same amortization from 1.5× growth through
`good_size` off a 32-byte floor (`T:src/store/typeval.h:378-416`) without paying a 4 KiB floor on
streams that never reach it, so pre-allocating would be pure regression here.

### 1.4 What Redis's representation costs

Measured struct sizes (compiled probe against the 8.9 field lists, `-O0` x86-64):

| Structure | Bytes | Source |
|---|---:|---|
| `stream` (8.9, with IDMP fields) | 160 | `R:src/stream.h:36-53` |
| `stream` (7.x field set, for reference) | 80 | — |
| `streamCG` | 64 | `R:src/stream.h:93-121` |
| `streamNACK` | 64 | `R:src/stream.h:140-149` |
| `streamConsumer` | 32 | `R:src/stream.h:124-137` |

A **one-entry stream** in Redis 8.9 is therefore ≈ `160 (stream) + rax (24 B head node, see §1.2b) +
4096 (listpack prealloc) + kvobj` — call it **~4.4 KiB**, dominated entirely by the pre-allocation.
A one-entry stream in the design of §1.6 is one `KvObj` allocation of roughly
`8 + klen + 32 + 56 + entry` — for a 16-byte key and one small field/value, **~140 B, one allocator
class, one allocation**. That is a **~30× floor reduction**.

Be precise about where the win lives. It is a *floor* effect, not a *rate* effect:

| Stream size | Redis bytes/entry (approx) | Where the cost is |
|---:|---:|---|
| 1 entry | ~4400 | the 4 KiB prealloc, ~99% of it |
| 10 entries | ~440 | same |
| 100 entries (one full node) | ~45 | prealloc is shrunk at roll-over (`R:src/t_stream.c:626`), so this is the first honest number |
| 10 000 entries | ~41 | listpack framing + delta bytes + ~0.34 B of rax |

So: **enormous below ~100 entries, roughly at parity above it.** Any claim that this design "beats
Redis on stream memory" must be qualified by entry count or it is a lie. The workloads it wins are
many-small-streams (per-user, per-session, per-device), which is exactly the population Redis's
constant-4 KiB floor overcharges by 30–50×.

Every mutation site in Redis maintains `s->alloc_size` by hand (`R:src/t_stream.c:627-628`,
`:658`, `:675-676`, `:745-746`, `:902`, `:1002`, `:1016-1017`, `:1598`, `:1607-1608`) with a
release-build assert at teardown (`R:src/t_stream.c:117`). Dragonfly refused to replicate that and
used an RAII allocator-delta instead (`StreamMemTracker`, `D:src/server/stream_family.h:14-22`,
`D:src/server/stream_family.cc:38-48`) — with a documented ordering hazard at
`D:src/server/stream_family.cc:1231-1232`. **This tree needs neither**: `ObjectSizeTracker`
(`T:src/store/flatstore.h:1455-1461`) already brackets a mutation and posts the delta, and its
contract comment records the exact bug class both other implementations are working around.

### 1.5 Consumer-group structures (what phase 2 needs)

- `streamCG` (`R:src/stream.h:93-121`): `last_id`, `entries_read`, `pel` (rax ID→NACK), a
  **time-ordered doubly linked list** over the same NACKs (`pel_time_head`/`pel_time_tail`/
  `pel_nack_tail`), and `consumers` (rax name→consumer). The time list is an 8.x addition that makes
  XAUTOCLAIM/`IDLE` O(1)-to-oldest instead of a full PEL scan (`R:src/t_stream.c:5837-5965`).
- `streamConsumer` (`R:src/stream.h:124-137`): `seen_time`, `active_time`, `name`, and its own `pel`
  rax **sharing the same `streamNACK*` values** as the group PEL. Two indexes, one object — the
  invariant that every claim path must preserve (`R:src/t_stream.c:2284-2290`, `:2134-2141`).
- `streamNACK` (`R:src/stream.h:140-149`): `delivery_time`, `delivery_count`, owning `consumer`,
  `id`, plus the time-list links.
- Delivery inserts into both PELs (`R:src/t_stream.c:2264-2299`); `XACK` removes from both
  (`R:src/t_stream.c:3922+`); claiming *reassigns* by removing from the old consumer PEL and
  inserting into the new one while the group PEL entry stays put (`R:src/t_stream.c:2134-2141`).
- `cgroups_ref` (`R:src/stream.h:45`) is an 8.2+ index from message ID → list of groups referencing
  it, used only by `KEEPREF`/`DELREF`/`ACKED`. **Out of scope**; do not lay it down.

Dragonfly kept all four Redis structs verbatim and reimplemented only the management helpers
(`D:src/server/stream_family.cc:261-294`, `:493-497`, `:1134-1140`), calling Redis's
`streamCreateCG` directly (`:1815`). That is a reasonable signal that the *shape* is right even if
the container choice is not.

### 1.6 PORT vs REPLACE — the honest evaluation

**Question as posed:** could small streams live as a compact byte-log in the `KvObj` block with the
same append-mostly access pattern, evaluated honestly against rax's seek-by-ID needs?

**Where a byte-log wins, and it is not close:**

1. **Append is the whole workload, and Redis's append is not as cheap as it looks.**
   `Compact::append` is one varint + one memcpy at `end_` (`T:src/store/typeval.h:125-137`), and one
   `Compact` entry holds an entire stream record. Redis's XADD is:
   - a rax `$` seek plus a roll-over test (`R:src/t_stream.c:556-622`);
   - `lpReplaceInteger` on the master `count` field — and this is the sharp edge. `LP_REPLACE`
     at `poff == LP_HDR_SIZE` (the *first* element) executes
     `memmove(dst+enclen+backlen, dst+replaced_len, old_bytes-poff-replaced_len)`
     (`R:src/listpack.c:1113-1119`), i.e. it names the **entire node body** — up to 4 KiB — on
     **every XADD** (`R:src/t_stream.c:672-676`). When the count's encoding width is unchanged the
     source and destination coincide, so this is a same-address `memmove` whose cost depends on
     whether the libc has a `src == dst` fast path. At best a wasted call, at worst a 4 KiB copy per
     append. **Our count lives at a fixed offset in the header record and is a plain store.**
   - `2N+4` separate `lpAppend`/`lpAppendInteger` calls, one per field, value, and framing field
     (`R:src/t_stream.c:725-744`), each paying its own encoding byte plus a 1–5 byte backlen
     (`R:src/listpack.c:339-374`);
   - a conditional `raxInsert` to re-publish a moved pointer (`R:src/t_stream.c:749-750`).

   In fairness: the *individual* `lpAppend` is nearly free — for an append `poff` is the EOF byte, so
   the memmove is exactly one byte (`R:src/listpack.c:1113-1114`), and the realloc is amortized by
   both the `lp_malloc_size` slack check (`R:src/listpack.c:1105-1109`) and the 4 KiB prealloc. The
   win is not "our memmove is smaller"; it is **one packed record versus 2N+4 framed elements, and
   one header store versus a whole-body `memmove`.**

   Related: `lpSeek` has **no random access** — it is O(min(index, numele−index))
   (`R:src/listpack.c:1694-1725`). External `Compact` builds a circular offset index above 16 entries
   (`T:src/store/typeval.h:333-362`) giving O(1) `at(index)`, which is what makes
   `XRANGE ... COUNT n` from a mid-node position and reverse iteration cheap.
2. **Head-trim is O(1) here and is not in Redis.** `Compact::pop_front` only advances `begin_`
   (`T:src/store/typeval.h:245-255`) — the gap-at-the-ends buffer is *exactly* a stream's
   append-tail/trim-head shape. Redis has no such move: deleting from the head of a listpack is an
   `lpInsert` delete at `poff == LP_HDR_SIZE`, memmoving the whole body
   (`R:src/listpack.c:1113-1119`). That is why Redis trims at **whole-node** granularity and needs
   the `~` approximation at all — and why it settles for tombstones plus an unimplemented GC
   (`R:src/t_stream.c:1019-1025`, `:1630-1631`). We can offer exact `MAXLEN` at approximate cost.
3. **Reverse iteration needs neither a backlen nor an `lp-count` suffix.** Redis pays for reverse
   traversal *twice*: a 1–5 byte reverse-decodable backlen after every listpack **element**
   (`R:src/listpack.c:339-374`, consumed by `lpPrev`/`lpSkipPrev` `R:src/listpack.c:557-574`), and a
   trailing `lp-count` after every stream **entry** so the entry's start can be found by jumping back
   N elements (`R:src/t_stream.c:717-722,736-744`, used at `R:src/t_stream.c:1444-1447`,
   `:1463-1469`). External `Compact` gets the same property from its circular offset index above 16
   entries (`T:src/store/typeval.h:333-362`, `T:NOTES-MDIET.md:81-90`) — `at(entries-1-i)` is O(1) —
   and embedded blobs are ≤192 B and simply rescan.
4. **Allocation count is the lever here** (`T:src/store/kvobj.h:9-13`). One block beats
   `stream` + rax head node + listpack — three allocations minimum, four once the tree branches.

**Where it loses, stated plainly:**

1. **Seek-by-ID is O(n) in a flat log.** This is real and it is the whole reason rax exists. It is
   *not* fatal, because the log is sorted by construction: the fix is a sorted index, not a tree.
   Binary search over `{base_id, node}` pairs beats a 16-byte radix descent on instruction count and
   cache behavior for monotone fixed-width keys — but see §1.2b for how narrow that margin actually
   is, and do not oversell it.
2. **The embedded tier is header-dominated.** `EmbeddedCompact` gives a lane exactly 16 bytes
   (`aux0` + `aux1`) of scalar state and is `static_assert`-locked at 32 B
   (`T:src/store/kvobj.h:143-176`). A stream needs, irreducibly:
   `base_id` 16 + `last_id` 16 + `max_deleted_entry_id` 16 + `entries_added` 8 = **56 B**.
   `length` is `entries()−1` and `first_id` is derivable by scan (§2.2), but none of these four are:
   `base_id` is the delta seed, `last_id` must survive an *empty* stream (Dragonfly's comment at
   `D:src/server/stream_family.cc:3054-3055` is precisely this trap), `entries_added` is an all-time
   counter that survives trims, and `max_deleted_entry_id` survives tombstone removal.
   **Resolution: reserve `Compact` entry 0 as a fixed 56-byte header record.** This needs *zero*
   change to `Compact`, `EmbeddedCompact`, `KvObj`, `Op`, or `Client`, and it follows existing
   precedent — hash already separates its logical count from `Compact::size()` and parks logical
   bytes in `aux0` (`T:src/store/typeval.h:583-591`, `T:src/store/typeval.h:656`), and set packs
   three scalars into `aux0` (`T:src/store/kvobj.h:747-751`). Use `aux0`/`aux1` for the two hottest
   fields (`last_id`) so XADD's monotonicity check needs no entry decode.
   **Consequence, stated without spin:** with a 16-byte key and no TTL the embed budget is
   `min(192, class − (8 + 16 + 32))` = 144 B, of which 57 is the header — leaving ~87 B, i.e. **2–5
   tiny entries**. That is a narrow tier. It is still worth having, because the population it serves
   (a stream with a handful of entries) is exactly where Redis's 4 KiB floor is a ~50× overcharge.
3. **In-place tombstone flips require fixed-width fields.** Fine: put `flags` in a fixed 1-byte
   position so `CompactView::replace` with an equal-length payload is a memcpy with no memmove and
   no index fix-up (`T:src/store/kvobj.h:327-347`, `T:src/store/typeval.h:192-220`).
4. **Gap reclamation after sustained head-trim.** `ensure_space` recenters only when growth is
   needed (`T:src/store/typeval.h:392-394`), so a `MAXLEN`-capped stream amortizes one `active`-byte
   memmove per ~`active/2` appends. Amortized O(1), but it must be *verified*, not assumed — §6.5.

**Verdict.** Build a two-tier `Stream` type:

- **Tier 0 (`Enc::Compact`, embedded):** header record + delta-encoded entries, one allocation.
- **Tier 1 (`Enc::Extern`, `StreamVal`):** a deque of `Compact` log nodes (one per macro node, node
  budget from the two knobs) + a sorted `std::vector<StreamNodeIndex{streamID base; uint32_t node}>`
  for seek + the header + (phase 2) the group table.

Tier 1 is structurally the `ListVal` shape already in the tree — a chain of `Compact` nodes with the
outer object owning the chain (`T:src/store/typeval.h:660-682`) — so it is a reuse, not a new
mechanism. Transition is the existing one-way `kCollectionEmbedMax` migration
(`T:src/store/kvobj.h:180`, `T:NOTES-MDIET.md:28-35`).

---

## 2. PHASE 1 — the log

Commands: `XADD`, `XLEN`, `XRANGE`, `XREVRANGE`, `XREAD` (+ `BLOCK`), `XDEL`, `XTRIM`, plus
`XSETID` and `XINFO STREAM` (both are phase-1 obligations because their fields *are* the phase-1
invariants, and because `TYPE`/`OBJECT ENCODING` must already answer for the new type).

### 2.1 Type plumbing (exhaustive touch list)

`Type::Stream = 5` (`T:src/store/kvobj.h:35`). Every exhaustive switch, all of them found:

| Site | Change |
|---|---|
| `T:src/store/kvobj.h:35` | add `Stream = 5` |
| `T:src/store/kvobj.h:836-852` (`kvobj_size`) | add `case Type::Stream` |
| `T:src/store/kvobj.h:860-866` (`kvobj_free`) | add `delete static_cast<StreamVal*>(ext)` |
| `T:src/store/kvobj.h` | add `kvobj_new_stream` / `kvobj_adopt_stream` beside `:717-719`, `:765-774` |
| `T:src/cmd/t_string.cc:1085` (`TYPE`) | `return "stream"` |
| `T:src/cmd/t_server.cc:832` | same |
| `T:src/snapshot/snapshot.cc:566` | bound check `type_raw > Type::Zset` → `Type::Stream` |
| `T:src/snapshot/format.h:70-75` | add `stream_snapshot_hooks()`; wire in `snapshot_type_hooks` |
| `T:src/cmd/command.h:78-84` | add `CommandTable stream_command_table()` |
| `T:src/cmd/commands.cc:55-57` | add `stream_command_table()` to the list (it becomes the 8th) |
| `T:src/store/typeval.h` | add `struct StreamVal : CompactValue` |
| `T:src/core/config.h:190-215`, `:277` | the two new knobs + help text |
| `T:src/cmd/t_server.cc` `init_config` | CONFIG SET/GET rows for both knobs |

`OBJECT ENCODING` names, extending `T:DESIGN-TYPES.md:84-92`: small → `compact`, expanded →
`stream` (Redis reports `stream` for all streams; `compact` is this tree's honest small-form name and
matches the existing four rows).

### 2.2 On-disk / in-memory record format

Header record (`Compact` entry 0, fixed 56 B payload, little-endian, `memcpy` loads — the tail is not
naturally aligned, same rule as `EmbeddedCompact` `T:src/store/kvobj.h:145-147`):

| Off | Bytes | Field |
|---:|---:|---|
| 0 | 16 | `base_id` (ms, seq) — the ID of the **physical** head record; the seed for prev-delta decoding |
| 16 | 16 | `last_id` — also mirrored into `aux0`/`aux1` so XADD's monotonicity check decodes no entry |
| 32 | 16 | `max_deleted_entry_id` |
| 48 | 8 | `entries_added` |

**`first_id` is deliberately not in the header.** Redis's `first_id` is *"the first non-tombstone
entry"* (`R:src/stream.h:40`), which is **not** the physical head once the head is a tombstone — so
it cannot double as the delta seed. Redis stores both and recomputes `first_id` by scanning
(`streamGetEdgeID(s,1,1,...)`, `R:src/t_stream.c:1046`, `:5086`). Do the same, but pay for the cache
only where it is free: tier 1's `StreamVal` holds `first_id` as an ordinary member (it is external,
8 bytes cost nothing), while tier 0 derives it with a ≤192-byte forward scan. It is on the phase-2
group-lag path (`R:src/t_stream.c:2208`), which is another reason tier 1 caches it and a stream with
groups is always tier 1 (§2.5 item 1).

Entry record (`Compact` entry *i*, i ≥ 1):

```
[ms-delta varint][seq-delta varint][flags u8][nfields varint]
  ( [flen varint][field bytes] )?          -- omitted when flags & SameFields
  [vlen varint][value bytes]  × nfields
```

Deltas are relative to the **previous entry** (not a node base), so `pop_front` stays O(1): popping
the physical head folds the departing record's delta into `base_id`. `flags` carries `Deleted` and
`SameFields`, mirroring `R:src/t_stream.c:19-21`, and sits at a **fixed byte position** so a
tombstone flip is an equal-length `replace()` — a memcpy with no memmove and no index fix-up.
`SameFields` compares against the **previous entry's** field names rather than a synthetic master
entry: same compression, no master record to maintain, and it survives head-trim. Redis's master
entry does not, which is precisely why Redis can only free a node whole
(`R:src/t_stream.c:1595-1600`).

Tier-1 nodes each carry their own header record so a node is self-describing (this is also what makes
the snapshot cursor resumable at a node boundary); the outer `StreamVal` holds the authoritative
stream header, the cached `first_id`, and the sorted node index.

### 2.3 Command semantics to match exactly

**`XADD key [NOMKSTREAM] [MAXLEN|MINID [~|=] threshold [LIMIT n]] <*|id|ms-*> field value ...`**
(grammar `R:src/t_stream.c:2536`, parser `R:src/t_stream.c:1082-1272`, command
`R:src/t_stream.c:2537-2686`.) Order of checks, all differ-visible:
1. Parse options first; syntax errors precede everything (`:2540-2542`).
2. Arity check on the field/value tail: `(argc - field_pos) < 2` or odd → arity error (`:2546-2549`).
3. **`0-0` explicit → error before key creation** (`:2554-2559`), so no empty key is left behind.
4. `NOMKSTREAM` on a missing key → null reply, no creation (`:2564`, `R:src/t_stream.c:2393-2424`).
5. `last_id == UINT64_MAX-UINT64_MAX` → "exhausted the last possible ID" (`:2615-2620`).
6. `streamAppendItem` failure: `EDOM` → "equal or smaller than the target stream top item";
   `ERANGE` → "Elements are too large to be stored" (`:2629-2633`).
7. Reply the generated ID, **then** trim (`:2639-2664`).
8. `LIMIT` is a syntax error without `~` (`R:src/t_stream.c:1082-1272`; Dragonfly enforces the same
   at `D:src/server/stream_family.cc:2655-2657`).

**`XLEN`** — `s->length`, live entries only (`R:src/t_stream.c:2768-2774`). Missing key → `:0`.

**`XRANGE key start end [COUNT n]` / `XREVRANGE key end start [COUNT n]`** —
`R:src/t_stream.c:2695-2765`. Arg order is swapped for REVRANGE (`:2700-2701`). `(` exclusive bounds
via `streamIncrID`/`streamDecrID`, with saturation returning "invalid start/end ID for the interval"
(`:2708-2717`). `COUNT 0` → **null array**, not empty array (`:2741-2742`); `COUNT` absent → 0 =
unlimited (`:2744`). Missing key → empty array (`:2736`).

**`XDEL key id [id ...]`** — `R:src/t_stream.c:5042-5102`. Parse **all** IDs before applying any
(`:5055-5057`) — partial application on a late syntax error is explicitly avoided. Per deleted ID:
bump `max_deleted_entry_id` if greater (`:5071-5073`); if the deleted ID equals `first_id`, recompute
`first_id` from the first live entry (`:5083-5088`). Returns the count actually deleted.

**`XTRIM key MAXLEN|MINID [~|=] threshold [LIMIT n]`** — `R:src/t_stream.c:5244-5282`, same parser as
XADD. Returns entries removed. Both strategies update `first_id` at the end (`R:src/t_stream.c:1042-1047`).

**`XSETID key id [ENTRIESADDED n] [MAXDELETEDID id]`** — `R:src/t_stream.c:3759-3851`. Four ordered
rejections: id < provided `max_deleted_entry_id`; missing key → `nokeyerr`; id < current
`max_deleted_entry_id`; id < the stream's top live ID when non-empty; `entries_added < length`.
Phase 1 implements all of them (they are pure header invariants) but may omit the group clamp at
`:3826-3846` until phase 2.

**`XINFO STREAM key`** — the non-`FULL` map, in this order (`R:src/t_stream.c:5318-5340`):
`length`, `radix-tree-keys`, `radix-tree-nodes`, `last-generated-id`, `max-deleted-entry-id`,
`entries-added`, `recorded-first-entry-id`, then `groups`, `first-entry`, `last-entry`.
We have no radix tree; report `radix-tree-keys` = macro-node count and `radix-tree-nodes` =
node count + 1 (Redis's own root). Do **not** invent new key names — clients parse these.
The 8.9 IDMP fields (`idmp-duration`, `idmp-maxsize`, `pids-tracked`, `iids-tracked`,
`iids-added`, `iids-duplicates`, `R:src/t_stream.c:5334-5346`) are **not** emitted; that puts our
reply at the Redis 7.x/Dragonfly shape, which is what clients expect.

### 2.4 `XREAD` and blocking — the shape that actually fits this tree

**`XREAD [COUNT n] [BLOCK ms] STREAMS key... id...`** (`R:src/t_stream.c:2784-3208`).

The load-bearing observation is that **Redis does not hand data to a blocked XREAD; it re-executes
the command.** `blockForKeys(c, BLOCKED_STREAM, ...)` (`R:src/t_stream.c:3192`) parks the client;
on a wake, `handleClientsBlockedOnKey` → `unblockClientOnKey` → `processCommandAndResetClient`
re-runs the whole command (`R:src/blocked.c:620-677`, `:725-760`). Before parking, `$` is rewritten
in the client's own argv to the concrete `last_id` so the re-run cannot spin
(`R:src/t_stream.c:3167-3178`). Dragonfly does the same thing with a `range_cb` re-hop
(`D:src/server/stream_family.cc:3139-3219`).

This tree already owns that exact mechanism. `BLMOVE` parks, and on wake the **IO thread** re-enters
`xshard_prepare` and re-dispatches through the ordinary scatter engine
(`T:src/cmd/blocking.inc:941-997`, phase `BlockingPhase::MoveRequested`). So:

**Lowering.** Register `XREAD` as `Readonly | MultiShard` (and `Blocking` when `BLOCK` is present),
`cmd_xshard_only`, with the key range discovered from `STREAMS` — this is a *validated* range, like
`command_prepare_scan_route` / `command_prepare_script_route` (`T:src/cmd/command.h:114-119`).
Non-blocking XREAD is then an ordinary gather: each owner emits its ready entries into its arena
slot, IO reassembles in client argument order. Dragonfly's fan-out is structurally identical
(`D:src/server/stream_family.cc:3296-3336`), including its single-shard fast path (`:3250`, `:3264`)
which is worth copying.

**Parking.** If the gather emitted **zero** entries across all keys and `BLOCK` was given, park via
the blocking registry with a per-key cursor, then on any wake set phase → *re-run* and let IO
re-enter `xshard_prepare`. Nothing is handed across; the re-run is the read.

**Five concrete deltas the waiter machinery needs.** These are the phase-1 blocking work items:

1. **`BlockingKey` needs a cursor.** `blocking_probe_key` currently tests
   `CollectionRef(object).entries() != 0` (`T:src/cmd/blocking.inc:300-307`). For a stream that is
   *wrong twice over*: the header record makes `entries()` ≥ 1 on an empty stream, and "non-empty"
   is not the readiness predicate anyway. Add `streamID cursor` to `BlockingKey` and switch the
   probe on `state.kind`.
2. **The readiness predicate is `last_valid_id > cursor`, not `last_id > cursor`.**
   `last_id` survives `XDEL`/`XTRIM`/metadata-only `XSETID`, so a stream that was emptied would look
   permanently ready and spin. Redis dodges this by re-running (`streamLastValidID`,
   `R:src/t_stream.c:1672-1687`, called at `:3054`, `:3077`); Dragonfly hit it head-on and documents
   it at `D:src/server/stream_family.cc:3054-3057`. **Guard `!length → not ready` first**, then
   compare against the last *live* ID.
3. **Not-ready waiters must be skipped, not block the queue.** `BlockingRegistry::service` returns at
   the first not-ready front waiter (`T:src/cmd/blocking.inc:590`) — correct FIFO for a pop, wrong
   for a stream, where waiter A's cursor may be ahead of waiter B's. Dragonfly's answer is the
   tri-state `KeyReadyResult{kKeyNotFound, kNotReady, kReady}` with `kNotReady` waiters moved to a
   `skipped` vector and re-appended (`D:src/server/tx_base.h:116-121`,
   `D:src/server/blocking_controller.cc:250-283`). Adopt the tri-state; do not adopt the boolean
   short-circuit whose residual bug Dragonfly still has filed (`D:src/server/stream_family.cc:3037-3039`).
4. **XREAD is a broadcast; XREADGROUP `>` is a handoff.** One `XADD` must serve *every* parked XREAD
   whose cursor is behind, because the read is non-destructive. With change (3) that falls out of the
   existing `service()` loop for free — it already iterates while data remains
   (`T:src/cmd/blocking.inc:610-613`). XREADGROUP `>` naturally self-limits because the group's
   `last_id` advances per delivery, so the next waiter re-probes against the new value.
5. **The publish hook.** `blocking_plain_mutation_published` string-matches the writer command
   (`T:src/cmd/blocking.inc:883-895`); add `XADD` (and, in phase 2, `XSETID` and `XGROUP SETID`,
   which can make a group readable without an append). The atomic-deferral discipline is already
   correct and needs no change: `blocking_tls_defer_plain` holds publication until
   `xshard_plain_finish` (`T:src/cmd/blocking.inc:865`, `T:NOTES-BLOCKING.md:37-41`).

**Lost wakeups.** This tree's guarantee is the two-pass probe/register with a writer landing
mid-pass recording the minimum ready argument (`T:NOTES-BLOCKING.md:20-24`,
`T:src/cmd/blocking.inc:644-703`). That is the same guarantee Dragonfly buys with
`AVOID_CONCLUDING` lock retention (`D:src/server/stream_family.cc:3278`,
`D:src/server/transaction.cc:1435-1436`). Nothing new is needed — but the phase-1 test must prove
it fires for streams specifically (§6.3), not assume it transfers.

**Timeout reply.** `XREAD BLOCK` timeout is a **null array** (`R:src/t_stream.c:3198`), same shape as
`BLPOP`. `blocking_reply_timeout` (`T:src/cmd/blocking.inc:272`) already emits that for the pop
family; add the stream kinds to the same arm. `BLOCK 0` = forever, already the tree's convention
(`T:NOTES-BLOCKING.md:4-5`, `T:src/cmd/blocking.inc:232`).

**Inside MULTI**, `BLOCK` degenerates to an immediate null-array reply — Redis via
`CLIENT_DENY_BLOCKING` (`R:src/t_stream.c:3163-3166`), Dragonfly at
`D:src/server/stream_family.cc:3020-3024`. Check against `T:src/cmd/multi.inc`'s existing handling.

### 2.5 Phase-1 scaffolding phase 2 will need

Lay these down in phase 1 or phase 2 becomes a rewrite:

1. **`StreamVal` gets a `void* groups = nullptr`** — created on demand, exactly as Redis does
   (`R:src/t_stream.c:82`: *"Created on demand to save memory when not used"*). Costs 8 bytes in the
   external tier and **nothing** in the embedded tier, because a stream with groups is by definition
   not a 2-entry stream and can be forced external on `XGROUP CREATE`.
2. **`entries_added` and `max_deleted_entry_id` must be exact from day one.** They are only *read* by
   XINFO in phase 1, but group lag arithmetic depends on them
   (`streamEstimateDistanceFromFirstEverEntry`, `R:src/t_stream.c:1822-1866`;
   `streamReplyWithCGLag`, `:1759-1820`). Getting them wrong in phase 1 produces a phase-2 bug that
   looks like a lag bug.
3. **Tombstone-range detection.** `streamRangeHasTombstones` (`R:src/t_stream.c:1723-1757`) is the
   predicate that decides whether `entries_read` can be incremented cheaply or must be re-estimated
   (`R:src/t_stream.c:2206-2218`). It is answerable in O(1) from
   `max_deleted_entry_id` vs the range — build it in phase 1 alongside the header.
4. **A `seek(streamID) → cursor` primitive**, not an inlined loop. XRANGE, XREAD, PEL replay
   (`streamReplyWithRangeFromConsumerPEL`, `R:src/t_stream.c:2345-2392`) and XAUTOCLAIM all need
   exactly this. Redis has it as `streamIteratorStart` and gets four callers for one implementation.
5. **A non-destructive `emit_range(cursor, end, count, sink)`** that phase 2 wraps with PEL
   insertion, mirroring how `streamReplyWithRange` is one function serving XRANGE, XREAD and
   XREADGROUP (`R:src/t_stream.c:2049`).
6. **Snapshot hooks with a resumable cursor.** `SnapshotSaveCursor::lane[4]`
   (`T:src/snapshot/format.h:46-51`) is four `uintptr_t` — enough for {phase, node index, entry
   index, byte offset}. Version the payload so phase 2 can append a groups section without a format
   break; the format is already versioned (`T:src/snapshot/format.h:21`).
7. **A `--stream-*` knob pair that phase 2 does not have to add to.** Group state has no node budget.

---

## 3. PHASE 2 — consumer groups

Commands: `XGROUP CREATE|SETID|DESTROY|CREATECONSUMER|DELCONSUMER`, `XREADGROUP`, `XACK`,
`XPENDING`, `XCLAIM`, `XAUTOCLAIM`, `XINFO GROUPS|CONSUMERS|STREAM FULL`.

### 3.1 Structures

Mirror §1.5 with this tree's containers, all owner-only, no refcounts:

```
StreamGroups  : sorted vector<{name, StreamCG}>            // few groups; vector beats a rax
StreamCG      : last_id, entries_read, pel, consumers, time-list head/tail
Pel           : sorted vector<{streamID id, uint32_t nack}> + a NACK arena   // ID-ordered
StreamNACK    : delivery_time, delivery_count, consumer index, prev/next time links
StreamConsumer: name, seen_time, active_time, sorted vector<streamID> into the group PEL
```

The two Redis PEL raxes both have **fixed 16-byte keys and are iterated in ID order** — same
argument as §1.2b (and Redis opts them into the same `keyFixedLen` path, `R:src/t_stream.c:310`,
`:3427`, `:3509`), same answer: sorted vector, binary search. The consumer PEL holds *IDs*, not
`streamNACK*`, so a group-PEL arena compaction cannot dangle (Redis's shared-pointer duality is a
real footgun: `R:src/t_stream.c:2279-2290` has to free-and-refind on a losing `raxTryInsert`).

Keep the **time-ordered doubly linked list** (`R:src/stream.h:108-117`, ops at
`R:src/t_stream.c:5837-5965`). Without it, `XAUTOCLAIM` and `XPENDING IDLE` are full PEL scans.
It is 8 bytes × 2 per NACK and is the one Redis 8.x structural addition worth taking.

### 3.2 Semantics that bite

- **`XREADGROUP` with an explicit ID reads the *consumer's own PEL*, not the stream** — a completely
  different code path (`R:src/t_stream.c:2174-2183`, `:2345-2392`). `>` reads new messages and
  inserts into both PELs. `$`/`+` are hard errors under GROUP (`R:src/t_stream.c:2929-2966`).
- **Delivery inserts into group PEL *and* consumer PEL**, and an existing NACK is *reassigned* rather
  than duplicated (`R:src/t_stream.c:2264-2299`). `NOACK` skips the PEL entirely.
- **Consumer auto-creation** on XREADGROUP (`R:src/t_stream.c:3060-3070`), which is a *write* even on
  a logically-read command — so `XREADGROUP` is `Write | DenyOom`, and it dirties the key.
- **`entries_read` / lag.** `streamReplyWithCGLag` `R:src/t_stream.c:1759-1820` and
  `streamEstimateDistanceFromFirstEverEntry` `R:src/t_stream.c:1822-1866`. Lag is `entries_added −
  entries_read` when trustworthy, and **null** when tombstones make it unknowable. Emitting a wrong
  number here is worse than emitting null.
- **`XAUTOCLAIM`**: `attempts = count * 10` bound (`R:src/t_stream.c:4818`, `:4884`, cited by Dragonfly at
  `D:src/server/stream_family.cc:2238-2242`); returns a **three-element** reply — next cursor,
  claimed entries, **deleted IDs** removed from the PEL (`R:src/t_stream.c:5008-5035`). The third
  element is frequently missed in ports; Dragonfly implements it, and its `count >= 2^18` rejection
  (`D:src/server/stream_family.cc:4211-4213`) is worth copying as a work bound.
- **`XCLAIM`** flags `IDLE`/`TIME`/`RETRYCOUNT`/`FORCE`/`JUSTID`/`LASTID`, with `JUSTID` *not*
  incrementing `delivery_count` (`R:src/t_stream.c:4515-4755`).
- **`XPENDING`** has two shapes: 3-arg summary (count, min ID, max ID, per-consumer counts —
  `R:src/t_stream.c:4340-4400`) and the ranged form with optional `IDLE`. The summary's per-consumer
  array **skips consumers with an empty PEL** (`R:src/t_stream.c:4371`).
- **`XGROUP CREATE ... MKSTREAM`** creates an empty stream; `$` resolves to current `last_id`.
  `XGROUP DESTROY` must wake XREADGROUP waiters so they can learn the group vanished — Dragonfly
  does exactly this (`D:src/server/stream_family.cc:2002-2006`) and it is the second half of
  §2.4 item (3)'s tri-state: a missing key/group is `kReady`-to-error for XREADGROUP but
  `kNotReady` for plain XREAD (`D:src/server/stream_family.cc:3040`).
- **Group-PEL entries can outlive their stream entries** (XDEL/XTRIM under KEEPREF). Every PEL replay
  must tolerate a missing entry: `streamReplyWithRangeFromConsumerPEL` emits a `nil` body
  (`R:src/t_stream.c:2345-2392`), XAUTOCLAIM collects the ID into `deleted_ids`.

### 3.3 Not in phase 2

`XDELEX`, `XACKDEL`, `XNACK`, `KEEPREF`/`DELREF`/`ACKED`, `cgroups_ref`, IDMP (`XADD IDMP/IDMPAUTO`,
`XIDMPRECORD`, `XCFGSET`), and `XINFO STREAM FULL`'s IDMP fields. None are in Dragonfly
(`D:src/server/stream_family.cc:4274-4300`). `XSETID ENTRIESADDED`/`MAXDELETEDID` **is** in phase 1
because it is cheap and Dragonfly's omission of it left dead code (`D:src/server/stream_family.cc:2088`,
`:2120-2121`) — a small warning about deferring header-only options.

---

## 4. Single-owner analysis — does anything leak?

**A stream is one key, one owner.** Confirmed by walking the command list: `XADD`, `XLEN`, `XRANGE`,
`XREVRANGE`, `XDEL`, `XTRIM`, `XSETID`, `XINFO`, `XGROUP`, `XACK`, `XPENDING`, `XCLAIM`,
`XAUTOCLAIM` are **all single-key** (`first_key == last_key == 1`). Only `XREAD` / `XREADGROUP` are
multi-key. So the cross-shard surface is exactly two things: the XREAD gather (§2.4) and the blocking
wakeup at publish.

Everything else checked for leakage, with the result:

| Concern | Verdict |
|---|---|
| Auto-ID generation across shards | **No leak.** `streamNextID` clamps to the stored `last_id` (`R:src/t_stream.c:170-179`), so per-key monotonicity depends on the object, not on a globally coherent clock. Two shards with skewed cached clocks produce two independently-correct streams. |
| Cached-clock coarseness | **Behavioral, not correctness.** This tree refreshes `now_ms` once per executor loop pass (`T:DESIGN-TYPES.md:158-161`); many XADDs in one pass share an ms and increment `seq`. Legal Redis behavior, but it makes auto-ID sequences differ from the oracle — see §6.2. |
| PEL idle times (phase 2) | Same coarseness. `XCLAIM MINIDLE` / `XPENDING IDLE` compare against the cached clock. Bound the test tolerance, do not tighten the clock. |
| `Op` / `Client` / `KvObj` footprint | **No growth.** Streams add a `Type` value and a `*Val` struct; no field in `Op` (336), `Client` (1984), or `KvObj` (8). The blocking cursor lives in `BlockingKey`, which is inside the opaque `BlockingState`, outside `Op` (`T:NOTES-BLOCKING.md:14-18`). |
| Atomic (epoch-MVCC) interaction | **Covered by the existing rule.** XADD is a single-key plain write, so it flows through `blocking_defer_plain_publication` and only publishes after `xshard_plain_finish` (`T:src/cmd/blocking.inc:865`, `:881-895`, `T:NOTES-BLOCKING.md:37-41`). An aborted group exposes no entry and wakes nobody. **This is the rule the task refers to and it needs no extension** — provided XADD is added to the name list at `T:src/cmd/blocking.inc:886-889`. |
| Snapshot pre-image | **Existing hook applies.** Streams are collections; XADD must be bracketed by `ObjectSizeTracker` (`T:src/store/flatstore.h:1455`) and pass the snapshot write-gate like every other collection write. Blocking probes already participate in pre-image prep (`T:NOTES-BLOCKING.md:49`). |
| Eviction / DENYOOM | `XADD`, `XGROUP CREATE*`, `XREADGROUP`, `XCLAIM`, `XAUTOCLAIM` are growth commands → `CmdFlags::DenyOom` (`T:src/cmd/command.h:34`). `XDEL`/`XTRIM`/`XACK`/reads are not, so an over-budget shard can still be drained. |
| Reply borrowing / zero-copy | **Must not be used.** The zero-copy descriptor is string-GET-only (`T:DESIGN-TYPES.md:193-194`) and `zc_ptr` is already overloaded as the scatter/multi/blocking state marker (`T:src/exec/op.h:178-227`). Stream replies emit sequentially through `op.sink()`. |
| Keyspace notifications | Redis fires `xadd`/`xtrim`/`xdel`/`xsetid`/`xgroup-*` (`R:src/t_stream.c:2648`, `:2654`, `:5097`, `:3849`). Dragonfly does not implement stream events at all (`D:src/server/stream_family.cc:1850`). Match whatever `T:NOTES-pubsub.md` already does for the other four types; do not invent. |
| Cross-shard XREAD reply ordering | **Must reassemble in client argument order**, not shard order. Dragonfly does this explicitly (`D:src/server/stream_family.cc:3314-3336`); the existing gather (`ValueSlot`, `key_order`) already has the machinery (`T:NOTES-XSHARD.md:14-36`). |
| Multi-shard blocking XREADGROUP | **One genuine hazard.** Dragonfly needed a separate *validation hop before the action hop* because waiting on sibling shards from inside a shard callback deadlocks two interleaved multi-shard readers (`D:src/server/stream_family.cc:3111-3137`). This tree's blocking probe is already a two-pass design over per-shard tasks with no in-callback waiting (`T:src/cmd/blocking.inc:325-355`, `:644-703`), so the deadlock shape does not exist here — **but the re-run path must not introduce one.** Keep the re-run on the IO thread, exactly as `blocking_resume_move` does (`T:src/cmd/blocking.inc:941-997`). |

**Conclusion for (c):** nothing leaks beyond XREAD's gather and its wake. The one *new* obligation is
that the readiness predicate is per-waiter-cursor and per-key, so the registry must skip rather than
stop (§2.4 item 3). Everything else — atomic deferral, snapshot pre-image, cancellation, ordered
retirement, the `blocking_waiters` gauges — is reused unchanged.

---

## 5. Steal / avoid

### Steal

| From | What | Why |
|---|---|---|
| `R:src/t_stream.c:585-605,702-744` | The three-way entry compression: ID deltas, `SameFields` flag, trailing element count | Real, measured, and cheap. Keep the first two; the third is unnecessary here (§1.6 point 3). |
| `R:src/t_stream.c:1576-1632` | Tombstone-on-delete, free the node only when empty | Makes XDEL O(1) and is load-bearing for `max_deleted_entry_id` and lag. |
| `R:src/t_stream.c:129-179` | `streamIncrID` / `streamDecrID` / `streamNextID` | 40 lines of exact edge-case handling. Port verbatim in spirit; every `(`-bound and `>`-cursor depends on them. |
| `R:src/t_stream.c:2049` | One `emit_range` serving XRANGE, XREAD, XREADGROUP | Four callers, one implementation. §2.5 item 5. |
| `R:src/t_stream.c:5055-5057` | Parse-all-IDs-then-apply in XDEL/XACK | All-or-nothing on syntax errors; a genuinely user-visible property. |
| `R:src/stream.h:108-117` | The time-ordered PEL list | Turns XAUTOCLAIM/`IDLE` from a scan into an O(1) head lookup. |
| `R:src/t_stream.c:3167-3178` | Rewrite `$` to a concrete ID **before** parking | Without it a re-run design spin-blocks forever. The comment says so explicitly. |
| `D:src/server/tx_base.h:116-121` | Tri-state key-ready result | The correct answer to a heterogeneous waiter queue. §2.4 item 3. |
| `D:src/server/stream_family.cc:3054-3057` | `!length → not ready`, and compare against the last *valid* ID | Two real bugs, pre-found. |
| `D:src/server/stream_family.cc:3144-3148` | Re-apply `COUNT` on the post-wake re-run | Their comment records that omitting it returned every entry the waking write added. |
| `D:src/server/stream_family.cc:3087-3109` | Re-resolve the group pointer after a wake | The cached pointer predates the wake. |
| `D:src/server/stream_family.cc:3250,3264` | Single-shard XREAD fast path that prefetches inside the probe hop | Saves a round trip on the overwhelmingly common one-key XREAD. |
| `D:src/server/stream_family.cc:4211-4213` | Hard bound on XAUTOCLAIM `COUNT` | Cheap work bound. |

### Avoid

| Thing | Why |
|---|---|
| **rax, in every one of its three uses** (stream nodes, group PEL, consumer PEL) | All three are fixed-16-byte monotone-ordered keys; Redis itself special-cased them with `raxNewEx(..., sizeof(streamID))` (`R:src/t_stream.c:73`, `:310`, `:3427`, `:3509`) and thereby flattened them to ~a sorted array already (§1.2b). Avoid it for **code mass and seek/trim instruction count**, not for per-entry bytes (~0.34 B — do not claim a memory win here). |
| **listpack, as a container** | We have `Compact`. Importing a second packed format with a different varint scheme, its own backlen suffix, and its own realloc discipline doubles the surface. Take the *encoding ideas*, not the code. |
| **Storing a mutable counter inside the packed stream** | Redis's master `count` is listpack element 0, so bumping it is an `LP_REPLACE` that names the whole node body (`R:src/listpack.c:1113-1119`, called per XADD at `R:src/t_stream.c:672-676`). Counters belong in a fixed-offset header. |
| **Relying on iterator stability across mutation** | rax's `RAX_ITER_SAFE` (`R:src/rax.h:172`) is declared and **never referenced anywhere** — the iterator is strictly unsafe, which is why `t_stream.c` re-seeks `">="` after every `raxRemove` (`R:src/t_stream.c:904-905`, `:1004-1005`, `:4945`). `Compact` has the same rule stated honestly ("every mutation invalidates prior Entry slices and offsets", `T:src/store/typeval.h:39-40`); design the cursor as an index, and revalidate. |
| **`STREAM_LISTPACK_MAX_PRE_ALLOCATE` (4 KiB per new node)** | `R:src/t_stream.c:645-649`. It is ~99% of a one-entry Redis stream's cost. Its real purpose is to land the node in a large allocator size class immediately so `lpInsert`'s slack check (`R:src/listpack.c:1105-1109`) never reallocs — a need `Compact`'s 1.5×-through-`good_size` growth already covers without the floor (§1.3). |
| **Hand-maintained `alloc_size` at every mutation site** | `R:src/t_stream.c:627,658,675,745,902,1002,1016,1598,1607` — nine sites in one file, guarded by a teardown assert (`:117`). Use `ObjectSizeTracker`. |
| **Dragonfly's `StreamMemTracker` allocator-delta** | `D:src/server/stream_family.cc:38-48`. Correct for them, but it introduced an ordering hazard they had to document (`:1231-1232`). Our bracket contract has no such hazard — see the finish-before-erase comment at `T:src/store/flatstore.h:1452-1461`. |
| **Hardcoding the node knobs** | `D:src/server/stream_family.cc:704-705` has no flags, and the trim path still reads the *other* legacy global (`:134`, `:144`) — the two are now inconsistent. |
| **The master-entry record** | It forces whole-node lifetime (you cannot drop the head without losing the field dictionary). Prev-entry `SameFields` gives the same compression and survives O(1) head-trim. |
| **`streamNACK*` shared between two raxes** | `R:src/t_stream.c:2279-2290` free-and-refinds on a losing insert. Store IDs in the consumer PEL, not pointers. |
| **Redis 8.x surface** (IDMP, XDELEX, XACKDEL, XNACK, cgroups_ref) | ~1500 lines of `t_stream.c` for features no client uses yet and Dragonfly does not ship. |
| **A `robj`-style refcount on stream entries** | Explicitly forbidden (`T:src/store/kvobj.h:11-15`). |
| **Extending `blocking_pick` to gather multiple keys** | `T:src/cmd/blocking.inc:309-323` selects *one* argument. Bending it into a gather would make the pop family pay for streams. Re-run through the scatter engine instead (§2.4). |

---

## 6. Validation

**Short answer to (e): yes, differ-vs-oracle works for streams**, and it works *unmodified* for the
large majority of the surface. `tests/differ.py` runs one deterministic command stream against the
target and the Redis-fork oracle and diffs every reply (`T:tests/differ.py:1-6`). Exactly three
things are not byte-diffable and each has a stated strategy: auto-IDs (§6.2), approximate trim
(§6.4), and phase-2 time fields (§6.6).

### 6.1 The `stream` suite (explicit IDs — the primary arm)

Add `gen_stream(rng)` alongside `gen_list`/`gen_zset` (`T:tests/differ.py:176`, `:209`). **Use only
explicit IDs.** With explicit IDs a stream is fully deterministic: `XADD k 5-1 f v`, `XRANGE`,
`XREVRANGE`, `XDEL`, `XTRIM ... =`, `XLEN`, `XSETID`, and every error path. This arm needs **zero**
normalization and should be the bulk of the coverage. Weight it toward the edges: `0-0`, `ms-*`,
`(`-exclusive bounds, `-`/`+`, `COUNT 0` vs absent, IDs at `UINT64_MAX`, duplicate/decreasing IDs,
`NOMKSTREAM` on missing keys, wrong-type keys, empty-after-trim, delete-the-first-entry (which
recomputes `first_id`), and XSETID's five rejection orders.

### 6.2 The auto-ID arm (time control)

`*` reads a clock, so target and oracle cannot agree. Three options, in order of preference:

1. **Structural check, not byte diff.** Issue `XADD k *`, capture *both* returned IDs, assert on each
   side independently: well-formed `ms-seq`, `ms` within a few seconds of the client's own clock,
   and **strictly increasing** across successive XADDs on the same key. Then `XSETID` both servers to
   an identical value and continue the deterministic stream. This is the arm to build first — it is
   the only one that needs no server support.
2. **ID substitution.** After each auto XADD, rewrite subsequent commands to use the *per-server*
   returned ID. This keeps the diff byte-exact for everything downstream of the XADD. It requires the
   generator to be stateful per server, which `differ.py` currently is not — a real but bounded
   change.
3. **A clock knob.** A `DEBUG SET-ACTIVE-EXPIRE`-style `DEBUG STREAM-NOW <ms>` on the target only
   does not help (the oracle still uses real time). Skip.

Note the tree-specific wrinkle from §4: this tree's coarser cached clock groups more XADDs into one
ms than Redis does, so **`seq` values will legitimately differ even under option 1**. Assert
monotonicity, never a specific `seq`.

### 6.3 Blocking arms

Model on `T:tests/blocking.py`, which already covers argument-order priority, FIFO wake order,
multi-element handoff, timeout bounds, the connection parse barrier, disconnect churn with both
gauges returning to zero, and a non-vacuous atomic-visibility arm (`T:NOTES-BLOCKING.md:51-64`).
Streams need these arms on top:

1. **Broadcast, not handoff.** Three clients `XREAD BLOCK 0 STREAMS k $`; one `XADD`. **All three**
   must wake with the same entry. (A list would serve one.)
2. **Skip, do not stop.** Client A blocks with a cursor *ahead* of the stream (an ID from the future);
   client B blocks behind it. One `XADD` must serve B while A stays parked. This is the direct test
   for §2.4 item 3 and it fails on the current `service()` loop.
3. **Empty-stream spin.** `XADD`, `XDEL` the only entry, then `XREAD BLOCK 100 STREAMS k 0-0`. Must
   time out, not spin. (Guards `last_id` surviving deletion.)
4. **`$` rewrite.** Block on `$`; `XADD` twice in quick succession. The waiter must receive exactly
   the first new entry (not both, not neither, and not spin).
5. **`COUNT` on the wake path.** Block with `COUNT 1`; `XADD` five entries in one pass. Exactly one
   entry comes back. (Dragonfly's recorded bug.)
6. **Multi-key, multi-shard.** `XREAD BLOCK 0 STREAMS a b c $ $ $` with a/b/c on different shards;
   XADD to `c`. Reply must name `c` only, and must be in argument order when two of them fire.
7. **Cross-check the gauges.** `blocked_clients` and `blocking_waiters` (`T:NOTES-BLOCKING.md:46-47`)
   return to zero after every arm — the disconnect-churn discipline that already exists.
8. **Non-vacuous atomic arm.** Mirror the existing one (`T:NOTES-BLOCKING.md:61-64`): force an
   atomic group to abandon, prove the XADD it contained woke nobody, then prove a later committed
   XADD does. **Owner rule — vacuous validation:** "0 failures" proves nothing unless the gate
   *opened*; the arm must ship a counter showing the mechanism fired, and any DEBUG-toggle
   instrument needs `enable-debug-command` in the boot conf plus an assertion on the toggle's reply.

### 6.4 Trim normalization (a required, easily-missed normalization)

`XTRIM k MAXLEN ~ 5` leaves *at least* 5 entries in both servers, but **not the same number** —
Redis stops at node boundaries (`R:src/t_stream.c:916`), we trim exactly (§0 decision 4). Handle it
one of two ways, and state which in the suite:

- **Preferred:** the differ suite emits only `=` (and never bare `MAXLEN`/`MINID`, which defaults to
  `=`… verify against `R:src/t_stream.c:1082-1272`). Cover `~` in a separate *property* test
  asserting `XLEN >= threshold` on both, not equality.
- **Alternative:** normalize `XLEN`/`XRANGE` replies after any `~` command. Fragile; avoid.

The same applies to `XINFO STREAM`'s `radix-tree-keys` / `radix-tree-nodes`: normalize both to a
constant, or exclude XINFO from the byte-diff and cover it in a property test.

### 6.5 Property / stress arms (not differ)

1. **Gap-buffer reclamation.** Drive `XADD` + `XTRIM MAXLEN = N` for millions of ops on one key and
   assert `used_memory` reaches a plateau. This validates the §1.6 point-4 amortization claim rather
   than assuming it.
2. **Embed → external migration.** Add entries one at a time across `kCollectionEmbedMax`; assert
   `OBJECT ENCODING` flips exactly once, `XRANGE` output is byte-identical before and after, and
   `used_memory` returns to baseline on `DEL`. Same shape as the `NOTES-MDIET` five-element probe
   (`T:NOTES-MDIET.md:64-78`).
3. **Snapshot round-trip.** Extend `T:tests/snap_typed_roundtrip.py`: save/load a stream with
   tombstones, a non-zero `max_deleted_entry_id`, a trimmed head, and `entries_added > length`.
   Byte-compare `XRANGE - +` and every `XINFO STREAM` field.
4. **Torture / ASAN.** Add streams to `T:tests/torture.py`. The specific hazards: an XADD that
   triggers embed→external migration while a `CompactView::Entry` is live (every mutation invalidates
   prior entries — `T:src/store/typeval.h:39-40`), and tombstone `replace()` at equal length.
5. **Gate.** Add the stream suite to `T:tests/gate.sh` (refs in `T:tests/gate_refs.txt`). **Owner
   rule — preflight contract:** the full gate is STABLE-only; the dev bar is mechanism-fired plus
   no regression in the touched sections. Because §2.4 item 3 changes shared blocking code, the
   *existing* `T:tests/blocking.py`, `T:tests/atomic_ryow.py`, and `T:tests/torture.py` are in the
   touched set even for a stream-only commit.

### 6.6 Group arms (phase 2)

Deterministic and fully differ-able once IDs are explicit: `XGROUP CREATE` at `0`/`$`/`MKSTREAM`,
`XREADGROUP >` competing consumers, `XREADGROUP 0` reading own PEL, `XACK` (including double-ack and
ack-of-nonexistent), `XPENDING` in both shapes, `XCLAIM` with each flag, `XAUTOCLAIM` including the
**third reply element** (deleted IDs), `XINFO GROUPS`/`CONSUMERS`. The only non-deterministic fields
are `idle` / `seen-time` / `active-time` / `delivery_time` — normalize the same way `differ.py`
already buckets `TTL`/`PTTL` (`T:tests/differ.py:71-79`). Add a lag arm: XDEL in the middle of a
group's unread range must make `lag` **null**, not a wrong integer.

---

## 7. Size estimate

| Component | New lines (est.) | Notes |
|---|---:|---|
| `StreamVal` + node/index containers in `typeval.h` | 150–250 | Follows `ListVal`'s chain-of-`Compact` shape |
| Header record + entry codec + cursor/seek | 300–400 | The delta/`SameFields` codec and `emit_range` |
| Phase-1 commands (`t_stream.cc`) | 900–1200 | Compare `t_list.cc` ≈ 1036, `t_zset.cc` ≈ 2350 |
| XREAD scatter lowering (`xshard_commands.inc`) | 200–300 | Gather + argument-order reassembly |
| Blocking deltas (`blocking.inc`) | 150–250 | Cursor in `BlockingKey`, tri-state probe, re-run phase, publish hook |
| Snapshot hooks | 150–200 | Resumable save cursor + load |
| Type plumbing (§2.1) | 80–120 | Spread across ~12 files, mostly one line each |
| Config knobs + CONFIG SET rows | 40–60 | |
| **Phase 1 total** | **~2000–2800** | |
| Phase 2 group structures | 350–500 | PEL, consumer, time list |
| Phase 2 commands | 1100–1500 | XREADGROUP/XACK/XPENDING/XCLAIM/XAUTOCLAIM/XGROUP/XINFO |
| Phase 2 snapshot extension | 150–250 | |
| **Phase 2 total** | **~1600–2250** | |
| Tests (both phases) | 800–1200 | differ suite, blocking arms, property arms |

**Both phases: ~3600–5000 lines of implementation plus ~1000 of tests.**

For calibration, Redis's `t_stream.c` is 6453 lines *including* ~1500 lines of Redis-8.x surface we
are rejecting, all AOF/replication propagation (which this tree does not have), and RDB validation.
Dragonfly's `stream_family.cc` is 4303 lines *on top of* a vendored rax (~3000) + listpack (~1800) +
a retained 13-function subset of `t_stream.c`. So the estimate above is roughly "Dragonfly's C++
without Dragonfly's 4800 lines of vendored C" — which is the whole point of decision 1.

## 8. Riskiest parts

1. **Multi-key blocking XREAD (highest).** It is the only place three subsystems meet: the scatter
   engine, the blocking registry, and a per-waiter readiness predicate. The registry's
   stop-at-first-not-ready rule is *wrong* for streams and the fix (tri-state, skip-and-continue)
   touches shared code that the list and zset lanes depend on. **Mitigation:** make the tri-state
   change first, in isolation, and re-run the entire existing `tests/blocking.py` before writing a
   line of stream code. Both other implementations got this wrong at least once and one still has it
   filed (`D:src/server/stream_family.cc:3037-3039`).

2. **The tombstone / `base_id` / `first_id` / `entries_added` / `max_deleted_entry_id` invariant
   web.** Five pieces of header state, mutated by five commands (XADD, XDEL, XTRIM, XSETID, and — in
   phase 2 — group delivery), each with its own recompute rule (`R:src/t_stream.c:1042-1047`,
   `:5071-5088`, `:3818-3849`). Errors here are invisible in phase 1 and surface as phase-2 lag bugs.
   **Mitigation:** a debug-only invariant checker asserted after every mutation under the ASAN build:
   - `length == entries() − 1 − tombstones`
   - `base_id ==` the ID of the physical head record (the delta seed round-trips)
   - `first_id ==` the ID of the first **non**-tombstone record
   - `entries_added >= length`
   - `max_deleted_entry_id <= last_id`, and `last_id >=` every stored ID
   - `first_id >= base_id`

   Per the vacuous-validation rule, ship a counter proving the checker ran and that at least one
   tombstone and one trim were present when it did.

3. **`XAUTOCLAIM`.** Cursor semantics, the `count * 10` attempt bound, and the three-element reply
   whose third element is the set of PEL entries whose stream entries no longer exist. Dragonfly
   implements it and still does not propagate its side effects (`D:src/server/stream_family.cc:2266-2291`).

4. **Auto-ID determinism against the oracle.** Not a correctness risk, but a *validation* risk: it is
   the one place the differ cannot be byte-exact, so it is the one place a real bug can hide behind
   "expected divergence". **Mitigation:** §6.2 option 1 is mandatory, and the explicit-ID arm must
   carry the real semantic coverage.

5. **The embedded tier's narrow budget.** ~87 usable bytes after the header (§1.6 point 2). If the
   measured migration rate is so high that almost every real stream is external immediately, the
   embedded tier is complexity without payoff. **Mitigation:** this is a *measurable* question — the
   `used_memory` probe in §6.5 item 2 answers it. Per the hardcode-or-delete rule, if the tier does
   not pay, delete it and make streams unconditionally external; the tier-1 design stands on its own.

6. **Gap reclamation under sustained trim** (§1.6 point 4). Lower risk, but it is an assumption about
   `ensure_space` recentering that has never been exercised by a workload that trims the head
   forever. §6.5 item 1 is the test.
