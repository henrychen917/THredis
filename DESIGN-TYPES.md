# Type foundation

This foundation keeps the server's existing ownership rule: an executor is the sole owner of each
shard, a `KvObj` never crosses into socket work, and replies are RESP bytes emitted through
`Op::Sink`. Collection lanes add representations and commands; they do not add locks, refcounts,
reply trees, or another dispatch path.

## What came from which reference

- The optimized Redis 8.6 fork is the semantic baseline for SET/GETEX and key expiration. Its
  worker-owned active-expiry fix is the direct lesson here: an active cycle must execute on the
  owner of the keyspace it deletes from. Its earlier hash-byte accounting regression is also a hard
  constraint: conversion decisions use maintained counters and the current write's lengths, never a
  scan of the collection.
- Upstream Redis supplies the RESP2 command semantics, the compact hash/set/zset defaults, the
  listpack-to-expanded one-way conversion shape, and lazy plus sampled active expiry behavior.
- Valkey agrees on those defaults and command semantics. Its current zset expands to an ordered
  btree plus hash index rather than Redis/fork's skiplist plus hash table; the foundation reserves
  the `Btree` expanded tag for the zset lane. Valkey's type files also reinforce keeping compact and
  expanded operations behind one type-family API.
- Dragonfly supplies the closest C++ shared-nothing design point: a tagged outer value owns a
  per-type inner object, type-aware destruction switches on the existing tag, and command families
  register their own tables. We adopt those shapes without Dragonfly's fibers, global transaction
  layer, or polymorphic reply builders. Dragonfly currently diverges on several thresholds (for
  example, dense set structures and a byte-oriented small hash policy); those values were studied
  but not adopted because Redis compatibility is the requested default.

## Command registry

Each family owns one static `CommandSpec` table and exports a `CommandTable` view:

| File | Export | Scope |
| --- | --- | --- |
| `src/cmd/t_string.cc` | `string_command_table()` | Strings plus type-agnostic key/expiry commands |
| `src/cmd/t_hash.cc` | `hash_command_table()` | Hash lane |
| `src/cmd/t_list.cc` | `list_command_table()` | List lane |
| `src/cmd/t_set.cc` | `set_command_table()` | Set lane |
| `src/cmd/t_zset.cc` | `zset_command_table()` | Sorted-set lane |
| `src/cmd/t_server.cc` | `server_command_table()` | Client/tool compatibility and cross-shard keyspace commands |
| `src/cmd/t_server.cc` | `server_command_table()` | Connection-local server/admin commands |

`src/cmd/commands.cc` calls all six exports at boot and builds a load-factor-at-most-1/2 linear-
probe table. Hashing uppercases ASCII bytes as it reads them; canonical table names are uppercase.
Lookup normally checks one slot and confirms the normalized bytes, so aliases cannot arise from a
hash collision.

Boot also assigns each row a dense command id. Each thread owns a plain counter array indexed by
that id; command execution increments only its current thread's array and `INFO commandstats`
performs the exceptional cross-thread aggregation. The arrays therefore add no shared write or
atomic read-modify-write to command execution.

`CommandSpec::min_arity` and `max_arity` are inclusive and count the command name. `max_arity == -1`
means unbounded. The IO dispatch path calls the one shared `command_arity_ok()` check before local
execution or routing. Key metadata remains `[first_key,last_key]` with `key_step`; the current
executor lowering is single-key, but it already hashes `first_key` rather than assuming argv 1
(`OBJECT ENCODING` routes by argv 2).

To register a lane command, replace that lane's zero-length `std::array` with a static
`CommandSpec[]`, return its pointer/count, and change no central file. A handler is
`void(Shard&, Op&)`, runs on the shard owner, and emits only through `op.sink()` (a large string GET
is the existing, explicit borrow exception).

## `KvObj` and collection ownership

