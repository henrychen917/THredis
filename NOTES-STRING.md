# String lane notes

## Semantics and storage decisions

- `SET`, `GETSET`, `SETNX`, `SETEX`, and `PSETEX` use the strict Redis integer parser before
  choosing storage. Canonical signed 64-bit decimal strings use `Enc::Int`; leading-zero forms,
  `+1`, and `-0` remain raw. `GET` still returns an integer-encoded value as a RESP bulk string.
- `APPEND` on a missing key follows Redis's set-value path and may create `Enc::Int`. Once an
  existing value is appended to, the result is raw, including an empty append to an integer value.
  A non-empty `SETRANGE` also materializes an integer value to raw. `GETRANGE` materializes only
  into a 24-byte handler-local buffer for the reply and does not change the stored encoding.
- All raw writes call `FlatStore::try_overwrite` or install a new `KvObj` with `FlatStore::insert`.
  `try_overwrite` rejects a borrowed value; `insert` retires the old object through the pending-free
  protocol. No handler calls `realloc` or writes through a possibly borrowed value pointer.
- `APPEND`, `SETRANGE`, `INCR`, `DECR`, `INCRBY`, `DECRBY`, and `INCRBYFLOAT` preserve an existing
  TTL. `GETSET` and ordinary `SET` clear it. `SET KEEPTTL` carries it forward, while an explicit
  expiry replaces it.
- `SETRANGE` zero-fills a new gap. An empty value is a no-op: it returns zero for a missing key and
  the existing byte length for a present string, without creating or re-encoding a key. Resulting
  strings may be at most 512 MiB; `APPEND` uses the same cap and the Redis error
  `ERR string exceeds maximum allowed size (proto-max-bulk-len)`.
- `GETRANGE` uses inclusive indexes and Redis's negative-index/clamping rules. Missing keys and
  empty/outside ranges return an empty bulk string. `STRLEN` is O(1); integer strings need at most
  20 bytes of local decimal formatting.
- `INCR`, `DECR`, `INCRBY`, and `DECRBY` share one checked signed-add path and always store the
  result as `Enc::Int`. `DECRBY INT64_MIN` reports `ERR decrement would overflow` before looking up
  the key; result overflow reports `ERR increment or decrement would overflow`.
- `INCRBYFLOAT` uses Redis's strict `strtold` validation, rejects NaN inputs, rejects overflow or a
  NaN/infinite result, and stores the result raw. Formatting matches Redis `LD_STR_HUMAN`:
  `%.17Lf`, trailing fractional zero removal, and `-0` normalization to `0`. The result is returned
  as a bulk string and an existing TTL is retained.

## `SET` option behavior

`NX` and `XX` are mutually exclusive. `KEEPTTL` is mutually exclusive with `EX`, `PX`, `EXAT`, and
`PXAT`; the four expiry forms are mutually exclusive with one another. As in Redis 7, repetition
of the same flag (including `GET`, a condition, or the same expiry kind) is accepted, and the last
value of a repeated expiry kind wins. Expiry values are strict positive signed 64-bit integers and
seconds-to-milliseconds conversion is overflow checked.

With `GET`, the old value is copied to the reply before the condition is evaluated. This produces
the Redis 7 condition behavior:

- `SET missing v NX GET`: returns nil and creates the key.
- `SET existing v NX GET`: returns the old string and leaves the key unchanged.
- `SET missing v XX GET`: returns nil and leaves the key missing.
- `SET existing v XX GET`: returns the old string and replaces the key.
- `SET non-string v NX GET`: returns `WRONGTYPE`, even though `NX` would fail. Without `GET`, a
  failed `NX` condition does not inspect the existing value's type.

Without `GET`, a failed condition returns nil and a successful write returns `OK`. With `GET`, the
old bulk/nil reply replaces `OK`. An already elapsed `EXAT`/`PXAT` deadline performs the successful
conditional write as set-then-expire: it removes an old key, leaves no replacement, and retains the
already-copied `GET` reply.

## Work bounds

- Option, integer, and float validation is linear only in the corresponding argument bytes. Integer
  parsing is bounded to a signed 64-bit representation; Redis-compatible long-double parsing and
  formatting are bounded by the 5 KiB Redis conversion buffer.
- `GETRANGE` is O(returned bytes). `SET ... GET` and `GETSET` are O(old reply bytes), which their
  reply semantics require.
- `APPEND` and non-empty `SETRANGE` build one raw result and install it through the owner-safe store
  path, so work is O(result bytes) plus the bytes supplied by the write. They do not scan any
  collection, keyspace, or conversion-eligibility state. The full-result copy is the cost of
  keeping published/borrowed string allocations immutable until retirement.
- Every integer mutation and string encoding decision is O(1) with respect to keyspace and value
  collection cardinality. No store locks, atomics, or rescanning conversion checks were added.

## Reference provenance