`Type` is `String`, `Hash`, `List`, `Set`, or `Zset`. Strings retain `Raw`, `Int`, and `Extern`
storage. A small collection uses `Enc::Compact`: 32 bytes of unaligned-safe scalar metadata and the
packed `Compact` byte stream live directly after the key in the `KvObj` allocation. When that tail
cannot fit the object's paid allocator class (or exceeds the 192-byte embed ceiling), it moves
one-way to `Enc::Extern`; the pointer then names exactly one of `HashVal`, `ListVal`, `SetVal`, or
`ZsetVal` from `src/store/typeval.h`.

`kvobj_free()` switches on `Type` and deletes the concrete struct. There is no base class, virtual
destructor, callback pointer, or refcount. A lane may add its expanded C++ container as a member of
its concrete struct; its ordinary destructor then remains behind the same outer switch. The
`OwnsExtern` flag exists only for TTL re-headering: an external collection pointer is transferred
to the new header before the old header retires, so exactly one header destroys it. Embedded
collections are copied into a newly sized single block. Strings are copied during re-headering
because their bytes may still be borrowed by the existing zero-copy GET path.

Use `obj_type_check(o, wanted, op.sink())` after lookup. A missing object is accepted as an empty
key; a present mismatch emits exactly `WRONGTYPE Operation against a key holding the wrong kind of
value` and returns false.

`OBJECT ENCODING` reports these stable names:

| Type | Small | Expanded |
| --- | --- | --- |
| string | `int` or `raw` | `raw` |
| hash | `compact` | `hashtable` |
| list | `compact` | `deque` |
| set | `compact` | `hashtable` |
| zset | `compact` | `btree` |

A conversion must set `CollectionEncoding` through `CompactValue::promote()` only after the new
backing owns copies of every compact entry. Conversion is one-way by default. If a lane later adds
demotion, it needs explicit hysteresis and an O(1) eligibility signal.

## Compact format and mutation contract

`Compact` is one contiguous byte vector containing repeated entries:

```text
[ULEB128 payload length][payload bytes][ULEB128 payload length][payload bytes]...
```

There is no stored outer header or reverse-length suffix. The resident `KvObj` tail supplies its
entry/byte counters; the external `Compact` keeps the same counters in its wrapper. `size()`,
`payload_bytes()`, `encoded_bytes()`, and `capacity_bytes()` are O(1). Iteration is forward through
`Iterator` or `first`/`next` entries. `append`, `replace`, and `erase` update entry and payload totals
at the same time they move bytes. Every mutation invalidates prior `Entry` slices and offsets.

An external `Compact` has no side index through 16 entries. Small access linearly decodes the
bounded byte stream; crossing that cutover builds the circular offset index exactly once and keeps
it thereafter. Embedded collections never have a side index. All offset-taking APIs use logical
offsets; `Compact::logical(Entry)` is the only bridge from an external entry's absolute buffer
offset.

Lane code mutates through `CompactValue::append/replace/erase`, not through a rescanning helper.
Before a compact write it computes the resulting logical entry count and checks
`compact_fits(limit, resulting_entries, largest_incoming_value)`. The list lane instead calls
`list_fits(limit, resulting_entries, resulting_payload_bytes)` because Redis's list default is a
packed-node byte budget. After promotion, every expanded insertion/deletion/replacement calls the
matching `note_expanded_*` method with the backing structure's already-known allocation count.
These maintained entry, payload, and allocation totals are the exact guard against repeating the
fork's O(n)-per-hash-write byte scan.

RESP2 aggregates do not need a reply object: call `reply_array_header(n)` and emit each bulk/integer
sequentially through the same sink. Maps and sets are flat arrays in RESP2.

## Compact thresholds

All eight CLI values are unsigned numeric boot settings and are copied into every shard's immutable
`TypeLimits`.

| CLI settings | Default | Provenance and lane interpretation |
| --- | ---: | --- |
| `--hash-max-compact-entries` / `--hash-max-compact-value` | 512 / 64 | Redis, Valkey, and fork `hash-max-listpack-*`; max field or value length |
| `--list-max-compact-entries` / `--list-max-compact-value` | 4294967295 / 8192 | Redis, Valkey, fork, and Dragonfly default `list-max-listpack-size -2`; unlimited entry count plus an 8 KiB aggregate payload budget |
| `--set-max-compact-entries` / `--set-max-compact-value` | 128 / 64 | Redis, Valkey, and fork string-set listpack defaults; max member length |
| `--zset-max-compact-entries` / `--zset-max-compact-value` | 128 / 64 | Redis, Valkey, and fork zset listpack defaults; max member length (score bytes still count in maintained payload totals) |

The list translation is necessary because Redis/Valkey/Dragonfly store large lists as a chain of
packed nodes, while this foundation's small representation is one `Compact`. A list lane must not
interpret 8192 as merely the largest individual element and then allow an unbounded single compact
blob.

## Expiry

Each `FlatStore` owns an `ExpireIndex`: an open-addressed set of full 64-bit hashes with a byte
state sidecar and no atomics. Inserting/replacing/deleting a `KvObj` updates the set alongside the
keyspace table. No other thread reads it.

Lazy expiry is in `FlatStore::find` and the erase path. After the normal lookup, a non-expiring key
pays one `HasTtl` branch. A TTL key compares its deadline with the executor's cached wall-clock
milliseconds and is deleted before the handler observes it. Thus every command using the store API
gets read/write expiry without duplicating checks in type handlers.

Each executor refreshes one real-time millisecond value at the start of a loop pass and stamps it
onto shards as it executes them; no operation calls `clock_gettime`. On the idle/sweep path it runs
an active pass with `kActiveExpireChecks == 20`. A persistent round-robin cursor selects up to 20
owned shards and divides those 20 expire-index slot checks among them. Resolving a sampled full hash
follows only its FlatStore probe run. Work is therefore bounded by the pass budget plus the
existing bounded incremental-rehash step, never by keyspace size. An active deletion republishes
that shard's size for IO-side `DBSIZE`.

TTL mutation keeps the deadline in `KvObj`'s existing optional layout. Updating an existing TTL is
in-place. Adding or removing the optional field creates a replacement header; strings and embedded
collections copy their value, while external collection ownership moves as described above. `SET`
clears TTL by default, supports
`KEEPTTL`, and `INCR` preserves an existing TTL.

The implemented generic surface is `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT` (including
`NX|XX|GT|LT`), `TTL`, `PTTL`, `PERSIST`, `EXPIRETIME`, `PEXPIRETIME`, `TYPE`, `OBJECT ENCODING`, and
IO-local `DBSIZE`. String extensions are SET's `EX|PX|EXAT|PXAT|NX|XX|KEEPTTL|GET`, plus `GETEX` and
`GETDEL`.

## Lane checklist

1. Work only in the lane's `src/cmd/t_*.cc` plus its concrete `*Val` definition when an expanded
   member is required. Do not edit `commands.cc`, `Op`, `Client`, or the network loops.
2. Register uppercase names with min/max arity, flags, handler, and exact key range. Multi-key
   commands still require the future scatter/gather lowering; metadata alone does not authorize a
   handler to touch foreign shards.
3. Allocate the concrete value and fill `CompactValue`, then attach it with
   `kvobj_adopt_hash/list/set/zset`. A fitting compact value is copied into the `KvObj` tail and the
   temporary wrapper is destroyed; an expanded or oversized value remains external. Ownership is
   consumed only after a non-null `KvObj` is returned.
4. Lookup through `sh.store().find`, call `obj_type_check`, and use the shard's matching
   `type_limits()` entry. Delete the top-level key when the last collection element is removed.
5. Make conversion eligibility O(1), build the expanded backing once, call `promote()` last, and
   maintain expanded counters on every later write. `OBJECT ENCODING` then reports the conversion
   from the shared encoding tag automatically.
6. Emit arrays sequentially through `op.sink()`. Do not add an aggregate reply buffer or borrow
   collection storage; the zero-copy descriptor remains string-GET-only.