- `/home/user/Projects/wt-round-mainline/src/t_string.c`: adopted the optimized fork's shared
  integer increment/decrement shape, integer encoding choices, TTL preservation on numeric and byte
  mutations, Redis-compatible errors, and its set-family ordering. The worker-owned design and the
  no-rescan lesson in `DESIGN-TYPES.md` remain the local concurrency/performance contract.
- `/home/user/Projects/redis/src/t_string.c`, `object.c`, and `util.c` (including the Redis 7.2
  source): adopted the SET condition/GET reply ordering, option exclusions and repeated-option
  behavior, range rules, 512 MiB checks, strict canonical integer parsing, byte-command
  materialization, overflow errors, and the exact long-double validation/formatting rules.
- `/home/user/Projects/valkey/src/t_string.c`, `object.c`, and `util.c`: used as an independent
  confirmation of set-family replies, integer encodings, TTL behavior, range handling, and
  `INCRBYFLOAT` formatting.
- `/home/user/Projects/dragonfly/src/server/string_family.cc`: adopted its single-owner/single-hop
  command-family shape and used its range/type behavior as a C++ cross-check. Its double-based float
  storage and broader transaction/tiering machinery were not adopted because this lane requires
  Redis long-double behavior and the existing `FlatStore` ownership model.
- `DESIGN-ZC.md`: adopted the mandatory borrow rule that any potentially published value is either
  changed only through guarded `try_overwrite` or replaced through `insert`/retirement.

## Regression tests to add

### Encoding and byte commands

- `SET k 123`; assert `OBJECT ENCODING k` is `int`, `GET k` is bulk `"123"`, `STRLEN k` is 3,
  and `GETRANGE k 1 -1` is `"23"`.
- `SET k 123`; `APPEND k 4` returns 4, `GET k` is `"1234"`, and encoding is `raw`. Repeat with
  `APPEND k ""`; an integer value must still become raw.
- `SET k 123`; `SETRANGE k 1 X` returns 3 and stores `"1X3"` as raw. `SETRANGE k 99 ""` returns 3
  and leaves the integer encoding unchanged.
- `SETRANGE missing 4 x` returns 5 and stores four zero bytes followed by `x`; test embedded and
  external lengths. `SETRANGE missing 536870911 x` is allowed, while offset 536870912 with `x`
  returns the proto-cap error. Do not construct these cap-edge values in routine unit runs unless
  the fixture can tolerate the allocation.
- `APPEND` at exactly 512 MiB succeeds and one byte beyond fails without changing the old value.
- Wrong-type cases for all byte/read commands; invalid `GETRANGE` indexes and invalid/negative
  `SETRANGE` offsets; missing-key empty-range/no-op behavior.

### SET family and TTL

- All four `NX GET`/`XX GET` missing/existing cases listed above, plus `NX GET` against a hash for
  the pre-condition `WRONGTYPE` rule. Specifically retain `SET k v XX GET` on a missing key as a
  nil reply with no key created.
- Every valid pairing of one condition, optional `GET`, and one of `EX`, `PX`, `EXAT`, `PXAT`, or
  `KEEPTTL`; both option orders; case-insensitive spellings; repeated `GET`, repeated `NX`, and
  repeated same-kind expiry with the last expiry winning.
- Syntax errors for `NX XX`, expiry plus `KEEPTTL`, two different expiry kinds, missing expiry
  operands, and unknown options. Invalid/zero/negative/overflowing expiry values must name the
  issuing command (`set`, `setex`, or `psetex`) and leave the old key unchanged.
- `SET` clears TTL; `SET KEEPTTL`, `APPEND`, `SETRANGE`, all integer mutations, and `INCRBYFLOAT`
  preserve it; `GETSET` clears it. Cover both raw and integer encodings.
- Past `EXAT`/`PXAT` with and without `GET`, including condition success/failure and an existing
  non-string value.
- `SETNX` returns 0 for every existing type without `WRONGTYPE`, returns 1 for missing/expired keys,
  and uses integer encoding for canonical integer input. `SETEX`/`PSETEX` overwrite other types.

### Numeric edges

- Run `INCR`, `DECR`, `INCRBY`, and `DECRBY` across zero and both signed 64-bit boundaries. Include
  `INCRBY k INT64_MIN`, `DECRBY k INT64_MIN`, negative decrements, invalid `+1`, `01`, `-0`, empty,
  whitespace, and 21-digit inputs. Verify the old value and TTL are unchanged on every error.
- Start integer commands from both `Enc::Int` and raw numeric strings, and confirm every successful
  result is `Enc::Int`.
- `INCRBYFLOAT missing 0.1`, then `0.2`, must store/reply `0.3`; cover `-0`, values requiring all 17
  fractional places, large fixed-form values, exponent inputs, long-double subnormals, explicit
  `inf`, `-inf`, `nan`, overflow, and a sum that becomes infinity. Invalid old values must win over
  invalid increment values, while a wrong-type key must win over both.
- Verify `INCRBYFLOAT` stores exactly the bytes it replies with and that a following invocation
  parses those bytes without drift.

All runtime cases are proposed tests only; this lane was validated with compile-only `make -j` and
no binary execution.
