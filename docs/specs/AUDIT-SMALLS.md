# AUDIT — remaining "small" parity items for TomoKV-cpp

Audit date: 2026-08-26. Read-only. No repository was modified.

**Trees read**
- Target: `/home/user/Projects/tomokv-cpp-perthread` (perthread fork, HEAD `20ce65a7e`, 164 registered commands).
- Oracle: `/home/user/Projects/redis/src`, `REDIS_VERSION "8.9.241"` (`version.h:1`).

**Caveat on the oracle tree.** This Redis checkout is *not* stock upstream. `RDB_VERSION` is **15**
(`rdb.h:21`) and `rdb.h` carries type tags up to **33**, including hash *templates*
(`RDB_TYPE_HASH_TMPL_LP`=29, `_TMPL_ARRAY`=31, and their `_REF` variants 30/32), `RDB_TYPE_ARRAY`=28
and `RDB_TYPE_GCRA`=33. Item 7 (DUMP/RESTORE) is the only item where this matters, and it matters a
lot — see §7. Every other item's semantics match documented upstream behaviour.

**Conventions used below**
- Every mechanism claim carries a `file:line`. Claims about tomokv carry the tomokv path.
- Build size: **S** ≤ ~400 LOC and no new subsystem; **M** ~400–1200 LOC or one new owned structure;
  **L** > 1200 LOC or a new cross-thread mechanism / new store form.
- Knob philosophy follows the house rule (numeric knobs; `0` = off ⇒ no allocation and a
  byte-identical plain path; `-1` = auto). No boolean-only knobs are proposed.
- Every proposal is checked against the three standing footprint locks: `sizeof(KvObj)==8`
  (`src/store/kvobj.h`), `sizeof(Op)==336` (`src/exec/op.h:238`), `sizeof(Client)==1984`
  (`src/net/conn.h:538`).
- Test plan sketches assume the two existing instruments: `tests/differ.py <thost> <tport> <ohost>
  <oport> <suite> [seed]` against the local Redis 8.9 oracle (suite registry at
  `tests/differ.py:676`) and `tests/gate.sh quick`.

---

## 1. BITFIELD / BITFIELD_RO — **BUILD**

### Redis mechanism

Entry points: `bitfieldGeneric(client*, int flags)` at `bitops.c:1886` (body to `:2123`);
`bitfieldCommand` `bitops.c:2125`; `bitfieldroCommand` `bitops.c:2129`. Table flags:
`bitfield` = `CMD_WRITE|CMD_DENYOOM`, `bitfield_ro` = `CMD_READONLY|CMD_FAST`
(`commands.def:12820-12821`).

**Two-pass structure.** `struct bitfieldOp {offset, i64, opcode, owtype, bits, sign}` at
`bitops.c:1874-1881`. Pass one (`bitops.c:1896-1964`) parses *every* subop into a realloc'd array
and touches no key. `OVERFLOW <mode>` produces no op — it mutates a running `owtype` local that all
*subsequent* ops inherit, then `continue`s (`bitops.c:1910-1924`). During this pass, for each write
op:

```c
if (highest_write_offset < bitoffset + bits - 1)
    highest_write_offset = bitoffset + bits - 1;     /* bitops.c:1942-1945 */
```

Between the passes there is **exactly one** key access: `lookupKeyRead` when every op was a GET
(`bitops.c:1969`), otherwise `lookupStringForBitCommand(c, highest_write_offset, &strOldSize,
&strGrowSize)` (`bitops.c:1983-1984`). That helper (`bitops.c:790-815`) creates or `sdsgrowzero`s
the string to exactly `(highest_write_offset>>3)+1` zero-filled bytes, **once**, so the execute pass
never re-grows. Pass two (`bitops.c:1990-2108`) emits `addReplyArrayLen(c, numops)` first
(`bitops.c:1990`) and then applies each op in order.

**Offsets.** `getBitOffsetFromArgument` `bitops.c:714-747`. The `#` form multiplies the parsed value
by `bits` behind an overflow guard `if (loffset > LLONG_MAX / bits)` (`bitops.c:730-736`). The cap is
`if (!mustObeyClient(c) && (loffset >> 3) >= server.proto_max_bulk_len)` (`bitops.c:738-743`) — note
this bounds the *starting* offset, not `offset+bits-1`, and `lookupStringForBitCommand` performs no
`checkStringLength`, so the final string can legally exceed `proto-max-bulk-len` by up to 8 bytes.
Error string, verbatim (`bitops.c:716`):
`bit offset is not an integer or out of range`

**Types.** `getBitfieldTypeFromArgument` `bitops.c:756-780`. `i` → signed, `u` → unsigned
(`bitops.c:761-768`); limits `llbits < 1 || (sign && llbits > 64) || (!sign && llbits > 63)`
(`bitops.c:770-777`) — **signed 1–64, unsigned 1–63**. Error string, verbatim (`bitops.c:758`):
`Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.`

**Bit primitives.** `setUnsignedBitfield` `bitops.c:500-513`, `setSignedBitfield` `:515-518`
(reinterprets the `int64_t` as `uint64_t` and delegates), `getUnsignedBitfield` `:520-532`,
`getSignedBitfield` `:534-554`. All four loop **bit by bit**, MSB-first within each byte
(`byte = offset>>3`, `bit = 7-(offset&7)`) — identical numbering to SETBIT, documented at
`bitops.c:479-498`. `getSignedBitfield` sign-extends manually:
`value |= ((uint64_t)-1) << bits` when `bits<64` and the top bit is set (`bitops.c:551-552`).

**Overflow.** Modes `BFOVERFLOW_WRAP 0 / SAT 1 / FAIL 2` (`bitops.c:575-577`).
`checkUnsignedBitfieldOverflow` `bitops.c:579-614`, `checkSignedBitfieldOverflow` `:616-669`. Both
return **0 = fine, 1 = overflow, -1 = underflow**, and write the corrected value through `limit`.
WRAP = modular truncation to `bits` (unsigned `bitops.c:605-613`; signed `:649-667`, which
sign-extends or masks based on bit `bits-1` of the sum). SAT = clamp to type max/min. FAIL =
no write, nil element.

Application (`bitops.c:2007-2073`): **SET replies the OLD value; INCRBY replies the NEW value.** For
SET, the overflow check is called with `incr=0` — i.e. it asks whether the *literal* target value
fits the type. The FAIL branch is
`if (!(overflow && thisop->owtype == BFOVERFLOW_FAIL)) { reply; write; } else addReplyNull(c);`
(`bitops.c:2029/2037` signed, `:2063/2071` unsigned).

**GET semantics.** `bitops.c:2076-2106`. `src = getObjectReadOnlyString(o,&strlen,llbuf)` renders an
int-encoded value into a stack buffer; then up to 9 bytes starting at `offset>>3` are copied into a
`memset(buf,0,9)` window and the field is read from that zero-padded window. Consequences: **GET on
a missing key, or past the end of a short string, returns integer `0` — never nil**, and a pure-GET
BITFIELD **never creates the key** (it took the `lookupKeyRead` branch at `bitops.c:1969`).

**BITFIELD_RO** is refused *after* parsing, only if at least one write op was present
(`bitops.c:1974-1979`), verbatim: `BITFIELD_RO only supports the GET subcommand`

Side effects tomokv has no analogue for: `notifyKeyspaceEvent(NOTIFY_STRING,"setbit",...)` — note the
event name is `setbit` even for BITFIELD — and `server.dirty += changes` (`bitops.c:2118-2120`), with
the quirk that `if (strGrowSize || (oldval != newval)) changes++` (`bitops.c:2034/2068`) makes *every*
write op count as a change whenever the string grew at all.

### TomoKV design sketch

This is the SETBIT shape, widened. Single key at argv 1 ⇒ an ordinary owner task; no scatter, no new
store path, no new type. `src/cmd/t_string.cc` already has every primitive:

- `parse_bit_offset` (`t_string.cc:544-553`) already implements the `(offset>>3) >= kProtoMaxBulkLen`
  cap and the exact error string; extend it with an optional `#`/`bits` multiplier arm.
- `string_bytes(o, integer)` (`t_string.cc:116-119`) is exactly `getObjectReadOnlyString`: it renders
  an `Enc::Int` value to decimal into a caller-owned `char[24]`. Use it on **both** the read and the
  write path.
- `store_string(sh, key, hash, image, expire, /*integer_encode=*/false)` (`t_string.cc:135`) is the
  single write funnel, carrying embed/extern selection, `obj_bytes` accounting, borrowed-object
  retirement, snapshot hooks and replacement admission. `cmd_setbit` (`t_string.cc:556-597`) is the
  template to copy verbatim, including TTL preservation (`const int64_t expire = o ?
  o->expire_at_ms() : -1`).

Concrete shape of `cmd_bitfield(Shard&, Op&)`:

1. **Parse pass.** Fixed `bitfieldOp ops[16]` on the stack with a heap spill above 16 (a BITFIELD op
   list is unbounded). Track `readonly` and `highest_write_offset` exactly as redis does. Every parse
   error writes a complete RESP error and returns *before* any key access — matching redis, where the
   parse loop precedes the lookup.
2. **Refusal.** If `spec` is `BITFIELD_RO` and `!readonly`, emit the verbatim refusal and return.
3. **Read-only fast path.** `find()`, `obj_type_check(o, Type::String, sink)`, then run the execute
   pass directly against `string_bytes(o, integer)` using the same zero-padded 9-byte window. **No
   allocation, no `store_string`, no key creation.** This path must be taken whenever every op is a
   GET, even under plain `BITFIELD`.
4. **Write path.** One `malloc` of `max(old.n, (highest_write_offset>>3)+1)`, `memcpy` the old bytes,
   `memset` the tail to zero, apply **every** op into that private image, then **one**
   `store_string(...)`. This is strictly cheaper than redis, which re-reads `o->ptr` per op after an
   unshare; tomokv never needs an unshare because the image is private by construction.
5. Replies are appended as the ops are applied, after `reply_array_header(op.sink(), numops)`.

The primitives (`set/get {Signed,Unsigned} Bitfield`, `check{Signed,Unsigned}BitfieldOverflow`) are
~90 lines of pure bit arithmetic, portable as-is and independent of every tomokv structure.

### Knobs and reply formats

**Knobs: none.** BITFIELD has no tunable dimension; under the hardcode-or-delete rule there is
nothing to make adjustable.

Registry rows for `src/cmd/t_string.cc` `kTable[]` (`t_string.cc:1114`):

```
{"BITFIELD",     2, -1, CmdFlags::Write | CmdFlags::DenyOom, cmd_bitfield,    1, 1, 1},
{"BITFIELD_RO",  2, -1, CmdFlags::Readonly,                  cmd_bitfield_ro, 1, 1, 1},
```

`min_arity == 2` is deliberate: `BITFIELD key` with no subops is legal and returns `*0\r\n`.
`DenyOom` on the write form matches redis's static `CMD_DENYOOM`, and tomokv's pre-execution gate
already protects `first_key` (per `NOTES-BITMAP.md`), so argv 1 is correct.

Reply format:

```
*<numops>\r\n
  :<v>\r\n      per GET / SET(old value) / INCRBY(new value)
  $-1\r\n       per op whose OVERFLOW mode is FAIL and which overflowed
```

Built from `reply_array_header` / `reply_int` / `reply_nil` (`src/net/resp.h:159,150,130`). No RESP3
divergence exists for this command.

Error strings, all three verbatim from the oracle (see above). Reuse `reply_err`.

### Build size — **S**

~230–280 lines in `t_string.cc` (90 bit primitives + ~60 parse + ~90 execute + wiring) and two
registry rows. No new file, no scatter lowering, no footprint change.

### Risk

Low — the lowest of the eight. Three real traps:

1. **Int-encoded values on the read path.** Redis renders via `getObjectReadOnlyString`
   (`bitops.c:2081`); tomokv must call `string_bytes(o, integer)`, not `o->str_value()`, or an
   `Enc::Int` object reads garbage. The existing `cmd_getbit` already does this correctly
   (`t_string.cc:606-607`) — copy it, do not re-derive it.
2. **Materialisation on the write path.** `NOTES-BITMAP.md` records that SETBIT materialises an
   int-encoded value even when the selected bit is unchanged. BITFIELD inherits this through
   `store_string(..., integer_encode=false)`; do not add a "nothing changed" short-circuit, because
   the growth-driven `changes` quirk means redis writes in cases where the value did not change.
3. **`#` overflow guard.** `loffset > LLONG_MAX / bits` must be checked *before* the multiply
   (`bitops.c:730-736`).

No shelve criteria — there is no scenario in which this item should be dropped.

### Test plan sketch

Add a `bitfield` suite to `tests/differ.py` (`gens` map, `tests/differ.py:676`). Randomised stream
mixing BITFIELD with SET/APPEND/GETRANGE/SETBIT/GETBIT over shard-spread keys, plus directed cases:

- every width boundary: `u1 u8 u16 u32 u63`, `i1 i8 i16 i32 i64`; and the two rejects `u64`, `i65`;
- `#` notation against each width, including `#0` and a `#` value that trips the overflow guard;
- all three OVERFLOW modes × both directions × signed and unsigned, at the exact type limits;
- OVERFLOW ordering: an `OVERFLOW SAT` mid-list must affect only the ops *after* it;
- SET returns old / INCRBY returns new, verified on a fresh key and an existing one;
- pure-GET BITFIELD on a missing key must return `0` and leave `EXISTS` at 0;
- GET past the end of a short string; GET straddling the end;
- BITFIELD against an int-encoded key (`SET k 12345` then `BITFIELD k GET u8 0`);
- growth: a single `SET u8 #1000000 1` and confirm `STRLEN` matches redis exactly;
- TTL preservation across a BITFIELD write; wrong-type refusal leaving the key untouched;
- `BITFIELD_RO` with GETs only (accepted) and with one SET (refused, verbatim message);
- `BITFIELD key` with no subops.

Then `tests/gate.sh quick` for the footprint locks and no-regression.

---

## 2. GEO family — **BUILD (in two slices)**

### Redis mechanism

File sizes, because they drive the estimate: `geo.c` 1006 lines, `geohash.c` 299, `geohash_helper.c`
280, `geohash.h` 135, `geohash_helper.h` 65, `geo.h` 22. The split is unusually clean:
**`geohash.c` + `geohash_helper.c` (579 lines) are pure math** — they include only their own headers
plus `math.h`, and contain no `robj`, `client` or zset type. `geo.c` is ~100% Redis glue (argv
parsing, zset introspection, RESP assembly, argv rewriting), the only exception being the two qsort
comparators at `geo.c:425-439`.

**Encoding.** Constants `GEO_STEP_MAX 26`, `GEO_LAT_MIN/MAX ±85.05112878`, `GEO_LONG_MIN/MAX ±180`
(`geohash.h:46-52`) — the EPSG:900913 / Web-Mercator limits, noted at `geohash.h:48`.
`interleave64()` `geohash.c:52-77` is the Stanford bithacks `InterleaveBMN` trick with
`B[] = {0x5555…, 0x3333…, 0x0F0F…, 0x00FF…, 0x0000FFFF…}` and `S[] = {1,2,4,8,16}`, finishing
`return x | (y << 1)`. `geohashEncode()` `geohash.c:121-151` normalises lat/long into `[0,1)`,
scales by `1ULL<<step`, and interleaves with **lat as x and long as y**. 26 steps ⇒ a 52-bit
integer, which a `double` mantissa represents exactly — that is the whole reason the score is a
double. `deinterleave64()` `geohash.c:82-110`; `geohashDecode()` `geohash.c:164-194` returns an
**area** (one grid cell), and `geohashDecodeAreaToLongLat()` `geohash.c:206-215` collapses it by
averaging, so **every coordinate Redis reports is the cell centre, not the inserted point**.
`geohash_move_x` / `geohash_move_y` `geohash.c:228-264` step a neighbour without deinterleaving, by
masking the `0xaaaa…` / `0x5555…` half; `geohashNeighbors()` `geohash.c:266-299` composes the 8
compass directions from them.

**GEOADD** `geo.c:445-504`. It recognises only `NX`/`XX`/`CH` (`geo.c:452-459`); `GT`/`LT`/`INCR`
are not in the loop, so those tokens fall through and fail as a longitude. It then performs a
literal **argv rewrite** into a synthetic ZADD (`geo.c:467-503`): a fresh `argv[0] = "zadd"`, the
key and any NX/XX/CH copied verbatim, and per triple a score built by
`geohashEncodeWGS84(x,y,GEO_STEP_MAX,&hash)` → `geohashAlign52Bits(hash)` →
`createStringObjectFromLongLongWithSds(bits)`. `replaceClientCommandVector` (`networking.c:5205`,
re-resolving `c->cmd` at `networking.c:5247`) then `zaddCommand(c)` as a direct call — which is why
GEOADD **propagates as ZADD**.

**GEOSEARCH / GEORADIUS.** One function, `georadiusGeneric()` `geo.c:524-845`, behind six wrappers
(`geo.c:848-873`) selected by the flags at `geo.c:506-514`. Flow:

1. Source/target resolution `geo.c:528-565` — `RADIUS_COORDS` (`base_args=6`), `RADIUS_MEMBER`
   (`base_args=5`, centre from `longLatFromMember` = `zsetScore` + `decodeGeohash`, `geo.c:120-126`),
   `GEOSEARCH`/`GEOSEARCHSTORE` (`base_args=2`/`3`, `storekey = argv[1]` positionally).
2. Option loop `geo.c:573-668` — flat and order-independent.
3. Units, `extractUnitOrReply` `geo.c:134-150`: `m`→1, `km`→1000, `ft`→0.3048, `mi`→1609.34,
   else the verbatim error `unsupported unit provided. please use M, KM, FT, MI`.
4. `geohashCalculateAreasByShapeWGS84()` `geohash_helper.c:121-211`. It computes a spherical bounding
   box (`geohashBoundingBox` `geohash_helper.c:98-116`), derives `radius_meters` (for a box, the
   half-diagonal `sqrt((w/2)²+(h/2)²)`, `geohash_helper.c:138-144`), picks a precision with
   `geohashEstimateStepsByRadius()` `geohash_helper.c:62-83`:
   ```c
   while (range_meters < MERCATOR_MAX) { range_meters *= 2; step++; }
   step -= 2;                                   /* safety margin */
   if (lat > 66 || lat < -66) { step--; if (lat > 80 || lat < -80) step--; }
   ```
   with `MERCATOR_MAX 20037726.37` (`geohash_helper.c:54`), encodes the centre, takes the 8
   neighbours, then does two corrections: an **edge-adequacy retry** that decrements `steps` once and
   recomputes if any of N/S/E/W fails to reach the true bounding-box edge
   (`geohash_helper.c:153-182`), and **neighbour trimming** that `GZERO`s the three boxes on a side
   the centre cell already covers (`geohash_helper.c:184-206`; `GZERO` at `geohash_helper.h:37`,
   detected later by `HASHISZERO` `geohash.h:42`).
5. `scoresOfGeoHashBox()` `geo.c:329-353` turns a `step`-precision prefix into a score interval by
   `*min = geohashAlign52Bits(hash); hash.bits++; *max = geohashAlign52Bits(hash);` — i.e. left-pad
   the prefix to 52 bits, then add one ULP *at that step's granularity*
   (`geohashAlign52Bits` = `bits <<= (52 - step*2)`, `geohash_helper.c:213-217`).
6. `geoGetPointsInRange()` `geo.c:261-324` is a half-open `[min,max)` ZRANGEBYSCORE scan
   (`zrangespec{.minex=0,.maxex=1}`, `geo.c:264`) over either encoding, using primitives exported
   from `t_zset.c` **solely for geo.c** (the extern block and its comment at `geo.c:38-39`:
   `zslValueLteMax` `t_zset.c:436`, `zslNthInRange` `t_zset.c:469`, `zzlFirstInRange`
   `t_zset.c:1078`). `membersOfAllNeighbors()` `geo.c:366-422` walks the fixed 9-box order
   `{self,N,S,E,W,NE,NW,SE,SW}`, skips zeroed boxes (`geo.c:384`), dedups a box identical to the
   previous one (`geo.c:409-416`), and stops at `limit` when `ANY` was given (`geo.c:417`).
7. Filtering, `geoWithinShape()` `geo.c:232-247` → `geohashGetDistanceIfInRadiusWGS84`
   (`geohash_helper.c:252-256`) or `geohashGetDistanceIfInRectangle` (`geohash_helper.c:266-280`).
   *(Note: there is no `…IfInRectangleWGS84`; the rectangle path has no WGS84 wrapper.)* The
   rectangle filter is a two-stage cheap-first rejection: latitude distance first
   (`geohashGetLatDistance` `geohash_helper.c:224-226`), then longitude distance along the parallel,
   and only then the real haversine. `geohashGetDistance()` `geohash_helper.c:229-242` is textbook
   haversine with `EARTH_RADIUS_IN_METERS 6372797.560856` (`geohash_helper.c:52`) plus a
   `v == 0.0` same-longitude fast path.
8. Sorting/COUNT `geo.c:715-753` — `COUNT` without an explicit order forces ASC unless `ANY`
   (`geo.c:719`); a truncating COUNT uses `pqsort` for a partial top-K sort (`geo.c:752-753`) rather
   than a full `qsort`.

**Reply nesting** (`geo.c:757-803`): `option_length` = number of enabled WITH* flags
(`geo.c:762-769`). Outer `*<returned_items>`. If `option_length == 0`, each element is a bare bulk
member. Otherwise each element is an inner array of `option_length+1` in the **fixed order
member → dist → hash → coord**, regardless of the order the flags were typed (`geo.c:786-802`);
`WITHCOORD` is itself a 2-element `[lon,lat]` array. Distances use `addReplyDoubleDistance`
`geo.c:212-216`, which is `fixedpoint_d2string(..., 4)` — **4 fixed decimal places**, not `%.17g`.
`GEOPOS` by contrast uses full-precision `addReplyDouble` (`geo.c:962-964`). The whole family uses
only `addReplyArrayLen`, so **there is no RESP3 shape divergence** in GEO.

**STORE / STOREDIST** `geo.c:804-843` does *not* go through ZADD: it builds a zset directly
(`zslInsert` + `dictAdd`, `geo.c:806-829`), converts back to listpack if it fits (`geo.c:832`), and
`setKey`s it. `score = storedist ? gp->dist : gp->score` (`geo.c:819`) — meaning **a STOREDIST
destination is not a geoset**: its scores are distances, and feeding it to GEOPOS will decode
nonsense. Empty result deletes the destination (`geo.c:837-840`). The reply is always an integer
count (`geo.c:842`), which is why STORE is incompatible with the WITH* flags (`geo.c:672-677`).

**GEOHASH** `geo.c:875-935` is the odd one: the comment at `geo.c:894-899` states that the internal
encoding uses a ±85 latitude range while the *standard* geohash uses ±90, so the command decodes the
score and **re-encodes against `r[1] = {-90, 90}`** (`geo.c:909-915`) before slicing 52 bits into 11
base32 characters from `"0123456789bcdefghjkmnpqrstuvwxyz"` (`geo.c:880`). 52 bits only fill 10
groups of 5; the 11th character is hardcoded `'0'` for API compatibility (`geo.c:919-930`).

Error strings, verbatim: `invalid longitude,latitude pair %f,%f` (`geo.c:110`);
`unsupported unit provided. please use M, KM, FT, MI` (`geo.c:146`); `need numeric radius`
(`geo.c:159`); `radius cannot be negative` (`geo.c:165`); `need numeric width` / `need numeric
height` (`geo.c:186-187`); `height or width cannot be negative` (`geo.c:192`); `COUNT must be > 0`
(`geo.c:593`); `%s is not compatible with WITHDIST, WITHHASH and WITHCOORD options` (`geo.c:673`);
`exactly one of FROMMEMBER or FROMLONLAT can be specified for %s` (`geo.c:680`);
`exactly one of BYRADIUS and BYBOX can be specified for %s` (`geo.c:687`) — note the deliberate
`or`/`and` inconsistency between those two; `the ANY argument requires COUNT argument` (`geo.c:694`);
`could not decode requested zset member` (`geo.c:551`, `:631`); `Unknown georadius search type`
(`geo.c:563`).

### TomoKV design sketch

GEO is a **pure layer over the existing zset lane** — there is no new type, no new encoding, no new
store path, and no change to `KvObj`. That is what makes it cheap despite its apparent size.

**Slice A — the portable core + single-key commands.** Vendor `geohash.c` / `geohash_helper.c`
semantics as one new file `src/cmd/geohash.cc` (~450 lines re-expressed in the tree's C++ idiom;
this is arithmetic, not a port of Redis internals, and carries no licence contamination of the kind
`src/net/resp.h:3-5` warns about). Then `src/cmd/t_geo.cc` implements:

| Command | tomokv lowering |
| --- | --- |
| `GEOADD` | Encode each triple to a 52-bit score, then call the **existing zset add funnel** directly. Do **not** replicate redis's argv rewrite — tomokv has no client command vector and no replication, so an internal call into the same helper `cmd_zadd` uses is both simpler and faster. Reuse the NX/XX/CH decision logic. |
| `GEOPOS`, `GEOHASH` | Single key, read-only. `zsetScore`-equivalent lookup per member, then `decodeGeohash`. |
| `GEODIST` | Single key, read-only, two member lookups + haversine. |
| `GEOSEARCH` (no store) | Single key, read-only. `geohashCalculateAreasByShapeWGS84` gives ≤9 score intervals; each is fed to the **existing** `emit_score_range`-class machinery (`t_zset.cc:1624`) built on `ScoreRange` / `score_gte_min` / `score_lte_max` (`t_zset.cc:92-103`, `parse_score_range` `t_zset.cc:172`), collecting candidates into a bounded vector instead of writing RESP; then filter by distance, sort, truncate, and emit. |
| `GEORADIUS_RO`, `GEORADIUSBYMEMBER_RO` | Same, with the legacy argument order. |

The key structural note: `geo.c` needs `zzlFirstInRange` / `zslNthInRange` exported specially
(`geo.c:38-39`) because Redis's zset internals are private to `t_zset.c`. tomokv has the same
problem in the same shape — the compact and `ZsetData`/Btree walkers live in an anonymous namespace
in `t_zset.cc`. **Put `t_geo` in `t_zset.cc` as an `#include`d `.inc`, following the established
`multi.inc` / `pubsub.inc` / `xshard_commands.inc` pattern**, rather than exporting the zset
internals. That is the single most important design decision in this item and it removes the
awkwardness redis lives with.

**Slice B — the storing forms.** `GEOSEARCHSTORE`, `GEORADIUS ... STORE|STOREDIST`,
`GEORADIUSBYMEMBER ... STORE|STOREDIST` are two-key commands and reuse the **`ZRANGESTORE`
precedent verbatim** (`NOTES-CGAPS.md`, "Scatter-v2 lowering"): hop one gathers the source zset
image on its owner and computes the result set; hop two publishes one destination write loaded
through `zset_snapshot_load`, which runs the ordinary zset add/build path under current compact
thresholds. Empty result erases the destination. When both keys map to one shard the existing
localfast arm runs the same sequence on one owner.

Registry rows (`MultiShard` only for the storing forms):

```
{"GEOADD",             5, -1, Write|DenyOom,             cmd_geoadd,        1, 1, 1},
{"GEOPOS",             2, -1, Readonly,                  cmd_geopos,        1, 1, 1},
{"GEODIST",            4,  5, Readonly,                  cmd_geodist,       1, 1, 1},
{"GEOHASH",            2, -1, Readonly,                  cmd_geohash,       1, 1, 1},
{"GEOSEARCH",          7, -1, Readonly,                  cmd_geosearch,     1, 1, 1},
{"GEORADIUS_RO",       6, -1, Readonly,                  cmd_georadius_ro,  1, 1, 1},
{"GEORADIUSBYMEMBER_RO", 5, -1, Readonly,                cmd_georadiusbym_ro, 1, 1, 1},
{"GEOSEARCHSTORE",     8, -1, Write|DenyOom|MultiShard,  cmd_xshard_only,   1, 2, 1},
{"GEORADIUS",          6, -1, Write|DenyOom|MultiShard,  cmd_xshard_only,   1, 1, 1},
{"GEORADIUSBYMEMBER",  5, -1, Write|DenyOom|MultiShard,  cmd_xshard_only,   1, 1, 1},
```

`GEORADIUS`/`GEORADIUSBYMEMBER` need the same dynamic-destination trick `SORT` already uses
(`NOTES-CGAPS.md`: "the last parsed `STORE` option supplies the optional destination route") because
their second key is a trailing token, not a fixed slot — exactly the reason redis needs
`georadiusGetKeys` (`db.c:3856-3891`). `GEOSEARCHSTORE`'s destination is positional at argv 1, so it
declares `first_key=1, last_key=2` statically.

### Knobs and reply formats

**Knobs: none.** Redis exposes no GEO tunable and neither should tomokv. The step estimator's `-2`
safety margin and the pole corrections are algorithmic constants, not policy — under
hardcode-or-delete they stay hard-coded at the oracle's values.

Reply shapes to match exactly:
- `GEOADD` → `:<added>` (or `:<changed>` under `CH`).
- `GEOPOS` → `*<n>`, each element either `*-1` (null array) or `*2` of two full-precision bulk
  doubles via `reply_double` (`src/net/resp.h:194`).
- `GEODIST` → bulk string with **4 fixed decimals**, or `$-1`. This needs a *new* formatter next to
  `reply_double`: `reply_fixed4(b, v)`. `reply_double` deliberately emits shortest-round-trip
  (`resp.h:195-201`) and would differ from redis on every GEODIST reply.
- `GEOHASH` → `*<n>` of 11-char bulk strings or `$-1`.
- `GEOSEARCH`/`GEORADIUS*` → as described above, with the fixed `member, dist, hash, coord` inner
  ordering.
- Storing forms → `:<count>`.

### Build size — **M** (Slice A) + **S** (Slice B)

Slice A: ~450 lines of portable geohash math + ~550 lines of command layer inside `t_zset.cc` via a
new `geo.inc`. Slice B: ~200 lines reusing the ZRANGESTORE scatter shape. Total ~1200 lines, but
**none of it touches the store, the loops, the footprints, or any cross-thread mechanism** — it is
the largest *volume* of the eight items and among the lowest *risk*.

### Risk

Low-to-moderate, and entirely in numeric fidelity rather than architecture.

1. **Double formatting is the top bug source.** GEODIST/WITHDIST use fixed 4 decimals
   (`geo.c:212-216`); GEOPOS uses full precision. Getting these backwards produces a diff on every
   single reply. Memory records an equivalent trap already paid for once: `reply_double` was moved
   from `%.17g` to shortest-round-trip because the zset differ caught 91 mismatches
   (`src/net/resp.h:195-198`).
2. **Score exactness.** GEOADD must produce a score bit-identical to redis's. Redis routes the
   52-bit integer through a *decimal string* and back into a double (`geo.c:494`,
   `t_zset.c:2035`); a direct `(double)bits` assignment is exact for ≤2⁵³ so the values agree, but
   this must be asserted in the differ, not assumed.
3. **The step estimator and trimming must be copied exactly.** A one-step difference changes which
   candidates are examined and therefore, under `COUNT ... ANY`, which results are returned — `ANY`
   is explicitly "first found in the fixed 9-box scan order", so tomokv's box order must be
   `{self,N,S,E,W,NE,NW,SE,SW}` (`geo.c:371-379`) and its per-box early stop must be at the same
   place (`geo.c:295,319,417`).
4. `decodeGeohash` (`geo.c:92-95`) assumes step 26 for every score. A STOREDIST destination violates
   that. Match redis's behaviour (garbage in, garbage out) rather than "fixing" it — a fix is a diff.

**Shelve criteria:** none for Slice A. Slice B may be shelved if the `SORT STORE`-style dynamic
destination routing turns out to require a registry change; in that case ship the `_RO` forms and
`GEOSEARCH` and document the storing forms as unsupported, which still covers the majority of client
usage.

### Test plan sketch

New `geo` suite in `tests/differ.py`. Because GEO is float-heavy, the suite must diff **reply bytes**,
which the existing harness already does — that is exactly the discrimination needed.

- Randomised: GEOADD of N points drawn from a fixed grid plus randomised jitter, interleaved with
  GEOSEARCH BYRADIUS / BYBOX at randomised centres and radii, plus ZSCORE/ZRANGE on the same key to
  prove the geoset is an ordinary zset.
- Directed: the ±85.05112878 latitude boundary and just past it (must error); ±180 longitude and the
  antimeridian; both poles; a radius that spans the whole world (forces `step=1` and the box dedup at
  `geo.c:409-416`); a radius small enough to force `step=26`; latitudes 66/80 exactly, to hit both
  pole corrections (`geohash_helper.c:70-73`); a search whose edge-adequacy check fires the
  `steps--` retry.
- Every WITH* flag combination (2³ = 8) in several typed orders, asserting the fixed reply ordering.
- `COUNT n`, `COUNT n ANY`, `ASC`/`DESC`, and `COUNT` with no order (forced ASC).
- GEODIST in all four units and against a missing key/member; GEOHASH against known fixtures
  (an 11-char string ending in `0`).
- GEOADD `NX`/`XX`/`CH` and the `XX and NX` conflict.
- STORE/STOREDIST: non-empty, empty (destination deleted), destination of a different type,
  and a STOREDIST destination fed back to GEOPOS (must reproduce redis's nonsense identically).
- Cross-shard: destination and source deliberately placed on different shards.

Then `tests/gate.sh quick`.

---

## 3. HEXPIRE family (per-field hash TTLs) — **SHELVE. Design recorded; criteria below.**

This is the riskiest item of the eight and the only one I recommend against building now. The
mechanism section is long because the shelve argument only carries weight if the thing being
shelved is fully understood.

### Redis mechanism

**Provenance warning.** This oracle tree has evolved well past the historical 7.4 HFE design. Hashes
no longer store plain `sds` fields in a `dict`; there is a merged `kvobj` model, a fused
field+value+TTL `Entry` type (`entry.c` / `entry.h`), a cluster-slot-aware expiry index called
**`estore`** — *not* a bare `ebuckets *hexpires` on `redisDb` — and a fourth, unrelated hash encoding
family (`OBJ_ENCODING_TMPL_LP`=14 / `TMPL_ARRAY`=15, `object.h:89-90`). Names below are what is
actually in the tree.

**A. Storage forms.** Encodings at `object.h:77,86,87`: `HT`=2, `LISTPACK`=11, `LISTPACK_EX`=12.

1. **`LISTPACK`** — plain pairs, structurally incapable of carrying a TTL:
   `hashTypeGetFromListpack()` always returns `EB_EXPIRE_TIME_INVALID` for it
   (`t_hash.c:1632-1645`).
2. **`LISTPACK_EX`** (`server.h:3964-3971`):
   ```c
   typedef struct listpackEx {
       ExpireMeta meta;  /* registers the hash in the global subexpires estore */
       void *lp;         /* listpack of field-value-ttl TRIPLETS, ordered by ttl */
   } listpackEx;
   ```
   Conversion inserts a TTL slot after every value (`hashTypeConvertListpack()`,
   `t_hash.c:3016-3032`), so from then on every field costs three listpack entries. The listpack is
   kept **sorted by TTL with the no-TTL fields at the tail** — the design comment is at
   `t_hash.c:1244-1256` — and the sort is maintained by a **linear `lpFindCb` scan** per insert
   (`cbFindInListpack` `t_hash.c:1290-1310`, `listpackExAddInternal` `t_hash.c:1411-1434`), i.e.
   O(n) per write, not a real ordered structure.
   **Byte costs, exactly:** a no-TTL field's `HASH_LP_NO_TTL`=0 encodes as
   `LP_ENCODING_7BIT_UINT_ENTRY_SIZE` = **2 bytes** (`listpack.c:37`); a real epoch-ms TTL needs
   `LP_ENCODING_64BIT_INT_ENTRY_SIZE` = **10 bytes** (`listpack.c:70`).
3. **`HT` with per-field TTL.** There is no separate TTL dict and no `hashTypeEntry`. The dict stores
   `Entry*` as keys with `no_value=1` (`entryHashDictType` / `entryHashDictTypeWithHFE`,
   `t_hash.c:87-111`), and `Entry` is one allocation where `(Entry*) == (sds field)`
   (`entry.h:130-134`). **Whether a field has a TTL is a bit in the field's own sds header** —
   `FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY = 0` (`entry.c:27`), read by `entryHasExpiry()` /
   `entryGetExpiry()` (`entry.c:48-97`) — and when set, an `ExpireMeta` is **prepended** to the
   allocation (`entry.c:81,117,145`).
   **This is the number that matters for the tomokv comparison: a TTL-less field in HT form costs
   ZERO extra bytes.** A field with a TTL costs `sizeof(ExpireMeta)` = **24 bytes**
   (`ebuckets.h:162-212`: 4 + 2 + 4 + 8 with padding, no `#pragma pack`).
   Per-*hash* there is a one-time upgrade: `htMetadataEx` (`server.h:4044-4052`) is
   `size_t alloc_size` + `ExpireMeta` + `ebuckets hfe` = **40 bytes** versus `sizeof(size_t)` = 8
   for a TTL-less hash dict (`hashDictMetadataBytes()` `t_hash.c:252-262`), so **+32 bytes once** on
   the first TTL. The discriminator is literally the dictType pointer:
   `isDictWithMetaHFE(d)` tests `d->type == &entryHashDictTypeWithHFE` (`t_hash.c:161-163`).

   Conversions: `LISTPACK → LISTPACK_EX` happens lazily on the first HFE command touching the hash
   (`hashTypeSetExInit()` `t_hash.c:2292-2332`, the conversion at `:2305-2306`); `LISTPACK_EX → HT`
   uses the same `hash-max-listpack-entries` (512, `config.c:3482`) and `hash-max-listpack-value`
   (64, `config.c:3494`) thresholds as plain hashes, re-checked after every write
   (`t_hash.c:1992-1993`), plus an `lpSafeToAdd()` byte budget that includes the TTL integer's own
   width (`t_hash.c:3468`). `hashTypeConvertListpackEx()` (`t_hash.c:3067-3124`) must deregister the
   hash from `db->subexpires` first (`:3080-3084`) because the old `ExpireMeta` is about to be freed,
   and re-register after (`:3119-3120`). **HT never converts back down.**

**B. `ebuckets` and `estore`.** `ebuckets.c` is 2725 lines (615 of them `REDIS_TEST`),
`ebuckets.h` 336. It is a hybrid: a plain singly-linked list while small, and a **rax keyed by
coarsened expiry time** when large, where each rax leaf points not to one item but to a **segment**
of up to `EB_SEG_MAX_ITEMS = 16` items (`ebuckets.c:64`) chained through the `next` pointer that
already lives inside each item's `ExpireMeta`. So intra-bucket chaining costs **no additional
allocation** beyond the 24 bytes already paid. Documented amortised cost: ~40 bytes per rax leaf
across up to 16 items (`ebuckets.h:20-22`). Bucket key is
`EB_BUCKET_KEY(t) = t >> EB_BUCKET_KEY_PRECISION` with precision 0 in this build
(`ebuckets.h:143-146`), so buckets are per-millisecond. API: `ebAdd` `:1426-1455`,
`ebRemove` `:1392-1410`, `ebExpire` `:1466-1551`, `ebGetNextTimeToExpire` `:1665-1702`,
`ebExpireDryRun` `:1563-1650`.

The global index is `redisDb::subexpires`, an `estore*` (`server.h:1234`) — **an array of
`ebuckets`, one per cluster hash-slot**, plus a Fenwick tree for O(log n) "next non-empty slot"
(`estore.c:20-28`, `estoreGetFirstNonEmptyBucket` / `estoreGetNextNonEmptyBucket` `:86-99`). The
per-slot array exists so cluster resharding can move a slot's TTL'd hashes in O(1)
(`estoreMoveEbuckets()` `estore.c:198-215`). Each hash also carries its **own private** `hfe`
ebuckets of its fields (`htMetadataEx.hfe`), so the structure is two-level: db → hash (by min
expiry) → field (by expiry).

**C. Commands.** Twelve, including `HSETEX` (8.0). `HEXPIRE`/`HPEXPIRE`/`HEXPIREAT`/`HPEXPIREAT`
are one-line wrappers (`t_hash.c:6425-6442`) over `hexpireGenericCommand()` `t_hash.c:6297`;
`HTTL`/`HPTTL`/`HEXPIRETIME`/`HPEXPIRETIME` over `httlGenericCommand()` `t_hash.c:6119`;
`HPERSIST` `t_hash.c:6466`; `HGETEX` `t_hash.c:5085`; `HGETDEL` `t_hash.c:4961`;
`HSETEX` `t_hash.c:4586`.

Syntax is uniformly `<cmd> key [args] FIELDS numfields field [field ...]`, parsed by
`parseHashCommandArgs()` (`t_hash.c:6036-6116`), which is keyword-order-flexible and rejects more
than one condition flag via `__builtin_popcount(...) > 1`.

**Reply constants, verbatim** — these are the contract:

```c
typedef enum SetExRes {                    /* t_hash.c:180-186 */
    HSETEX_OK =                1,   /* Expiration time set/updated as expected */
    HSETEX_NO_FIELD =         -2,   /* No such hash-field */
    HSETEX_NO_CONDITION_MET =  0,   /* Specified NX | XX | GT | LT condition not met */
    HSETEX_DELETED =           2,   /* Field deleted because the specified time is in the past */
} SetExRes;

typedef enum GetExpireTimeRes {            /* t_hash.c:188-192 */
    HFE_GET_NO_FIELD =        -2,   /* No such hash-field */
    HFE_GET_NO_TTL =          -1,   /* No TTL attached to the field */
} GetExpireTimeRes;

typedef enum SetPersistRes {               /* t_hash.c:154-159 */
    HFE_PERSIST_NO_FIELD =    -2,
    HFE_PERSIST_NO_TTL =      -1,
    HFE_PERSIST_OK =           1
} SetPersistRes;
```

Condition semantics, identical in both encodings (`t_hash.c:1495-1546` listpack,
`:2156-2241` HT): with **no** previous TTL, `XX` and `GT` fail while `NX`, `LT` and no-flag
succeed — the comment *"For fields without expiry, LT condition is considered valid"* is at
`t_hash.c:1518` and `:2181`. With a previous TTL, the condition fails when
`(GT && prev >= new) || (LT && prev <= new) || NX`.

Bounds: `HFE_MAX_ABS_TIME_MSEC = EB_EXPIRE_TIME_MAX >> 2` (`t_hash.c:36`), where
`EB_EXPIRE_TIME_MAX = 0x0000FFFFFFFFFFFF` (`ebuckets.h:149-150`) — two bits are reserved for a
future indexing trick (comment `t_hash.c:24-33`).

Error strings, verbatim: `FIELDS keyword specified multiple times` (`t_hash.c:6054`);
`FIELDS requires at least numfields and one field argument` (`:6059`);
`wrong number of arguments` (`:6074`); `missing FIELDS argument` (`:6106`);
`unknown argument: %s` (`:6100`); `Multiple condition flags specified` (`:6111`);
`Mandatory argument FIELDS is missing or not at the right position` (`:6130`);
`Number of fields must be a positive integer` (`:6136`);
`` The `numfields` parameter must match the number of arguments `` (`:6141`);
`Only one of EX, PX, EXAT, PXAT or PERSIST arguments can be specified` (`:4569`);
`Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified` (`:4566-4570`);
`invalid expire time in '%s' command` (`networking.c:894-897`).

`HTTL` reports a logically-expired-but-not-yet-reaped field as **`-2` (no field)**, not as its
residual TTL (`t_hash.c:6214-6217`, `:6247-6250`). Seconds are computed with a ceiling:
`(expire + 999 - basetime) / 1000` (`:6219-6222`).

**D. Expiry execution.** Three paths:
- **Lazy, on every read**: `hashTypeGetValue()` `t_hash.c:1713-1820`. On a hit past its deadline it
  deletes the field, calls `propagateHashFieldDeletion()`, bumps `stat_expired_subkeys`, emits
  `"hexpired"` (`:1808`), and **if that was the last field, `dbDelete()`s the key** and emits
  `NOTIFY_GENERIC "del"` (`:1810-1816`). Replicas and `CLIENT_MASTER` context report expired but do
  **not** delete — they wait for the master's explicit `HDEL`. A whole family of `HFE_LAZY_*` flags
  (`server.h:4069-4078`) lets individual call sites suppress notification / signalling / hash
  deletion.
- **Active**: `activeSubexpiresCycle()` `expire.c:228-285`, called from `activeExpireCycle()` at
  `expire.c:387` **before** whole-key expiry (comment: *"ebuckets is optimized for active
  expiration"*). It picks the next non-empty slot via the Fenwick tree, self-throttles against
  `HFE_DB_BASE_ACTIVE_EXPIRE_FIELDS_PER_SEC / server.hz` with up to a 32× boost
  (`expire.c:102,263-272`), and per hash calls `hashTypeExpire()` `t_hash.c:3683-3758`, whose
  callback `onFieldExpire()` (`:5974-6002`) deletes the field and propagates `HDEL`. The callback
  returns `ACT_REMOVE_EXP_ITEM` or `ACT_UPDATE_EXP_ITEM`, letting `ebExpire()` re-slot the hash in
  the same pass.
- **Notifications**, and the distinction is easy to get wrong: **`"hexpire"`** = a TTL was set or
  updated (`t_hash.c:6371`, `:4730`, `:5194`); **`"hexpired"`** = a field actually expired
  (`:1808`, `:3722`, `:4947`, `:5047`); `"hpersist"` (`:6621`, `:5184`); plus `NOTIFY_GENERIC "del"`
  when the last field goes.
- **Propagation**: field deletions propagate a standalone **`HDEL key field`**
  (`propagateHashFieldDeletion()` `t_hash.c:5952-5971`), *not* an `HPEXPIREAT`. The HEXPIRE family
  propagates its own effect rewritten to canonical `HPEXPIREAT key <abs-ms> FIELDS n field...`
  (`t_hash.c:6399-6405`), with failed fields stripped and `preventCommandPropagation()` if none
  succeeded (`:6389-6395`). `HGETDEL` rewrites itself to `HDEL` (`t_hash.c:5058-5060`); `HGETEX`
  rewrites to `HPERSIST` or `HPEXPIREAT` (`:5177-5214`, comment *"This command will never be
  propagated as it is."*). AOF rewrite emits one `HMSET` per field plus one
  `HPEXPIREAT key t FIELDS 1 field` per TTL'd field (`aof.c:2460-2480`).

**E. Interactions.**
- **Whole-key `EXPIRE` is fully orthogonal** — whole-key TTLs live in `db->expires`, field TTLs in
  `db->subexpires`; deleting the key removes it from both (`db.c:886-888`).
- **`RENAME`/`MOVE`** keep the TTLs but re-register the slot, because the slot is a function of the
  key name (`db.c:2309-2327`, `:2405-2424`).
- **`COPY`** deep-duplicates: `hashTypeDup()` `t_hash.c:3489-3596` `memcpy`s the listpack blob for
  `LISTPACK_EX` (`:3508-3512`) or re-creates each entry with `ENTRY_HAS_EXPIRY` and re-`ebAdd`s for
  HT (`:3552-3560`).
- **`HSET` on a field that had a TTL clears it.** `hashTypeSet()` only preserves the TTL when the
  caller passes `HASH_SET_KEEP_TTL` (`t_hash.c:1912`), and `hsetCommand` passes `HASH_SET_COPY`,
  which is **0** (`t_hash.c:4025,4066`). The strip is at `:1978-1983` (listpack) and `:2020-2029`
  (HT). `HINCRBY`/`HINCRBYFLOAT` are the exceptions — they pass `KEEP_TTL` explicitly
  (`:4802,4866`), matching tomokv's existing behaviour of preserving the *key* TTL on HINCRBY
  (`NOTES-HASH.md`, tricky test 6).

**F. Persistence.** `rdb.h:59-90`: `RDB_TYPE_HASH_METADATA_PRE_GA`=22,
`RDB_TYPE_HASH_LISTPACK_EX_PRE_GA`=23, `RDB_TYPE_HASH_METADATA`=24,
`RDB_TYPE_HASH_LISTPACK_EX`=25. The METADATA layout (`rdb.c:1360-1427`) is
`[minExpire 8B][numfields][per field: ttl (0 = none, else abs - minExpire + 1), field, value]`; the
LISTPACK_EX layout (`rdb.c:1343-1359`) is `[minExpire 8B][listpack blob with triples]`, where the
saved `minExpire` is read and then **discarded** on load (`UNUSED(minExpire)`, `rdb.c:3606`) because
the blob is authoritative. Global registration happens after the key lands
(`rdb.c:5014-5020`).

**G. Size, honestly.** Dedicated files: `ebuckets.c` 2725 (615 test), `ebuckets.h` 336,
`estore.c` 498 (282 test), `estore.h` 91, `entry.c` 408, `entry.h` 141 — **4199 lines, 897 of them
tests**. Inside `t_hash.c` (6627 lines total): ~2,112 lines are functions whose entire body is HFE,
measured by summing exact function spans, plus an estimated 400–450 more threaded through shared
storage functions — **~2,500–2,600 lines, roughly 38–40% of the file**. Elsewhere: `expire.c` ~140,
`rdb.c` ~250, `db.c` ~70, `defrag.c` ~90, `aof.c` ~20, misc ~60. **Whole feature ≈ 6,500–7,400
lines of C.**

### How this fits OUR hash forms — the honest assessment

TomoKV has **three** hash representations, not Redis's two-plus-templates, and they differ in how
much room they have for a per-field deadline.

**Form 1 — resident / embedded compact (the mdiet lane).** Layout
(`NOTES-MDIET.md`): `[KvObj 8][long-key len 4?][TTL 8?][key K][metadata 32][Compact bytes E][slack]`.
Two facts make this the binding constraint:

- **The 32-byte metadata tail is fully spoken for.** Its two general-purpose words are
  `aux0` and `aux1` (`src/store/kvobj.h:152-159`), and **hash uses both**: aux0 holds the logical
  field/value byte total and aux1 holds the PRNG state
  (`NOTES-MDIET.md` metadata table; `HashVal::compact_payload_bytes` and `HashVal::random_state`,
  `src/store/typeval.h:656-657`). **There is no free word for a cached min-expire.**
- **The embedded ceiling is 192 bytes of encoded Compact** (`NOTES-MDIET.md`, "Embed capacity
  math"). The measured five-field probe encodes to `E = 30` and lands in an **80-byte** size class —
  a 68.8% reduction against the previous 256 bytes. Adding an 8-byte absolute-ms deadline per field
  takes that `E` from 30 to 70 and pushes the object into a 112- or 128-byte class. A hash that fits
  today at 12–15 fields would no longer fit and would be forced out through the documented
  "migrates to the old external compact wrapper" path.

So **the first `HEXPIRE` on a resident hash evicts it from the one-allocation form.** That is not a
bug in the design — it is the correct behaviour given the ceiling — but it means HFE is in direct
tension with the lane that just delivered mdiet's footprint win, on exactly the workload mdiet was
built for.

**Form 2 — external compact.** Tractable. The current pair encoding is one outer `Compact` entry per
field, `[ULEB outer len][ULEB field len][field bytes][value bytes]` (`NOTES-HASH.md`, "Compact
encoding"). Extend it with an **object-level** flag rather than a per-entry one: when the hash has
any field TTL, every entry carries a fixed 8-byte deadline; when it has none, the encoding is
byte-identical to today. Setting the first field TTL rewrites the whole compact once — O(n) bounded
by `hash-max-compact-entries` (512), which is the same bound `NOTES-HASH.md` already accepts for
compact scans. This beats Redis's `LISTPACK_EX`, which charges 2 bytes to *every* field forever
(`listpack.c:37`).

**Form 3 — expanded `HashFieldMap`.** Also tractable, but the naive version is wrong.
`HashFieldMap::Node` is `{uint64_t hash; std::string field; std::string value;}`
(`src/cmd/t_hash.cc:36-42`) — 72 bytes with libstdc++'s 32-byte `std::string`. Adding an `int64_t
expire_at` makes it 80 and charges **+8 bytes to every field of every hash**, including the
overwhelming majority that never use a TTL. Redis charges **zero** for a TTL-less HT field
(`entry.c:27`), so the naive version is strictly worse than the oracle on the common case, against a
tree whose memory position is 109 B/key vs dfly's 85 and Redis's 105.
Correct shape: a **side map allocated only when the hash has ≥1 field TTL**, keyed by dense node
index — `std::unordered_map<uint32_t,int64_t>` or, better, a sorted `std::vector<std::pair<uint32_t,
int64_t>>` since HFE hashes are small in practice. A TTL-less hash then pays **nothing**, and an
HFE hash pays one allocation plus ~16 bytes per TTL'd field. Do **not** steal a bit from
`Node::hash`: it is the seeded full field hash used both for bucket indexing and for comparison
(`NOTES-HASH.md`, expanded map section), and masking it at every site is exactly the kind of
invariant that rots.

**The global index.** Redis's answer is `estore` + `ebuckets` — ~3,300 lines of production code,
most of which exists to make active expiry cheap **across cluster slots**. TomoKV has no cluster
slots, and already has a per-shard whole-key expiry sampler:
`FlatStore::active_expire(budget)` (`src/store/flatstore.h:629-651`) driven by
`ExLoop::active_expire_cycle()` with `kActiveExpireChecks = 20` per pass
(`src/core/ex_loop.h:41,157-174`). A second, differently-keyed per-shard structure is needed —
"which hash objects have a field due soonest" — but it can be **one min-heap of
`(min_expire_ms, key_hash)` per shard**, popped by an `active_field_expire_cycle` that resolves the
object through the same `find_hash_in` path `active_expire` already uses
(`flatstore.h:635-637`), expires the due fields, and re-pushes with the new minimum. That is
~250–350 lines, not 3,300 — the single genuine simplification tomokv gets for free.

But the min-heap needs the **cached per-object minimum**, and Form 1 has nowhere to put it. Which
gives the design its one clean rule:

> **Field TTLs are supported only on the external and expanded forms. Setting the first field TTL on
> a resident hash migrates it out, and the cached min-expire lives in the heap-side `HashVal`
> (`src/store/typeval.h:645-659`), which can grow freely.**

That rule keeps mdiet's footprint intact for every hash that never uses HFE, which is almost all of
them, and it makes the always-on lazy check a test of a byte the handler has already loaded.

### Knobs and reply formats (if built)

```
--hash-field-expire N   0 = off: the family is not registered, no per-shard heap is allocated,
                            and the hash read path is byte-identical to today
                        N = active field-expiry budget (fields examined per owner pass)
                       -1 = auto (derive from kActiveExpireChecks)
```

Twelve registry rows in `src/cmd/t_hash.cc` `kTable[]` (`t_hash.cc:1506`), all single-key at argv 1,
all `Write` except the four `HTTL`-family and `HGETEX` without a modifier:

```
{"HEXPIRE",      6, -1, Write, cmd_hexpire,      1, 1, 1},   ... HPEXPIRE, HEXPIREAT, HPEXPIREAT
{"HTTL",         5, -1, Readonly, cmd_httl,      1, 1, 1},   ... HPTTL, HEXPIRETIME, HPEXPIRETIME
{"HPERSIST",     5, -1, Write, cmd_hpersist,     1, 1, 1},
{"HGETEX",       5, -1, Write, cmd_hgetex,       1, 1, 1},
{"HGETDEL",      5, -1, Write, cmd_hgetdel,      1, 1, 1},
{"HSETEX",       6, -1, Write | DenyOom, cmd_hsetex, 1, 1, 1},
```

Replies are arrays of the integer constants quoted verbatim above, one per requested field —
`*<numfields>` then `:1` / `:0` / `:-1` / `:-2` / `:2`. `HGETEX`/`HGETDEL` reply arrays of bulk or
`$-1`. `HSETEX` replies `:0` or `:1`.

### Build size — **L**

Honest per-part estimate for tomokv:

| Part | Lines |
| --- | ---: |
| Compact pair encoding + object-level "has field TTLs" flag + the one-time rewrite | ~250 |
| `HashFieldMap` side TTL map and its accounting hooks | ~200 |
| Lazy expiry threaded through **every** hash read site (`compact_find` `t_hash.cc:555`, `HashFieldMap::find` `:96`, `decode_pair` `:533`, `generic_getall`, `cmd_hscan` `:1299`, `cmd_hrandfield` `:1402`) | ~300 |
| Per-shard min-expire heap + `active_field_expire_cycle` + the ex-loop hook | ~350 |
| The twelve commands, the NX/XX/GT/LT matrix, and the exact reply codes | ~700 |
| Snapshot hooks — the hash format at `t_hash.cc:1545-1621` must carry TTLs, and `kSnapshotFormatVersion` (`src/snapshot/format.h:21`) must bump | ~150 |
| Interactions: HSET clears TTL, COPY, RENAME, last-field key delete, `ObjectSizeTracker` bracketing | ~200 |
| **Total** | **~2,150** |

Plus the mdiet migration path. This is solidly **L**, and it is the largest item of the eight by a
wide margin.

### Risk

Highest of the eight, for five distinct reasons.

1. **Lazy expiry mutates a collection from inside a read path.** `NOTES-HASH.md` states the standing
   invariant: *"A compact mutation invalidates all earlier pair slices, and handlers retain none
   across a mutation."* HFE makes every hash **read** a potential mutation. Every site holding a
   `PairView` or a `HashFieldMap::Node*` across what is now a possible deletion becomes a
   use-after-free. This is precisely the bug class this tree has paid for repeatedly — the worker
   argv refcount race, the `SET..EX` UAF, the fence `qb_pos` crash. It is not hypothetical.
2. **Direct tension with the mdiet lane.** See Form 1 above. The mitigation (migrate out on first
   HEXPIRE) is correct but means the feature and the footprint win are mutually exclusive per object.
3. **Accounting.** The `obj_bytes` bracket contract (finish-before-erase) must now bracket a lazy
   deletion that happens inside a read. Hash reads are not `SnapshotWrite`-flagged and are not
   maxmemory-admitted; both facts become false the moment a read can delete.
4. **Snapshot format break.** `kSnapshotFormatVersion` must bump, which makes existing dumps —
   including the eight `dump-2026-08-26T01:54:33-*.dfs` files sitting in the repo root — unloadable
   without a version-tolerant loader.
5. **Always-on cost on the hottest hash path.** The lazy check runs for every `HGET` in the server,
   for a feature most hashes never use. The object-level flag makes it near-free, but "near-free" is
   a claim that has to be measured against the ≤3% always-on rule, not asserted.

One thing in its favour: **HFE is entirely single-key.** No scatter, no cross-shard lowering, no
atomicity question. Whatever else it is, it is not a distributed problem.

### Shelve criteria — the concrete test

**Recommendation: shelve.** Build it only if **all five** hold:

1. **A named consumer needs per-field TTLs.** The genuine ones are session stores and rate limiters.
   Absent one, the workaround already works and is cheap: one key per field with an ordinary TTL, at
   ~109 B/key, using machinery that is already shipped and already tested.
2. **The owner accepts that a resident (mdiet) hash migrates out of the one-allocation form on its
   first `HEXPIRE`** — i.e. accepts losing the 68.8% small-hash footprint win for HFE-using hashes.
   If the answer is "field TTLs must not cost the mdiet win", the item is not buildable as designed
   and must be redesigned first.
3. **The lazy-expiry-inside-a-read audit is done BEFORE any code is written.** Enumerate every site
   in `t_hash.cc` that holds a `PairView`, a `Compact::Entry`, or a `HashFieldMap::Node*` across a
   call that could now delete, and fix the lifetime contract first. This is the standing
   "verify before implementing — grep the CODE first" rule, and it is the difference between an L
   item and an L item plus a week of UAF hunting.
4. **The snapshot version bump is accepted** and a version-tolerant loader exists, so an old dump
   still loads.
5. **Budget is set at L (~2,000+ lines) with a three-regime A/B whose benefit arm is
   hashes-with-no-TTLs showing zero regression.** That arm is the one that matters, because it is
   the overwhelming majority of traffic. A measurable regression there sinks the feature regardless
   of how well the TTL path works.

If any of the five fails, record the design above in `NOTES-HASH.md` as a designed-not-built lane —
the same treatment `thredis-shared-kv-never-built` got — so the analysis is not re-done from scratch
later.

### Test plan sketch (if built)

- New `hfe` suite in `tests/differ.py`. Field TTLs are wall-clock dependent, so the suite must use
  **absolute** far-future deadlines for the deterministic arm (`HPEXPIREAT` with a fixed timestamp)
  and confine the actually-expiring cases to a directed, sleep-based battery. Diffing relative TTL
  replies against a live oracle is a flake generator; do not.
- Deterministic differential arm: the full NX/XX/GT/LT matrix × {field has TTL, field has no TTL,
  field absent, key absent} — 16 cells, asserting the exact `-2 / 0 / 1 / 2` codes — plus every
  verbatim parse error above, `HTTL`/`HPTTL`/`HEXPIRETIME`/`HPEXPIRETIME` on all four field states,
  `HPERSIST` on all three, `HGETEX` with each of `EX/PX/EXAT/PXAT/PERSIST` and their mutual
  exclusion, `HSETEX` with `FNX`/`FXX`/`KEEPTTL`, and `HGETDEL`.
- Directed timing battery (`tests/hfe.py`): a past deadline deletes the field and replies `2`; the
  last field expiring deletes the **key** (`TYPE` becomes `none`); lazy expiry on `HGET`, `HGETALL`,
  `HSCAN`, `HRANDFIELD`, `HLEN` — and **`HLEN` is the subtle one**, since Redis's length must not
  count logically-expired fields.
- Interaction: `HSET` on a TTL'd field clears the TTL; `HINCRBY` preserves it; whole-key `EXPIRE`
  coexists with field TTLs; `COPY` duplicates them; `RENAME` preserves them; `DEL` removes both
  registrations.
- Encoding crossings, in both directions across every threshold: resident → external on the first
  `HEXPIRE` (assert `used_memory` moves by the predicted amount, not merely that it changed);
  external compact → expanded with TTLs live; and `OBJECT ENCODING` at each step.
- **Mechanism-fired proof** (the vacuous-validation rule): ship an
  `expired_subfields` counter and assert it is nonzero after the active-expiry battery. A test where
  every field happened to be reaped lazily proves nothing about the active cycle.
- Snapshot round-trip through `tests/snap_typed_roundtrip.py` with TTLs live, plus loading an
  old-version dump.
- ASAN build plus the whole battery, specifically targeting risk 1 — the read-path deletion.
- Three-regime A/B: hashes with no TTLs (must be zero-regression), hashes with all fields TTL'd, and
  a 10%-TTL'd mix.

---


---

## 4. FUNCTION family — **DECIDE: build the persistent-state version, or decline. Do not build the cheap one.**

### Redis mechanism

`functions.c` 1138 lines, `functions.h` 127, `function_lua.c` 513, `script.c` 712,
`script_lua.c` 1767, `eval.c` 1762.

**Library model.** Three layers, all declared in `functions.h` because `rdb.c` needs them:
`engine` is a vtable (`create`, `call`, `get_used_memory`, `get_function_memory_overhead`,
`get_engine_memory_overhead`, `free_function`, `free_ctx`) at `functions.h:37-74` — Lua is its only
implementor (`luaEngineInitEngine` `function_lua.c:428-513`). `functionInfo`
(`functions.h:86-93`) holds `name`, an opaque `void *function` (for Lua a `luaFunctionCtx*` wrapping
a Lua registry ref), the owning `functionLibInfo*`, a description and `f_flags`. `functionLibInfo`
(`functions.h:97-102`) holds `name`, its own `functions` dict, the engine, and `sds code` — the
**full original source including the shebang**, retained for `LIST WITHCODE` and RDB.
`functionsLibCtx` (`functions.c:36-41`) is the global container and carries **two** dicts: `libraries`
and a **flat cross-library `functions` index**, which is what makes function names globally unique.
Every function is linked into both by `libraryLink` (`functions.c:304-323`) and unlinked by
`libraryUnlink` (`:281-302`); a cross-library name collision is rejected at `functions.c:1011-1016`.
The single global instance is `static functionsLibCtx *curr_functions_lib_ctx` (`functions.c:103`);
`FUNCTION FLUSH` and `RESTORE FLUSH` swap the whole pointer (`functionsLibCtxSwapWithCurrent`,
`functions.c:206-209`).

**Registration.** `functionExtractLibMetaData()` `functions.c:897-954` parses the `#!<engine>
name=<lib>` shebang: must start with `#!` (`functions.c:901`, error `Missing library metadata`),
must have a newline (`:906`, `Invalid library metadata`), engine name is `parts[0]` minus `#!`
(`:920`), and `md->code` starts at the newline so **line numbers in errors stay aligned** (`:944`).
Errors verbatim: `Invalid metadata value given: %s` (`:932`),
`Invalid metadata value, name argument was given multiple times` (`:925`),
`Library name was not given` (`:937`), `Engine '%S' not found` (`:981`),
`Library '%S' already exists` (`:989`), `No functions registered` (`:1003`).

`luaEngineCreate()` `function_lua.c:88-142` compiles with `luaL_loadbuffer` and then **runs the
chunk once** (`lua_pcall(lua,0,0,0)`, `function_lua.c:118`) under a 500 ms watchdog
(`LOAD_TIMEOUT_MS` `functions.c:16`; hook `luaEngineLoadHook` `function_lua.c:68-79`, error
`FUNCTION LOAD timeout` `:76`). The chunk's only job is to call `redis.register_function`.

`redis.register_function` = `luaRegisterFunction` `function_lua.c:403-425`, callable **only** during
LOAD (`function_lua.c:406-410`, error `redis.register_function can only be called on FUNCTION LOAD
command`). Two forms, dispatched on `lua_gettop` (`function_lua.c:389-401`): positional
`(name, callback)` (`:358-387`), and the table form with `function_name` / `callback` / `flags` /
`description` (`:276-356`; unknown key → `unknown argument given to redis.register_function` `:327`).
Flags are validated against `scripts_flags_def` `script.c:23-30`:
`no-writes`, `allow-oom`, `allow-stale`, `no-cluster`, `allow-cross-slot-keys`.
**The callback is stored as an integer Lua-registry handle**: `luaL_ref(lua, LUA_REGISTRYINDEX)`
(`function_lua.c:373` positional / `:311` table) into `luaFunctionCtx.lua_function_ref`
(`function_lua.c:46-49`). Invocation later does `lua_rawgeti(lua, LUA_REGISTRYINDEX, ref)`
(`function_lua.c:163`); deletion does `lua_unref` (`function_lua.c:190`).

**Sandboxing — this is not metatable trickery.** Redis ships a *patched* Lua with a real `readonly`
bit on the `Table` struct (`deps/lua/src/lobject.h:341`), set through
`lua_enablereadonlytable` (`deps/lua/src/lapi.c:1094`) and enforced inside the VM itself:
`if (h->readonly) luaG_runerror(L, "Attempt to modify a readonly table");`
(`deps/lua/src/lvm.c:141-142`). The prototype `luaRegisterGlobalProtectionFunction`
(`script_lua.h:50`) is **dead — it has no definition and no call site anywhere in the tree**.

The load-time / call-time split is a `__index` swap on one permanent, empty globals table
(`function_lua.c:489-496`). Two frozen snapshots live in the registry: `__LIBRARY_API__`, a
*restricted* `redis` table containing only `register_function`, `log`/`LOG_*` and the version
constants (`function_lua.c:434-450`), and `__GLOBALS_API__`, the full builtins + full `redis`
(`function_lua.c:475-482`). `luaEngineCreate` repoints `__index` at `__LIBRARY_API__` just before
running the library body (`function_lua.c:93-99`) and unconditionally repoints it back at
`__GLOBALS_API__` in the `done:` cleanup (`:130-136`). **Why `redis.call` still works from a
callback**: Lua resolves `GETGLOBAL` dynamically at *call* time, and by the time FCALL invokes a
stored callback the swap has already been reverted. The library allow-list
(`script_lua.c:31-108`, enforced by `luaNewIndexAllowList` `:1282-1340`) is a **one-time boot-time**
check, not a per-call one — the comment at `script_lua.c:94-101` says so explicitly.

**FCALL.** `fcallCommandGeneric()` `functions.c:619-656`: feeds monitors *first*
(`functions.c:621`, comment "Functions need to be fed to monitors before the commands they
execute"), resolves the name from the flat dict (`:626`, error `Function not found` `:628`), parses
numkeys (`Bad number of keys provided` `:637`, `Number of keys can't be greater than number of args`
`:641`, `Number of keys can't be negative` `:644`), calls `scriptPrepareForRun` (`:650`), then
`engine->call(...)` (`:653-654`), then `scriptResetRun` (`:655`).

**Where EVAL and FCALL converge** — three shared functions, and only one bit differs:
`scriptPrepareForRun()` `script.c:192-317` (all the no-cluster / stale / readonly-replica / disk-error
/ `_ro`-conflict / min-replicas / OOM gates, and it publishes `curr_run_ctx` at `:314`);
`scriptCall()` `script.c:636-707` (the per-nested-command gate used by `redis.call`);
`luaCallFunction()` `script_lua.c:1659-1745`. The single fork is `SCRIPT_EVAL_MODE`, set only by
`eval.c:613`.

**KEYS/ARGV.** `script_lua.c:1679-1705`. Both paths build the two tables with `luaCreateArray`. Under
`SCRIPT_EVAL_MODE` they are published as globals (`lua_setglobal(lua,"KEYS")` `:1681-1686`,
`"ARGV"` `:1688-1693`) and the call is `lua_pcall(lua,0,1,-2)` (`:1703`). Under FCALL they are left on
the stack and passed **as the callback's two positional arguments**: `lua_pcall(lua,2,1,-4)`
(`:1705`, comment at `:1699-1700`).

**Admin subcommands** (all `CMD_NOSCRIPT`): LOAD `functions.c:1044-1078` (reply = bulk library name,
`:1077`; `Unknown option given: %s` `:1053`; `Function code is missing` `:1058`); DELETE `:586-600`
(`Library not found` `:590`); FLUSH `:810-833` (`FUNCTION FLUSH only supports SYNC|ASYNC option`
`:823`); LIST `:506-581`; STATS `:432-473`; DUMP `:710-715`; RESTORE `:727-807`
(`Wrong restore policy given, value should be either FLUSH, APPEND or REPLACE.` `:748`;
`DUMP payload version or checksum are wrong` `:755`; `Pre-GA function format not supported` `:770`;
`given type is not a function` `:774`); KILL `:603-605`.

`FUNCTION LIST` reply, per library, a map of 3 (4 with `WITHCODE`): `library_name`, `engine`,
`functions` → array of maps `{name, description|nil, flags}` where `flags` is a **RESP3 set** of
status strings (`functionListReplyFlags` `functions.c:475-491`), plus `library_code`. With a
`LIBRARYNAME` filter the outer length is deferred (`functions.c:529`, `:578-580`).
`FUNCTION STATS` reply is a map of 2: `running_script` → nil or
`{name, command → array of the *caller's* argv, duration_ms}` (`functions.c:448-452`), and `engines`
→ `{LUA → {libraries_count, functions_count}}`.

**Persistence and replication.** `rdbSaveFunctions()` `rdb.c:1859-1880` writes
`RDB_OPCODE_FUNCTION2` (245, `rdb.h:104`) followed by the **entire original source text**
(`rdb.c:1871`); load feeds it back through the identical `functionsCreateWithLibraryCtx` path
(`rdb.c:4639`). `FUNCTION DUMP` uses **exactly the same footer as key DUMP** — 2-byte LE RDB version
+ 8-byte LE CRC64 (`functions.c:687-705`), verified by the same `verifyDumpPayload`
(`cluster.c:160-188`) at `functions.c:754`. `FUNCTION LOAD/DELETE/FLUSH/RESTORE` are ordinary
`CMD_WRITE` commands that bump `server.dirty` and never call `preventCommandPropagation`, so they
**propagate verbatim** (`server.c:4218`) — the whole library source travels the replication stream.
`FCALL`/`EVAL` are the opposite: `scriptResetRun` calls `preventCommandPropagation`
(`script.c:339`), so effects replicate individually.

**Timeout / KILL.** One global `static scriptRunCtx *curr_run_ctx` (`script.c:33`) shared by both.
`luaMaskCountHook` (`script_lua.c:1596-1614`) fires every 100000 VM instructions when
`busy-reply-threshold > 0`; `scriptInterrupt()` (`script.c:141-170`) logs, enters timed-out mode
(`:59-65`) and calls `processEventsWhileBlocked()` (`:167`) so the server can still answer KILL.
`scriptKill()` `script.c:361-391` refuses with `-NOTBUSY No scripts in execution right now.`
(`:363`), `-UNKILLABLE The busy script was sent by a master instance...` (`:367`), and — the
important one — `-UNKILLABLE Sorry the script already executed write commands against the dataset...`
(`:372-376`) driven by `SCRIPT_WRITE_DIRTY`, set at `script.c:679`. Cross-type mismatch replies
`shared.slowscripterr` / `shared.slowevalerr` (`server.c:2244-2247`).

**Where the code actually is:** over 80% of `functions.c` is engine-agnostic bookkeeping — dict
management, RESP shaping, RDB/DUMP framing, name and shebang parsing. Only `fcallCommandGeneric`
(~40 lines) and four one-line vtable call-outs reach into Lua. `function_lua.c` is ~100% engine
glue, of which the two `register_function` argument parsers are the largest chunk (~215 lines).

### The obstacle, stated plainly

TomoKV's Lua v1 creates and destroys a whole interpreter per command:

```c
lua_State* state = luaL_newstate();          // src/cmd/scripting.cc:695
create_sandbox(state, context);              // :698  (opens base/table/string/math, strips ~15 APIs)
luaL_loadbuffer(state, source.p, source.n, "user_script");   // :699
...
lua_close(state);                            // :704, :711, :720, :732, :746 — every exit path
```

`NOTES-LUA.md` states the design intent: *"Every evaluation receives a fresh Lua state, so globals do
not leak between requests."*

FUNCTION's entire premise is the exact opposite. A library is compiled **once**, its callbacks live
as `luaL_ref` handles in that state's registry, and every later FCALL is just a `lua_rawgeti` plus a
`lua_pcall`. On a fresh-state engine a registry ref is meaningless, so FCALL would have to recompile
*and re-execute* the whole library body on every call. That makes FCALL **strictly slower than the
EVALSHA that already exists** — which is the one outcome the feature must not have.

So FUNCTION on tomokv is not a command-surface item. It is a request for a **persistent
per-executor Lua state**, and that is the decision to make.

### TomoKV design sketch — the honest version

Routing is the easy part and is already solved: FCALL takes `numkeys` at argv 2 and keys from argv 3,
so it is the EVAL shape shifted by one. Reuse `CmdFlags::ScriptRoute` (`src/cmd/command.h:42`),
`command_script_key_range` (`scripting.cc:824-832`) with `first = 3`, and the same single-owner
CROSSSLOT check that V1 documents (`NOTES-LUA.md`). Redis's own key extractor is the same shape:
`functionGetKeys` = `genericGetKeys(0, 2, 3, 1, ...)` (`db.c:3672-3675`) versus `evalGetKeys`
(`db.c:3667-3670`).

The state is the hard part:

1. **Authoritative registry, process-wide.** A `FunctionRegistry` modelled on `ScriptCache`
   (`scripting.cc:103-148`): a mutex, `libname -> {source, function names}`, and a
   `std::atomic<uint64_t> generation_`. `FUNCTION LOAD/DELETE/FLUSH/RESTORE` are
   `ConnLocal|Admin|Write` IO-thread commands that parse the shebang, validate names, mutate this
   map and bump the generation. **They compile nothing** — they cannot, because compilation must
   happen once per executor.
2. **Per-executor home.** One `lua_State*` per EX thread with the sandbox built once, plus
   `libname -> {ref per function}` and a cached `generation`. Hang it off `ThreadCtx`
   (`src/core/thread.h:82`) or a `thread_local` in `scripting.cc`.
3. **Lazy sync on FCALL.** On the owner: one relaxed atomic load; if `home.generation !=
   registry.generation`, take the registry lock, compile the new/changed libraries into this home
   state and `lua_unref` the deleted ones, then update `home.generation`. Steady state costs exactly
   one atomic load. A library therefore compiles at most once per executor, at the first FCALL after
   a load.
4. **Invocation.** `lua_rawgeti(L, LUA_REGISTRYINDEX, ref)`, push the keys table and the args table,
   `lua_pcall(L, 2, 1, errh)` — the FCALL convention (`script_lua.c:1705`), not the KEYS/ARGV-globals
   convention. Reply conversion reuses `append_lua_result` (`scripting.cc:743-745`) unchanged.
5. **`ScriptUndo`** (`scripting.cc:717-722`) works for FCALL unmodified — it captures declared-key
   images before the call and rolls back on error, exactly as it does for EVAL.

**Three costs this design imports, none of which the current engine pays:**

- **A long-lived Lua heap on the executor.** Its garbage collector now runs inside the EX task pass,
  at times tomokv does not choose and for durations tomokv does not budget. The fresh-state design
  converts this into a fixed, bounded per-EVAL allocation. This is a direct conflict with the house
  rule that always-on machinery costs ≤3% and with the tree's stated aversion to unbudgeted work in
  the loops (`docs/COMPLEXITY-AUDIT.md`, "work is repeatedly charged to sets that merely *could*
  contain work"). Mitigation exists — `lua_gc(L, LUA_GCSTEP, n)` with a fixed step at a task
  boundary, plus a `--function-heap-max` byte cap that recycles the home state when exceeded — but
  it is machinery that must be designed, not assumed.
- **Role flips leak states.** `ThreadCtx::role_` is atomic and a thread can be converted
  (`src/core/thread.h:93,106`). A converted executor must free its Lua home or every flip leaks one
  interpreter plus every compiled library.
- **Persistent library globals survive a rolled-back FCALL.** With `--atomic 1` the store side is
  restored but library-level Lua state is not. Redis has the identical property, so this is parity,
  not a regression — but it must be written down, because V1's fresh-state guarantee currently makes
  the stronger promise.

### The cheap alternative, and why it is a trap

FUNCTION can be faked in ~250 lines: store the library source, and have FCALL synthesise
`<library source> ; return __registered[<name>](KEYS, ARGV)` and run it through the existing
fresh-state `run_eval`. It needs no new mechanism at all.

**Do not do this.** It re-executes the library body on every call, so it is slower than EVALSHA; and
it silently changes semantics, because library top-level side effects would fire once per call
instead of once per load. Shipping a FUNCTION that is worse than the EVAL it sits next to is worse
than not shipping one.

### Knobs and reply formats

If built:

- `--function-heap-max N` bytes, `0` = FUNCTION disabled entirely (no per-thread state allocated,
  the plain EVAL path stays byte-identical), `-1` = auto. This satisfies the knob rule: zero means
  no allocation and no always-on cost.
- No other knob. The 500 ms load watchdog and the 100000-instruction budget are hard-coded, matching
  the existing `kInstructionLimit` (`scripting.cc:44`).

Registry rows (a new `function_command_table()` alongside `scripting_command_table()`,
`src/cmd/command.h:84`):

```
{"FCALL",    3, -1, Write | CursorShard | ScriptRoute, cmd_fcall,    3, -1, 1},
{"FCALL_RO", 3, -1, Readonly | CursorShard | ScriptRoute, cmd_fcall_ro, 3, -1, 1},
{"FUNCTION", 2, -1, ConnLocal | Admin,                 cmd_function, 0,  0, 0},
```

Replies must match `functions.c` exactly, including the RESP2 flattening of `FUNCTION LIST`'s maps
and the RESP2 array rendering of its `flags` **set** — the tree is RESP2-only, so both collapse to
flat arrays, exactly as `cmd_hello` already flattens the seven-field map to a 14-element array
(`t_server.cc:380-388`).

**`FUNCTION KILL` must reply `-NOTBUSY No scripts in execution right now.` unconditionally**, and
this must be documented rather than hidden. V1's design makes cross-thread kill impossible by
construction: *"the owner itself reaches the hook and unwinds the protected Lua call"*
(`NOTES-LUA.md`). There is no second thread that can observe a busy executor, and adding one would
mean a cross-thread flag read on the script hot path.

`FUNCTION DUMP`/`RESTORE` should reuse whatever §7 decides for key DUMP/RESTORE — the two share the
footer convention in Redis (`functions.c:687-705` vs `cluster.c:90-144`) and should share it here.

### Build size — **M**, and it is the wrong kind of M

~700–900 lines: ~250 for the registry and admin subcommands, ~150 for FCALL routing and invocation,
~200 for the per-thread home and its lazy sync, ~150 for reply shaping, plus the GC-budget and
role-flip teardown machinery. The line count is moderate; the *mechanism* count is not — it adds a
long-lived, GC-bearing heap to the executor, which is a new always-on cost class.

### Risk and shelve criteria

**Recommendation: shelve, and document as unsupported.**

The value is low. FUNCTION is a management surface over an engine tomokv already has: everything a
library does, an `EVALSHA` does today. No mainstream client requires FCALL to connect or to run an
ordinary workload — unlike RESP3 (§8) or CLIENT (§6), nothing degrades without it. Against that, it
is the only item of the eight that puts unbudgeted work inside the executor pass.

**Build it only if all four hold:**
1. A named consumer requires `FCALL` specifically, not merely "scripting".
2. The owner accepts a long-lived Lua heap on EX threads, and a GC budget is designed and A/B'd
   against the ≤3% always-on rule.
3. Role-flip teardown of the per-thread state is designed before the first line is written.
4. `FUNCTION KILL`'s permanent `-NOTBUSY` is accepted as documented behaviour.

If any fails, add a `NOTES-COMPAT.md` row next to the RESP3 refusal: *FUNCTION is not implemented;
use EVAL/EVALSHA, which are*, with the fresh-state reason given explicitly. A reasoned refusal is a
better artifact than a slow implementation.

### Test plan sketch

Only relevant if built. Extend `tests/lua_scripting.py` rather than adding a suite, since the
existing battery already covers conversion, cache arms, cross-owner rejection and atomic rollback:

- Shebang parsing: every one of the six verbatim metadata errors above.
- `register_function` in both forms; all five flags; duplicate names within a library and across two
  libraries; a library that registers nothing.
- **Load-time sandbox**: a library body that calls `redis.call` at top level must fail; the same call
  from inside a registered callback must succeed. This is the single most important correctness test
  and it directly exercises the `__index` swap (`function_lua.c:93-99` / `:130-136`).
- **The persistence property, which is the whole point**: `FUNCTION LOAD` a library whose body has an
  observable side effect (increments a Lua upvalue), then FCALL it N times, and assert the side
  effect happened **once**. A fresh-state implementation fails this test; the sugar version fails it
  loudly. Ship this test first.
- **Per-executor sync**: load a library, then FCALL against keys on every shard, forcing every EX
  thread to compile it; assert identical results and that a counter of compilations equals the
  executor count, not the call count. This is the vacuous-validation guard — the sync must be proven
  to have *fired*.
- `FUNCTION LIST` with and without `LIBRARYNAME` and `WITHCODE`; `FUNCTION STATS` idle;
  `FUNCTION DELETE` of a missing library; `FLUSH` with and without SYNC/ASYNC.
- `FCALL` with numkeys 0, negative, and greater than argc; cross-owner keys (must CROSSSLOT).
- `FCALL_RO` against a library without `no-writes` (must refuse).
- Atomic rollback: a failing FCALL with `--atomic 1` restores every declared key.
- ASAN build plus the whole battery, specifically to catch a leaked or double-closed home state
  across a role flip.

---

## 5. MONITOR — **BUILD, IO-side, behind a knob, with four cost containments**

### Redis mechanism

`replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc)` —
`replication.c:806-850`, declared `server.h:3574`.

**The early-out is the first line of the body** (`replication.c:807-808`):

```c
if (monitors == NULL || listLength(monitors) == 0 || server.loading) return;
```

`listLength()` is an O(1) counter read, so with no monitors attached the cost is one call plus two
compares — no allocation, no `gettimeofday`, no iteration.

**Line format** (`replication.c:812-836`). The payload is hand-built as a RESP **simple string**:
`sdsnew("+")` (`:812`), then `sdscatprintf(cmdrepr,"%ld.%06ld ",tv.tv_sec,tv.tv_usec)` from a real
`gettimeofday(2)` (`:816-817`), then one of three client segments (`:818-824`) —
`"[%d lua] "` for `CLIENT_SCRIPT`, `"[%d unix:%s] "` for `CLIENT_UNIX_SOCKET`, else
`"[%d %s] "` with `getClientPeerId(c)` (`networking.c:4177-4185`). Then every argument
(`:826-835`): `OBJ_ENCODING_INT` args are printed as `"\"%ld\""`, all others through
`sdscatrepr` (`sds.c:970-993`), which wraps in `"` and escapes `\`, `"`, `\n`, `\r`, `\t`, `\a`,
`\b` as two-char forms and any other non-`isprint()` byte as `\xHH`. Space-separated, then `\r\n`
(`:836`).

**There is no truncation.** Not on `argc`, not on per-argument bytes — the loop runs
`j = 0..argc` unconditionally and `sdscatrepr` walks the full length. (The 1024-byte truncation at
`debug.c:2357` and the 64-byte one at `networking.c:3752` are unrelated debug paths.)

**Delivery** (`replication.c:837-849`): one `createObject(OBJ_STRING,cmdrepr)`, then per monitor
`addReply(monitor,cmdobj)` **and** `updateClientMemUsageAndBucket(monitor)` (`server.c:1139`).
Commands flagged `CMD_INTERNAL` (`server.h:268`) are hidden from non-`CLIENT_INTERNAL` monitors
(`:843-845`).

**Call sites** — five, and the pattern matters:
- `server.c:4141-4147`, inside `call()`, the main path:
  ```c
  if (update_command_stats && !reprocessing_command &&
      !(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN)))
  ```
  Exclusions are `CMD_SKIP_MONITOR` (`1ULL<<11`, `server.h:249`) and `CMD_ADMIN`
  (`1ULL<<4`, `server.h:243`). It uses `c->original_argv` when set, so **a rewritten command is
  shown to monitors in its original client-issued form** (`EXPIRE`, not `PEXPIREAT`).
- `eval.c:636` (`EVAL`) and `eval.c:650` (`EVALSHA`) — explicit early feeds, *because* those
  commands carry `CMD_SKIP_MONITOR`; the comment is "so that lua commands appear after their script
  command". Each nested `redis.call` then feeds itself through the normal `server.c:4146` path.
- `functions.c:621` (`FCALL`) — same rationale.
- `multi.c:124`, in `execCommandAbort()` — synthesises an `EXEC` line when `EXEC` itself is rejected
  before `call()` runs.

**`monitorCommand()`** — `server.c:7188-7203`. Refuses `CLIENT_DENY_BLOCKING` with
`MONITOR isn't allowed for DENY BLOCKING client` (`:7189-7194`); **silently returns with no reply
at all** if the client is already a replica or monitor (`:7198`); otherwise sets
`CLIENT_SLAVE|CLIENT_MONITOR` together (`:7200`), `listAddNodeTail(server.monitors,c)` (`:7201`),
`+OK`. The MULTI refusal is *indirect* — `MONITOR` lacks `CMD_NO_MULTI`, so it queues, and at
`EXEC` time `multi.c:164` sets `CLIENT_DENY_BLOCKING`, which is what `monitorCommand` then trips
over. The subscriber-mode refusal is the generic RESP2 pubsub gate at `server.c:4761-4776`.

**Cleanup**: `freeClient` → `networking.c:2428-2431`
(`listSearchKey(server.monitors,c); listDelNode(...)`), and `RESET` →
`clearClientConnectionState()` `networking.c:2142-2148`.

**Cost with a monitor attached**: per eligible command, per client — one `gettimeofday(2)` syscall,
several `sdscatprintf`/`sdscatrepr` calls proportional to total argument bytes, one `robj`
allocation, and then per monitor an `addReply` plus a full `updateClientMemUsageAndBucket()`
recomputation. All of it synchronous, inside `call()`, on the executing thread. Redis's own
multi-IO-thread code pins monitor clients to the main thread for exactly this reason
(`iothread.c:292-298`).

### TomoKV design sketch

**The key structural observation: tomokv can feed monitors entirely on the IO thread, and never
tell an executor that MONITOR exists.** The IO thread already holds the parsed argv and the resolved
spec before it dispatches — `op->spec = spec;` at `src/core/io_loop.h:403`. Redis has to feed inside
`call()` because that is where it first has both; tomokv has both strictly earlier. So the feed sits
in `parse_and_dispatch` and the store path, the task channels and the executors are all untouched.

Transport is already built and proven: pub/sub established the exact mechanism this needs.
- `PubSubEvent` (`src/core/pubsub_event.h:39-52`) is a heap-owned, cross-IO message that names a
  connection by `(target_io, conn_id)` and **never** by a cross-thread `Client*` — the header's own
  comment says so (`pubsub_event.h:1-5`).
- `pubsub_post()` (`src/core/pubsub.inc:63-76`) delivers it and wakes the target with **one
  `nullptr` token** on the existing `client_in` IO-to-IO channel, reusing the notify-mask and
  park/wake protocol unchanged.
- `pubsub_append_wire()` (`src/core/pubsub.inc:177-185`) is the "append spontaneous bytes to a
  connection" primitive, and `pubsub_deliver()` (`:186-208`) already handles the hard case: when the
  ROB is not quiesced it appends the frame to the newest live Op (`:196-203`) so the push lands
  behind every command already parsed, preserving wire order.

Add a `MonitorFeed` event kind and a `MonitorRegister`/`MonitorCleanup` pair. Registration
replicates the monitor's `(io, conn_id)` to every IO thread — there are at most a few dozen IO
threads and monitors are rare, so a replicated list beats a home-thread index here (the opposite of
the pub/sub channel-home decision, and for the opposite reason: pub/sub has many channels and few
publishers per channel, MONITOR has one "channel" and every thread publishes to it).

**Feed point.** Place the feed **after the last IO-side refusal**, immediately before the task is
published — not at `op->spec = spec`. Redis feeds inside `call()`, i.e. only for commands that
actually execute; feeding at spec-resolution would show commands that are subsequently refused for
routing (CROSSSLOT) or admission reasons. Feeding at publish time closes almost all of that gap.

### Cost containment — this is the substance of the item

1. **One relaxed atomic load on the dispatch path.** `Server::monitor_count()` as a
   `std::atomic<uint32_t>`, read under `__builtin_expect(count != 0, false)`. When zero, the plain
   path is byte-identical and nothing is allocated — the knob rule's `0` case, satisfied structurally
   rather than by a config check.
2. **No `gettimeofday` per command.** The loops already carry a cached wall clock
   (`cached_now_ms_`, threaded through `set_cached_now_ms`, `src/core/ex_loop.h:168`). Take one
   `clock_gettime(CLOCK_REALTIME)` per *pass*, not per command. Document that the microsecond field
   has millisecond granularity, or take the syscall once per serve batch if exact microseconds are
   wanted. Redis pays one syscall per fed command; tomokv should not.
3. **A bounded per-monitor feed buffer and a drop counter — mandatory.** A monitor on a slow socket
   must never apply backpressure to the data path. Bound the pending feed bytes per monitor; on
   overflow drop frames and count them. Redis has no such bound, which is why an attached monitor is
   a real availability hazard there (it grows the output buffer until the client-output-buffer-limit
   kills the connection). tomokv should do better **and must report it** — a silently lossy debug
   stream is worse than no stream.
4. **Argument truncation, on by default.** Redis copies a 512 MiB `SET` value verbatim into the
   monitor stream. Cap per-argument bytes with a marker, and make the cap a knob whose `0` restores
   exact Redis parity.

### Knobs and reply formats

```
--monitor-buffer N     bytes of pending feed per monitor.
                       0 = MONITOR unavailable (command replies with an error; no registry,
                           no event kind allocated, dispatch path byte-identical)
                      -1 = auto (resolve at boot; suggest 64 MiB)
--monitor-arg-max N    bytes shown per argument; 0 = unlimited (exact Redis parity); default 128
```

Both live via `CONFIG SET` like `zc-min` (`src/core/config.h:61`, `NOTES-COMPAT.md` CONFIG section).

Registry row (`src/cmd/t_server.cc` `kTable[]`, `t_server.cc:892`):

```
{"MONITOR", 1, 1, CmdFlags::ConnLocal | CmdFlags::Admin, cmd_monitor, 0, 0, 0},
```

`CmdFlags::Admin` on the row is also the *exclusion* predicate: feed every dispatched command whose
spec lacks `CmdFlags::Admin`, which is tomokv's nearest equivalent of Redis's
`CMD_SKIP_MONITOR|CMD_ADMIN` and correctly hides `MONITOR`, `CLIENT`, `CONFIG`, `INFO`, `COMMAND`,
`SAVE`/`BGSAVE`, `KEYS` and `OBJECT` from the stream.

A `monitor_` bit on `Client`. **It must land in the existing bool run at
`src/net/conn.h:495-504` without growing the struct** — `subscriber_mode_` is documented as
consuming existing alignment padding (`conn.h:503`); verify with the
`static_assert(sizeof(Client) == 1984)` at `conn.h:538` before committing to the design.

Wire format, byte-for-byte with `replication.c:812-836`:

```
+<sec>.<usec> [<db> <addr>] "<arg>" "<arg>" ...\r\n
```

with `[0 lua]` never produced (see divergences), `[0 unix:<path>]` when the connection came in on
the Unix acceptor (`NOTES-COMPAT.md`, "Unix socket design"), and the `sdscatrepr` escape set
reproduced exactly.

Two INFO STATS counters, per the vacuous-validation rule: `monitor_frames_sent` and
`monitor_frames_dropped`.

### Documented divergences

These are real and must be written into `NOTES-COMPAT.md`, not discovered by a user:

1. **No global order across IO threads.** Within one connection the feed is exact parse order; within
   one IO thread it is exact dispatch order; **across IO threads there is no order and no shared
   clock**, so two commands can appear in the stream in an order different from the order their
   owners executed them. Redis's "the monitor stream is the execution order" guarantee is a property
   of being single-threaded. Reproducing it here needs a global sequencer on the hot path, which the
   ≤3%-always-on rule forbids.
2. **Nested script commands are not fed.** Redis feeds the outer `EVAL` explicitly (`eval.c:636`) and
   then each `redis.call` through the ordinary path. tomokv feeds the outer `EVAL`/`EVALSHA` at
   dispatch and does **not** feed the nested calls, because the interpreter runs on an executor and
   feeding from there would put monitor work on the shard-owning thread. Anyone using MONITOR to
   debug scripts will notice; say so.
3. `MULTI`-queued commands are fed as `EXEC` dispatches them, which matches Redis; but a rejected
   `EXEC` does not synthesise the `execCommandAbort` line (`multi.c:124`).

### Build size — **M**

~350–450 lines. Roughly: formatter and escape function ~90; event kind, registration replication and
fanout ~140 (most of it a copy of the pub/sub request/result shape at `pubsub.inc:296-319`);
registry row, `Client` bit and the dispatch hook ~40; drop accounting and INFO fields ~50; knobs and
CONFIG rows ~40; cleanup on disconnect ~30.

### Risk

Moderate, concentrated in three places.

1. **The escape function must be byte-exact with `sdscatrepr`** (`sds.c:970-993`), including that
   `isprint()` is locale-sensitive in C but Redis runs in the C locale. Get this wrong and every
   binary-valued command silently diffs.
2. **`Client` footprint.** If the `monitor_` bit does not fit the existing padding, the design must
   pay for it elsewhere rather than growing 1984 — the footprint law at `conn.h:539-542` is explicit
   that growth is allowed only knowingly.
3. **Vacuous validation.** A MONITOR test that attaches a monitor and sees *some* lines proves
   little. The directed test must assert both counters moved, and must include a deliberate
   slow-monitor case that forces `monitor_frames_dropped > 0`.

**No shelve criteria.** The alternative — documented-unsupported — is defensible on the grounds that
MONITOR is a debugging tool with unreproducible ordering semantics, but it is the single most-reached-for
operational command in the Redis ecosystem, and its transport already exists in this tree. Build it.

### Test plan sketch

`tests/monitor.py`, modelled on `tests/pubsub.py` (`NOTES-pubsub.md` test section):

- One monitor, one writer: assert every issued command appears exactly once, in order, with exact
  quoting for binary values, embedded NULs, empty strings and integer-encoded arguments.
- Admin exclusion: issue `CONFIG GET`, `CLIENT INFO`, `INFO`, `COMMAND COUNT`, `MONITOR` from a
  second connection and assert **none** appear.
- Multiple monitors (say 8) receive identical streams.
- Ordering: single connection issuing 10k pipelined commands — the stream must be exactly the issue
  order. Then two connections **on the same IO thread** — still exactly ordered. Then two on
  different IO threads — assert only the per-connection subsequences, which is the documented
  guarantee.
- Truncation: `--monitor-arg-max 16`, a 1 MiB value, assert the marker; then `0` and assert the full
  value.
- Drop: a monitor that never reads, plus a 1 MiB-value write loop; assert `monitor_frames_dropped`
  becomes nonzero and that the **writer's throughput is unaffected** — that is the actual property
  under test.
- Lifecycle: 200 abrupt monitor connect/disconnect cycles concurrent with load; assert
  `monitor_frames_sent` keeps rising, no leak, and clean shutdown with no stuck connections (the
  pub/sub battery's shape).
- `MONITOR` twice on one connection; `MONITOR` inside `MULTI`; `MONITOR` while subscribed.
- `--monitor-buffer 0`: `MONITOR` must be refused and the dispatch path must be provably unchanged
  (instructions/op A/B, per the loopback `instr/op` bisect instrument).

Then `tests/gate.sh quick`.

---

## 6. CLIENT subcommands — **BUILD, but fix the catalog first**

### Redis mechanism

All subcommands dispatch through one `strcasecmp` chain in `clientCommand()`
(`networking.c:4470-5029`), except `CLIENT SETINFO`, which has its own top-level function
`clientSetinfoCommand()` (`networking.c:4418-4444`).

**`catClientInfoString()`** — `networking.c:4209-4321`. The flags block (`:4222-4254`) builds a
`char flags[17]`: `O` monitor, `g` replica mid-slot-migration, `S` replica, `o` master importing,
`M` master, `P` pubsub, `x` MULTI, `b` blocked, `t` tracking, `R` broken redirect, `B` tracking
bcast, `d` dirty CAS, `c` close-after-reply, `u` unblocked, `A` close-ASAP, `U` unix socket,
`r` cluster readonly, `e` no-evict, `T` no-touch, `C` rdb-channel, `I` internal, `N` none.

The field list is one `sdscatfmt` at `networking.c:4277-4317`, in exactly this order:

```
id addr laddr fd name age idle flags db sub psub ssub multi watch qbuf qbuf-free
argv-mem multi-mem rbs rbp obl oll omem omem-shared omem-unshared tot-mem events
cmd user redir resp lib-name lib-ver io-thread tot-net-in tot-net-out tot-cmds
read-events avg-pipeline-len-sum avg-pipeline-len-cnt
```

`cmd=` is `lastcmd->fullname` or the literal `"NULL"` (`:4305`); `user=` is the ACL user name or
`"(superuser)"` (`:4306`); `multi=` is `mstate.count` or `-1` (`:4290`); `redir=` is the tracking
redirect id or `-1` (`:4307`); the bare `fd=%i` comes from `connGetInfo` (`connection.h:381-384`).
**The function pauses the client's owning IO thread while it reads** (`:4212-4220`, `:4319`).

`getAllClientsInfoString(int type)` `networking.c:4323-4350` filters with `getClientType()`
(`networking.c:5418-5426`), which deliberately reports monitors as `CLIENT_TYPE_NORMAL`:
*"Even though MONITOR clients are marked as replicas, we want the expose them as normal clients."*
So `CLIENT LIST TYPE normal` **includes** monitors and `TYPE replica` excludes them.

**`CLIENT LIST`** `networking.c:4543-4578`: `[TYPE normal|master|replica|pubsub]`
(`Unknown client type '%s'`, `:4550`) or `ID id [id...]` (`Invalid client ID`, `:4560`; missing ids
silently skipped, `:4564-4568`). Reply is `addReplyVerbatim(...,"txt")` (`:4577`).
**`CLIENT INFO`** `networking.c:4537-4542`, same formatter, same verbatim delivery.

**`CLIENT KILL`** `networking.c:4607-4721`. Old form `CLIENT KILL addr:port` at argc 3, with
`skipme = 0` — *"With the old form, you can kill yourself"* (`:4619-4622`). New filter form
(argc > 3), all filters ANDed: `ID` (`:4630-4636`, `client-id should be greater than 0`),
`MAXAGE` (`:4637-4648`, `maxage is not an integer or out of range` / `maxage should be greater
than 0`), `TYPE` (`:4649-4655`), `ADDR` (`:4656`), `LADDR` (`:4658`), `USER` (`:4660-4667`,
`No such user '%s'`), `SKIPME yes|no` (`:4668-4676`, defaults to 1 in the new form). Reply
**differs by form**: old → `+OK`, or `-ERR No such client` when nothing matched (`:4711-4714`);
new → always `:<count>` (`:4716`).

The **close-self-last** rule (`:4701-4705`, `:4719-4721`): other matches are `freeClient`d
immediately, but if the victim is the caller, `close_this_client` is set and only *after* the reply
has been queued does it do `c->flags |= CLIENT_CLOSE_AFTER_REPLY`. Two consult sites finish it:
`networking.c:3811-3816` stops parsing further commands, and `writeToClient` calls
`freeClientAsync(c)` once the buffer has fully drained (`networking.c:3011-3013`).

**`CLIENT PAUSE timeout [WRITE|ALL]`** `networking.c:4773-4792`
(`CLIENT PAUSE mode must be WRITE or ALL`, `:4780-4785`) and **`CLIENT UNPAUSE`** `:4769-4772`.
The bitmask (`server.h:763-778`) is the important part:

```c
#define PAUSE_ACTIONS_CLIENT_WRITE_SET (PAUSE_ACTION_CLIENT_WRITE|PAUSE_ACTION_EXPIRE|PAUSE_ACTION_EVICT|PAUSE_ACTION_REPLICA)
#define PAUSE_ACTIONS_CLIENT_ALL_SET   (PAUSE_ACTION_CLIENT_ALL|PAUSE_ACTION_EXPIRE|PAUSE_ACTION_EVICT|PAUSE_ACTION_REPLICA)
```

— a WRITE pause also stops **key expiration, eviction, and replica traffic**. Purposes are
enumerated at `server.h:781-787` (`PAUSE_BY_CLIENT_COMMAND`, `..._SHUTDOWN`, `..._FAILOVER`,
`..._SLOT_HANDOFF`), aggregated by `updatePausedActions()` (`networking.c:5634-5654`). The gate is
in `processCommand()`, `server.c:4830-4838`, and replicas are exempt. Parked clients go on
`server.postponed_clients` via `blockPostponeClient` (`blocked.c:698-714`) and are **replayed from
scratch** through `processCommand()` on unpause (`networking.c:5658-5666`, `blocked.c:184-216`).

**`CLIENT NO-EVICT ON|OFF`** `networking.c:4593-4606` sets `CLIENT_NO_EVICT` and structurally
removes the client from `server.client_mem_usage_buckets[]`. Consult site is
`clientEvictionAllowed()` `server.c:1099-1105` — and note the exclusion is *structural*:
`evictClients()` only scans the bucket lists, so the flagged client is never a candidate.

**`CLIENT NO-TOUCH ON|OFF`** `networking.c:5015-5025` sets `CLIENT_NO_TOUCH`. Consult site is
`lookupKey()` at `db.c:332-335`:

```c
if (((flags & LOOKUP_NOTOUCH) == 0) &&
    (server.current_client && server.current_client->flags & CLIENT_NO_TOUCH) &&
    (server.executing_client && server.executing_client->cmd->proc != touchCommand))
    flags |= LOOKUP_NOTOUCH;
```

which then skips `updateLFU(val)` / `val->lru = LRU_CLOCK()` (`db.c:336-343`). `TOUCH` itself is
explicitly carved out. Both flags are cleared by `RESET` (`networking.c:2181-2183`).

**`CLIENT REPLY ON|OFF|SKIP`** `networking.c:4579-4592`: `OFF` sets `CLIENT_REPLY_OFF` (nothing is
ever sent until turned back on); `SKIP` sets `CLIENT_REPLY_SKIP_NEXT`, which is promoted to
`CLIENT_REPLY_SKIP` at the start of the *next* command's reply cycle (`networking.c:3155-3163`),
suppressing exactly one reply; the gate is `_prepareClientToWrite()` `networking.c:323`.

`SETNAME`/`GETNAME`/`ID`/`SETINFO` at `networking.c:4759-4762`, `:4763-4768`, `:4534-4536`,
`:4418-4444`. Verbatim strings: `Client names cannot contain spaces, newlines or special
characters.` (`:4368`); `Unrecognized option '%s'` (`:4427`);
`%s cannot contain spaces, newlines or special characters.` (`:4433`).

### What tomokv has today

`cmd_client` at `src/cmd/t_server.cc:427-471` implements `ID`, `SETNAME`, `GETNAME`, `SETINFO`,
`INFO`, `LIST` (no filters) and `NO-EVICT` (accepted, no-op). The state lives in a process-global
`std::unordered_map<Client*, ClientMeta>` under `g_clients_mu`
(`t_server.cc:161-171`), populated at connect through `command_client_connected`
(`src/cmd/command.h:107`), and `append_client_line` (`t_server.cc:191-195`) emits only six fields:

```c
appendf(out, "id=%llu addr=%s name=%s db=%u lib-name=%s lib-ver=%s\n", ...);
```

### The real content of this item: the catalog is the wrong shape

`g_clients` is keyed by **`Client*`** — a cross-thread pointer to a structure owned exclusively by
one IO thread. It works today only because every field it caches is cold and copied under a mutex.
It does **not** generalise:

- Every field `CLIENT LIST` is missing (`qbuf`, `obl`, `oll`, `omem`, `tot-net-in/out`, `tot-cmds`,
  `age`, `idle`, `events`, `cmd`, `multi`, `sub`) is **live IO-hot state on the owning thread**.
  Reading it from another IO thread is precisely the cross-thread read of io-hot fields the tree
  forbids. Redis solves this by pausing the owning IO thread (`networking.c:4212-4220`); tomokv
  must not.
- `CLIENT KILL` cannot be built on it at all. Dereferencing a `Client*` from a non-owning thread
  races teardown; the tree already learned this lesson and wrote it into the pub/sub header:
  *"Spontaneous deliveries name connections by owning IO plus process-unique connection id, never by
  a cross-thread `Client*`"* (`NOTES-pubsub.md`; `src/core/pubsub_event.h:1-5`).

**So sequence the item this way: replace the catalog first, then every subcommand is small.**

**Step 0 — CLIENT LIST/INFO/KILL become IO scatters.** Copy the pub/sub request/result shape
verbatim (`PubSubEventKind::ChannelsRequest`/`ChannelsResult`, handled at
`src/core/pubsub.inc:296-312`): the invoking IO posts a request to every IO thread, each formats
**its own** clients' lines from **its own** live state, and the origin gathers the results and
completes the async command with its ROB slot held — the same acknowledgement discipline
`NOTES-pubsub.md` describes for SUBSCRIBE. The global mutex and the `Client*` map both disappear;
the cold name/lib/db fields move onto the owning IO thread beside everything else.

Everything after that is small:

| Subcommand | tomokv shape | Size |
| --- | --- | ---: |
| `LIST` / `INFO` full field set | Formatted on the owning IO thread from live state; emit every field tomokv can answer truthfully and omit the rest rather than emitting a lie. Reply via bulk string (RESP2). | S on top of Step 0 |
| `LIST TYPE` / `LIST ID` | Filter inside each IO thread's pass. `TYPE normal` must include monitors if §5 ships (`networking.c:5418-5426`); `master`/`replica` match nothing. | S |
| `KILL` (both forms) | Same scatter; each IO matches its own clients and calls the existing `mark_closing()`. | M |
| `NO-TOUCH ON\|OFF` | See below — the one with real semantics. | S |
| `NO-EVICT ON\|OFF` | **Keep the current no-op.** It is already correct: tomokv has no *client* eviction (`maxmemory` evicts keys; `evicted_keys` for clients is a placeholder, `NOTES-COMPAT.md`). Only tighten the arity/validation. | S |
| `REPLY ON\|OFF\|SKIP` | A two-bit state on `Client`, consulted at ROB retirement: discard the Op's reply bytes instead of appending them. The ROB still advances — this is "retire without emitting", not "don't execute". | S |
| `SETINFO` error strings | Currently `ERR Unrecognized option or bad number of arguments for CLIENT SETINFO` (`t_server.cc:454`) and a combined message (`t_server.cc:449`); Redis emits `Unrecognized option '%s'` and `%s cannot contain spaces...`. Straight fix. (`SETNAME`'s message at `t_server.cc:434` already matches `networking.c:4368` exactly.) | S |

**`CLIENT KILL`'s close-self-last rule is already implemented in this tree.** `cmd_quit`
(`t_server.cc:417-420`) is exactly Redis's discipline:

```c
reply_ok(op.sink());
if (g_client) g_client->mark_closing();
```

reply first, then flag — the same ordering as `networking.c:4719-4721` followed by
`networking.c:3011-3013`. Reuse it rather than re-deriving it.

**`CLIENT NO-TOUCH` is the one with a real mechanism, and it is free.** The flag must reach the
executor, and `Op::route_flags_` (`src/exec/op.h:123`) is a `uint8_t` with **only bit 0 used**
(`kAtomicHazard`, `op.h:229`). Set bit 1 on IO from the `Client` flag and consult it where the
eviction metadata is bumped — `KvObj::eviction_meta()` / `set_eviction_meta()`
(`src/store/kvobj.h:56-58`), which are already documented as "never written while maxmemory is
disabled". Zero footprint cost, `sizeof(Op)` unchanged.

**Allocate `route_flags_` bits deliberately**, because §8 wants one too:

```
bit 0  kAtomicHazard  (existing)
bit 1  kNoTouch       (this item)
bit 2  kResp3         (item 8)
bits 3-7 free
```

### CLIENT PAUSE — the one to think hardest about

Redis's pause exists to make **failover** safe: `PAUSE_ACTION_REPLICA` is in both action sets
(`server.h:763-778`) and `PAUSE_DURING_FAILOVER` is one of the four purposes (`server.h:781-787`).
tomokv has no replication and no failover, so that entire motivation is absent. What remains is
"quiesce writes for a manual operation".

The faithful implementation is expensive here: the postpone-and-replay model (`blocked.c:698-714`,
`:184-216`) does not fit tomokv's zero-copy parse, because argv are Slices into the connection read
buffer pinned only until the Op retires (`src/exec/op.h:8-13`). Parking a command means either
copying argv into connection-lived storage — which MULTI already does (`src/cmd/multi.h:36-37`) —
or holding the ROB slot. And pausing expiry would mean a control message to every executor to stop
`active_expire_cycle` (`src/core/ex_loop.h:157`).

**Recommended shape: a connection-level parse barrier, not a postponed list.** Store a pause
deadline; in `parse_and_dispatch` (`src/core/io_loop.h:357`), when the deadline is live and the
command is in the paused class, simply `break` out of the parse loop for that connection —
the existing `scatter_barrier()` / `atomic_backpressure()` check at `io_loop.h:363` is already
exactly this shape and costs nothing to extend. No copying, no postponed list, no replay, and the
connection resumes naturally when the deadline passes. Parse the syntax exactly, including the
verbatim `CLIENT PAUSE mode must be WRITE or ALL`.

**And document what is not paused: expiry and eviction continue.** Redis pauses both; tomokv would
not, and saying so is better than a subtly different guarantee.

### Knobs and reply formats

**Knobs: none.** Every subcommand here is connection state or an operator action.

Replies: `LIST`/`INFO` → bulk string, one `\n`-terminated line per client (RESP2; Redis's
`addReplyVerbatim(...,"txt")` degrades to exactly this at `networking.c:1477-1495`).
`KILL` old form → `+OK` / `-ERR No such client`; filter form → `:<count>`.
`PAUSE`/`UNPAUSE`/`NO-EVICT`/`NO-TOUCH`/`REPLY ON` → `+OK`. `REPLY OFF`/`SKIP` → nothing at all.
`GETNAME` → bulk or `$-1`. `ID` → integer.

Registry: no new rows — `CLIENT` is already `{"CLIENT", 2, -1, ConnLocal|Admin, cmd_client, 0,0,0}`
(`t_server.cc:909`). But note that `LIST`/`KILL` becoming async scatters means `cmd_client` can no
longer answer them synchronously under `ConnLocal`; they need the same async-command treatment
pub/sub commands get (`PubSubStartResult::Async`, `src/core/io_loop.h:442-456`).

### Build size

Step 0 (catalog → IO scatter) **M**, ~400 lines, most of it a transcription of the pub/sub
request/result pattern. Then: `LIST`/`INFO` fields **S** ~150; `KILL` **M** ~250;
`NO-TOUCH` **S** ~40; `REPLY` **S** ~60; error-string fixes **S** ~20; `PAUSE` **M** ~200 if built.
Total if everything ships: ~1100 lines, but with one genuinely new mechanism (the client scatter)
that also simplifies what exists.

### Risk and shelve criteria

The risk is almost entirely in Step 0, and it is the good kind — it *removes* a hazard. The current
mutex-and-`Client*` catalog is a latent race that today is masked by only ever caching cold copies;
adding `KILL` on top of it without Step 0 would be a genuine use-after-free.

Two traps:
1. **Disconnect during a scatter.** The pub/sub battery already had to solve this
   (`NOTES-pubsub.md`: "Disconnect sends an idempotent cleanup request to every home IO... keeps the
   client alive until all cleanup acknowledgements return"). `CLIENT KILL` must ride the same
   lifecycle, not invent a second one.
2. **Self-kill inside the scatter.** The victim may be the origin. Follow `cmd_quit`: complete the
   reply through the normal ROB path first, then `mark_closing()`.

**Shelve criteria — `CLIENT PAUSE` only**: shelve unless an operator workflow needs it. Its Redis
semantics are load-bearing for failover, which tomokv does not have, and the honest tomokv version
pauses less than the Redis one does. Everything else in this family should ship: `CLIENT LIST` and
`CLIENT KILL` are what an operator reaches for when a client misbehaves, and today one is
information-poor and the other absent.

### Test plan sketch

`tests/client_cmds.py`, plus differential coverage for the parts that have exact replies:

- **Differential** (`tests/differ.py`, extend the existing suites rather than adding one): every
  error path with a verbatim string — `CLIENT KILL ID 0`, `MAXAGE -1`, `MAXAGE abc`,
  `TYPE bogus`, `USER nosuch`, `SKIPME maybe`, `CLIENT PAUSE -1`, `CLIENT PAUSE 100 SOMETIMES`,
  `CLIENT NO-TOUCH`, `CLIENT NO-TOUCH MAYBE`, `CLIENT SETINFO BOGUS x`, `CLIENT SETNAME "a b"`,
  `CLIENT REPLY MAYBE`, `CLIENT LIST TYPE bogus`, `CLIENT LIST ID notanid`.
- **`CLIENT KILL`**: kill by ADDR, by ID, by TYPE, with SKIPME yes/no, matching zero clients (both
  reply forms), and self-kill — asserting the reply is fully received before the socket closes.
  Then 200 concurrent kill/connect cycles under load, asserting no leak and clean shutdown.
- **`CLIENT LIST` across IO threads**: open N connections spread over every IO thread, assert every
  one appears exactly once and that `age`/`tot-cmds` are plausible and monotonic. Assert the scatter
  *fired* — a counter of participating IO threads equal to the IO thread count, not 1.
- **`CLIENT NO-TOUCH`**, the only one with observable data-path effect: run with `--maxmemory` and
  `allkeys-lru`, read a key repeatedly with NO-TOUCH ON, and assert its eviction metadata did **not**
  advance while a control connection's reads did. Then assert `TOUCH` still bumps it even with
  NO-TOUCH ON (the `db.c:332-335` carve-out). This is the mechanism-fired proof for the item.
- **`CLIENT REPLY`**: `OFF` then a burst of writes then `ON` — assert exactly one `+OK` arrives and
  the writes took effect; `SKIP` suppresses exactly one reply.
- **`CLIENT PAUSE`** if built: assert reads proceed under `WRITE`, writes stall, the deadline
  releases them, `UNPAUSE` releases early, and that expiry demonstrably *continued* (the documented
  divergence, tested rather than assumed).

Then `tests/gate.sh quick`.

---

## 7. DUMP / RESTORE — **DECIDE: ship a self-compatible codec, or shelve**

### Redis mechanism

`createDumpPayload()` `cluster.c:90-144`; `verifyDumpPayload()` `cluster.c:160-188`; `dumpCommand()`
`cluster.c:193-209`; `restoreCommand()` `cluster.c:212-375`.

**Payload layout** (`cluster.c:123-128`, verbatim comment):

```
----------------+---------------------+---------------+
... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
----------------+---------------------+---------------+
```

The RDB payload is `rdbSaveObjectType()` (one type byte, `cluster.c:118`) followed by
`rdbSaveObject()` (`cluster.c:119`). Version bytes are little-endian
(`buf[0]=RDB_VERSION&0xff; buf[1]=(RDB_VERSION>>8)&0xff`, `cluster.c:131-133`). The CRC is
`crc64(0, buffer, len_including_version_bytes)` byte-swapped to little-endian and appended
(`cluster.c:135-143`). `RDB_VERSION` in this tree is **15** (`rdb.h:21`).

**Verification** `cluster.c:160-188`: minimum length 10 (`:166`); footer at `p+(len-10)`;
`rdbver = (footer[1]<<8)|footer[0]` (`:170`); the rule is **`if (rdbver > RDB_VERSION) return
C_ERR`** (`:174`) — accept anything at or below the running server's version; a CRC field of exactly
zero is treated as "no checksum, always valid" (`:181-182`); otherwise recompute over `len-8` bytes
and compare (`:184-187`).

**RESTORE options** (`cluster.c:211`, `:219-250`): `REPLACE`, `ABSTTL`, `IDLETIME seconds`
(mutually exclusive with FREQ, must be ≥0), `FREQ 0..255`. The TTL argument is **milliseconds,
relative to now by default**; `ABSTTL` makes it absolute. Redis converts to absolute immediately
(`ttl += commandTimeSnapshot()`, `cluster.c:283`) and then **rewrites its own propagation to always
carry an absolute TTL plus `ABSTTL`** (`cluster.c:351-359`). Error strings, verbatim:
`Invalid TTL value, must be >= 0` (`cluster.c:263`);
`Invalid IDLETIME value, must be >= 0` (`cluster.c:231`);
`Invalid FREQ value, must be >= 0 and <= 255` (`cluster.c:242`);
`DUMP payload version or checksum are wrong` (`cluster.c:271`);
`Bad data format` (`cluster.c:290` and `:298`, the same string from two sites);
and `shared.busykeyerr` = `-BUSYKEY Target key name already exists.` (`server.c:2264-2265`,
raised at `cluster.c:252-257`). An already-expired absolute TTL takes a fast path that inserts
nothing and still replies `+OK` (`cluster.c:318-332`).

**The value codec.** `rdbSaveObjectType()` `rdb.c:694-758` chooses the tag; `rdbSaveObject()`
`rdb.c:1139-1674` writes the body. Tags emitted by *this* tree:

| Value | Tag | # | Save site |
| --- | --- | ---: | --- |
| string | `RDB_TYPE_STRING` | 0 | `rdb.c:696` |
| list (quicklist **and** listpack) | `RDB_TYPE_LIST_QUICKLIST_2` | 18 | `rdb.c:698-702`; listpack is wrapped as a fake 1-node quicklist at `rdb.c:1170-1179` |
| set, intset | `RDB_TYPE_SET_INTSET` | 11 | `rdb.c:704` |
| set, hashtable | `RDB_TYPE_SET` | 2 | `rdb.c:706` |
| set, listpack | `RDB_TYPE_SET_LISTPACK` | 20 | `rdb.c:708` |
| zset, listpack | `RDB_TYPE_ZSET_LISTPACK` | 17 | `rdb.c:713` |
| zset, skiplist | `RDB_TYPE_ZSET_2` | 5 | `rdb.c:715` |
| hash, listpack | `RDB_TYPE_HASH_LISTPACK` | 16 | `rdb.c:720` |
| hash, listpack + field TTL | `RDB_TYPE_HASH_LISTPACK_EX` | 25 | `rdb.c:722` |
| hash, hashtable | `RDB_TYPE_HASH` | 4 | `rdb.c:724` |
| hash, hashtable + field TTL | `RDB_TYPE_HASH_METADATA` | 24 | `rdb.c:727` |
| hash, template forms | 29/30/31/32 | | `rdb.c:734-741` — **not upstream** |
| stream | `RDB_TYPE_STREAM_LISTPACKS_5` | 27 | `rdb.c:745` |
| array / GCRA / module | 28 / 33 / 7 | | `rdb.c:751-754` |

Primitives: `rdbSaveLen` `rdb.c:176-207` / `rdbLoadLenByRef` `:219-253`, with
`RDB_6BITLEN 0`, `RDB_14BITLEN 1`, `RDB_32BITLEN 0x80`, `RDB_64BITLEN 0x81`, `RDB_ENCVAL 3`
(`rdb.h:37-42`) and `RDB_LENERR = UINT64_MAX`. `rdbSaveRawString` `rdb.c:457-487` tries integer
encoding first when `len <= 11` (`:462`) via `rdbEncodeInteger` `:270-290` (`RDB_ENC_INT8/16/32`),
then LZF when `server.rdb_compression && len > 20` (`:472`, with the in-code justification "under 20
bytes it's unable to compress even aaaaaaaaaaaaaaaaaa") via `rdbSaveLzfStringObject` `:381-397`
(which additionally requires `len > 4` and bails if compression does not shrink), else a verbatim
`[rdbSaveLen(len)][bytes]` (`:479-486`). Scores in `RDB_TYPE_ZSET_2` use
`rdbSaveBinaryDoubleValue` `rdb.c:667-670` — raw IEEE-754 binary64, little-endian, no length prefix
— called at `rdb.c:1248`. The older text form `rdbSaveDoubleValue` `rdb.c:619-642` (length-prefixed
ASCII with sentinels 253=NaN, 254=+inf, 255=-inf) is load-only today.

Per-type serializer sizes, measured: STRING ~4 lines (`rdb.c:1142-1145`), LIST ~37
(`:1146-1182`), SET ~36 (`:1183-1218`), ZSET ~37 (`:1219-1255`), **HASH ~175** (`:1256-1430`),
STREAM ~155 (`:1431-1585`), ARRAY ~51 (`:1619-1669`).

### The decision this item actually poses

There are two entirely different features hiding behind one command pair, and they should not be
confused:

**(a) Self-compatible DUMP/RESTORE** — the payload is opaque and only tomokv reads it. Used for
`DUMP k` → `RESTORE k' 0 <payload>` copies, backup scripts, and key migration between tomokv
instances. This is genuinely small, because **tomokv already has a complete per-type value codec**:
`SnapshotTypeHooks{begin_save, read_save, load}` (`src/snapshot/format.h:53-75`), implemented for all
five types (`string_snapshot_hooks` `t_string.cc:1272`+, and the hash/list/set/zset equivalents —
`grep` finds **zero** remaining `SnapshotHookStatus::Unsupported` in `src/cmd/`). Those hooks already
stream a bounded payload with a resume cursor and already reload through the normal build path under
current compact thresholds (`t_zset.cc:2318` comment). DUMP becomes: call `begin_save`, drain
`read_save` into a buffer, prepend the type+encoding bytes, append a footer. RESTORE becomes: verify
the footer, then call `load` and `insert`.

**(b) Redis-wire-compatible DUMP/RESTORE** — a real `redis-cli --pipe` / `MIGRATE`-from-Redis
interop story. This requires implementing the RDB value codec on both sides for five types, which is
~350 lines of serializer plus ~500 lines of *loader* (loaders are strictly harder: they must accept
every legacy tag, e.g. `RDB_TYPE_HASH_ZIPMAP`=9, `RDB_TYPE_LIST_ZIPLIST`=10,
`RDB_TYPE_ZSET_ZIPLIST`=12, `RDB_TYPE_HASH_ZIPLIST`=13, `RDB_TYPE_LIST_QUICKLIST`=14, plus
`RDB_TYPE_ZSET`=3's text doubles), plus **listpack, ziplist, intset and quicklist parsers** that
tomokv does not have and has no other reason to have, plus LZF decompression, plus CRC64 with
Redis's exact polynomial and table.

### TomoKV design sketch — for (a)

`DUMP` and `RESTORE` are single-key ordinary owner tasks. Registry rows:

```
{"DUMP",    2, 2, CmdFlags::Readonly,                  cmd_dump,    1, 1, 1},
{"RESTORE", 4, -1, CmdFlags::Write | CmdFlags::DenyOom, cmd_restore, 1, 1, 1},
```

Payload, deliberately shaped so a Redis client's `verifyDumpPayload` **rejects** it rather than
misparsing it:

```
[ u8 tomo_type ][ u8 encoding ][ value bytes from read_save ... ]
[ u16 LE format version ][ u64 LE checksum ]
```

Set the 16-bit version field to a value **above** any plausible `RDB_VERSION` (e.g. `0xT0MO`-style
sentinel ≥ 0x8000). Redis's check is `if (rdbver > RDB_VERSION) return C_ERR` (`cluster.c:174`), so
a stock Redis given a tomokv payload replies `DUMP payload version or checksum are wrong` — a clean
refusal instead of a corrupt load. Use the existing `snapshot_checksum` (FNV-1a,
`src/snapshot/format.h:96-100`) rather than importing CRC64; there is no interop to preserve.

`cmd_dump`: `find()` → if absent `reply_nil` → `snapshot_type_hooks(type).begin_save(...)` →
loop `read_save` into a `std::string` sized by `cursor.total` → prepend header, append footer →
`reply_bulk`.

`cmd_restore`: parse `ttl` and the option tail exactly as `cluster.c:219-250` does; check existence
against `REPLACE` (verbatim `BUSYKEY`); verify length ≥ 10, version and checksum; convert relative ms
to absolute unless `ABSTTL`; take the already-expired fast path (reply `+OK`, insert nothing,
`cluster.c:318-332`); otherwise `hooks.load(key, encoding, expire_at_ms, payload, limits, result)` and
insert through the normal admission path. `IDLETIME`/`FREQ` are parsed and validated for exact
error parity but map onto `KvObj::set_eviction_meta()` (`src/store/kvobj.h:56-58`) only when
maxmemory is enabled; otherwise they are accepted and dropped, matching the tree's existing "accepted
compatibility value" idiom (`NOTES-COMPAT.md`, CONFIG section).

### Knobs and reply formats

**Knobs: none.** `DUMP` → bulk string or `$-1`. `RESTORE` → `+OK` or one of the verbatim errors
above. RESP3 makes no difference to either.

### Build size

- **(a) self-compatible: S–M.** ~300 lines total, no new codec, no new parser, no footprint change.
  The snapshot hooks are the whole engine and they already exist and are already differentially
  tested (`tests/snap_typed_roundtrip.py`).
- **(b) Redis-wire-compatible: L.** Honest sizing, per type, for **both** directions:

| Type | Serializer | Loader | New parsers needed |
| --- | ---: | ---: | --- |
| string | ~40 | ~60 | integer encodings, LZF decompress |
| list | ~60 | ~140 | quicklist node framing, listpack, ziplist (legacy) |
| set | ~50 | ~120 | intset, listpack, ziplist (legacy) |
| zset | ~60 | ~140 | listpack, ziplist (legacy), binary + text doubles |
| hash | ~70 | ~160 | listpack, ziplist (legacy), zipmap (legacy) |
| shared | ~80 (`rdbSaveLen`, string encoder, CRC64 table) | ~80 | — |
| **total** | **~360** | **~700** | **~400 more** for listpack/ziplist/intset/zipmap/LZF |

That is roughly **1400–1500 lines of new code whose only purpose is to speak another server's
private on-disk format**, and every one of the legacy tags is a permanent compatibility liability.

### Risk and shelve criteria

**Recommendation: build (a), do not build (b).**

For (a) the risk is low and bounded: the only novel surface is the option parser and the footer, and
a corrupt payload is caught by the checksum before any store mutation. One real trap — `cmd_restore`
must run the checksum and the type dispatch **before** touching the store, so a malformed payload can
never leave a half-inserted object; the snapshot loader already has this property because it builds a
complete `KvObj*` and only then inserts.

**Shelve criteria for (b)**, i.e. build it only if *all three* hold:
1. A named consumer needs to move data **from Redis into tomokv** (or the reverse) without a
   client-side re-write. Nothing in the current lane list implies this.
2. The oracle tree's non-upstream tags (29–33) are confirmed irrelevant, i.e. the interop target is
   stock Redis. Against *this* checkout, a hash could arrive as `RDB_TYPE_HASH_TMPL_LP` and there is
   nothing sensible to do with it.
3. Someone owns the legacy-tag matrix as an ongoing commitment.

Absent those, (b) is a documented non-goal. Document it the way `NOTES-COMPAT.md` documents
RESP3: an explicit, reasoned refusal rather than a silent gap.

### Test plan sketch

New `dump` suite in `tests/differ.py` cannot diff payload *bytes* against the oracle (they are
deliberately different), so it splits in two:

- **Differential, on observable behaviour only**: `RESTORE` error paths — bad TTL, bad IDLETIME, bad
  FREQ, BUSYKEY without REPLACE, a truncated payload, a payload with a corrupted checksum byte, a
  payload with a bumped version byte. All of these must produce byte-identical *error* replies.
- **Round-trip battery** (new `tests/dump_roundtrip.py`, modelled on
  `tests/snap_typed_roundtrip.py`): for every type and every encoding on both sides of every compact
  threshold — string raw/int/extern, hash compact/expanded, list compact/deque, set int-compact/
  generic-compact/table, zset compact/btree — `DUMP` then `RESTORE` to a second key **on a different
  shard**, then assert full structural equality via the type's own read commands, plus `OBJECT
  ENCODING`, `TTL`, and `used_memory` delta.
- Directed: `RESTORE` with `ABSTTL` in the past (must reply `+OK` and create nothing);
  `RESTORE ... REPLACE` over a different type; `DUMP` of a missing key; `DUMP` of a key with a live
  TTL followed by `RESTORE 0` (TTL must not survive).
- Negative interop: feed a real `redis-cli DUMP` payload to tomokv `RESTORE` and assert the verbatim
  `DUMP payload version or checksum are wrong`; feed a tomokv payload to redis and assert the same.

Then `tests/gate.sh quick`.

---

## 8. RESP3 / HELLO — **BUILD the protocol core; gate CLIENT TRACKING separately**

### Redis mechanism

**Request direction is unchanged.** Confirmed by reading both parsers in full:
`processInlineBuffer` (`networking.c:3215-3316`) and `processMultibulkBuffer`
(`networking.c:3366`+) contain **zero** references to `c->resp`, and the multibulk parser asserts
the RESP2 grammar unconditionally: `serverAssertWithInfo(c,NULL,c->querybuf[c->qb_pos] == '*');`
(`networking.c:3391`). RESP3 extends only the reply grammar. **`src/net/resp.h`'s `resp_parse`
needs no change at all.**

**The wire types**, each with its `c->resp` branch:

| Byte | Emitter | Line | RESP2 fallback |
| --- | --- | --- | --- |
| `_` null | `addReplyNull` | `networking.c:1292-1298` | `$-1\r\n` |
| `_` null (array ctx) | `addReplyNullArray` | `:1312-1318` | `*-1\r\n`; RESP3 has no null-array, both become `_\r\n` (comment `:1308-1311`) |
| `,` double | `addReplyDouble` | `:1108-1141` | a `$`-bulk built in place to avoid a memcpy (`:1118-1139`) |
| `#` bool | `addReplyBool` | `:1300-1306` | `:1\r\n` / `:0\r\n`; RESP3 `#t\r\n` / `#f\r\n` |
| `(` bignum | `addReplyBigNum` | `:1143-1151` | bulk string |
| `=` verbatim | `addReplyVerbatim` | `:1477-1495` | plain bulk; RESP3 `=<len+4>\r\n<ext3>:<data>\r\n` |
| `%` map | `addReplyMapLen` | `:1270-1274` | `prefix = c->resp==2 ? '*' : '%'; if (c->resp==2) length *= 2;` |
| `~` set | `addReplySetLen` | `:1276-1279` | `prefix = c->resp==2 ? '*' : '~';`, same length |
| `>` push | `addReplyPushLen` | `:1286-1290` | **none** — `serverAssert(c->resp >= 3)` plus a `CLIENT_PUSHING` assert |
| `\|` attribute | `addReplyAttributeLen` | `:1281-1284` | **none** — `serverAssert(c->resp >= 3)` |

`+`, `-`, `:`, `$`, `*` are protocol-invariant (`networking.c:899-909`, `:690-696`, `:1229-1238`,
`:1392-1394`, `:1264-1268`).

Double payload formatting is `d2string()` (`util.c:718-749`): `nan`, `inf`, `-inf` as literal
tokens, sign-aware `-0`/`0`, an integer fast path via `double2ll`+`ll2string`, else `fpconv_dtoa`.
The *same text* is sent under both protocols; only the framing byte differs.

`shared.null[resp]` / `shared.nullarray[resp]` (`server.c:2268-2276`) are per-protocol prebuilt
constants so hot miss paths can index instead of branch; `shared.emptymap[]` (`:2278-2281`) and
`shared.emptyset[]` (`:2283-2286`) do the same for empty aggregates.

Deferred lengths mirror the same divergence exactly once per type: `setDeferredMapLen`
(`:1086-1090`) and `setDeferredSetLen` (`:1092-1095`) repeat the `addReply*Len` logic verbatim on
top of the shared `setDeferredAggregateLen` (`:1052-1080`).

**`|` attributes have no real data-path use.** The only emitters in the whole tree are
`DEBUG PROTOCOL attrib` (`debug.c:909-919`) and the modules API `RM_ReplyWithAttribute`
(`module.c:3330-3334`). No core command emits one.

**HELLO** — `helloCommand()` `networking.c:5032-5129`. Syntax
`HELLO [protover [AUTH user pass] [SETNAME name]]`. Version gate (`:5042-5045`):

```c
if (ver < 2 || ver > 3) { addReplyError(c,"-NOPROTO unsupported protocol version"); return; }
```

The leading `-` means `addReplyErrorLength` (`:690-696`) does **not** prepend `ERR`, so the wire
text is exactly `-NOPROTO unsupported protocol version\r\n`. Other verbatim strings:
`Protocol version is not an integer or out of range` (`:5038`),
`Syntax error in HELLO option '%s'` (`:5069`), and the long
`-NOAUTH HELLO must be called with the client already authenticated, ...` (`:5088-5095`).

`if (ver) c->resp = ver;` (`:5100-5101`) — **a bare `HELLO` with no argument leaves `c->resp`
untouched**; it is an introspection call. The reply is `addReplyMapLen(c, 6 + !server.sentinel_mode)`
(`:5102`), so it is a `%7` map under RESP3 and a flat `*14` array under RESP2 — which is exactly
what tomokv already emits (`t_server.cc:380-388`). Field order (`:5104-5128`): `server`, `version`,
`proto` (the **new** value), `id`, `mode`, `role` (omitted in sentinel mode), `modules`.
Default `c->resp = 2` at `createClient` (`networking.c:146-149`).

**Commands whose shape changes** — and three widely-assumed ones that *do not*:

| Command | RESP2 | RESP3 | Line |
| --- | --- | --- | --- |
| `CONFIG GET` | flat `*2N` | `%N` map | `config.c:1015` |
| `HGETALL` | flat `*2N` | `%N` map | `t_hash.c:5350-5372` |
| `HRANDFIELD WITHVALUES` | flat array | array of pairs | `t_hash.c:5504-5519`, `:5595` |
| `ZRANDMEMBER WITHSCORES` | flat array | array of pairs | `t_zset.c:4478-4503`, `:4537` |
| `ZSCORE` / `ZMSCORE` | bulk | `,` double | `t_zset.c:4106`, `:4127` |
| `ZINCRBY`, `ZADD INCR` | bulk | `,` double | `t_zset.c:2081-2085` |
| `ZRANK/ZREVRANK WITHSCORE` | `[rank, bulk]` | `[rank, ,double]` | `t_zset.c:4172` |
| `ZPOPMIN/MAX` **with** COUNT | flat `*2N` | nested pairs | `t_zset.c:4274-4290` |
| `ZPOPMIN/MAX` **without** COUNT | flat 2-elem | **still flat** 2-elem | `t_zset.c:4374-4390` (`use_nested_array = (c->resp > 2 && count != -1)`) |
| `ZRANGE`/`ZDIFF`/`ZINTER`/`ZUNION … WITHSCORES` | flat `*2N` | nested pairs | `t_zset.c:3171-3184`, `:3401-3404` |
| `SPOP` with count | array | `~` set | `t_set.c:894` |
| `SMEMBERS` | array | `~` set | `t_set.c:1594` |
| `SINTER`/`SUNION`/`SDIFF` | array | `~` set | `t_set.c:1568`, `:1910` |
| `CLIENT INFO` | `$` bulk | `=txt:` verbatim | `networking.c:4537-4542` |
| `COMMAND DOCS` | flat array | map | `server.c:5704-5720` |
| `GEOPOS` | bulk | `,` double | `geo.c:962-964` |
| `EXEC` abort | `*-1` | `_\r\n` | `multi.c:154` |
| pubsub `message`/`pmessage` | `*3`/`*4` array | `>3`/`>4` push | `pubsub.c:271-299` |
| **`XPENDING`** | array | **unchanged** | `t_stream.c:4265-4430` — no `c->resp` anywhere in the function |
| **`INCRBYFLOAT`** | bulk | **unchanged** | `t_string.c:963-994` — always `addReplyBulk` |
| **`GEODIST` / `WITHDIST`** | bulk | **unchanged** | `geo.c:207-216` — `addReplyDoubleDistance` always emits a bulk |

Also protocol-dependent: RESP2 restricts the command set while subscribed
(`server.c:4759-4769` — *"With RESP3 there are no limits"*), and `PING` uses the special
subscribed-mode array framing **only** in RESP2 (`server.c:5303-5319`). tomokv already implements
the RESP2 side of both (`src/core/io_loop.h:414-441`, `pubsub_reply_ping`).

**CLIENT TRACKING** — `tracking.c`, 761 lines. It is **not** gated at `CLIENT TRACKING ON`;
`enableTracking()` (`tracking.c:166-195`) succeeds unconditionally. The gate is at *delivery*, in
`sendTrackingMessage()` (`tracking.c:292-310`): RESP3 → `>2 ["invalidate", [keys]]`; RESP2 **with**
`REDIRECT` to a pubsub client → a `__redis__:invalidate` pubsub message; RESP2 without redirect →
`goto done`, **silently dropped**. Default mode records fetched keys per client in `TrackingTable`
via `trackingRememberKeys` (`tracking.c:203-245`, called from `call()` at `server.c:4227-4241`);
BCAST mode registers prefixes in `PrefixTable` (`tracking.c:137-157`) and flushes per event-loop
cycle (`trackingBroadcastInvalidationMessages`, `:729-745`). OPTIN/OPTOUT is one test at
`tracking.c:213-216`; NOLOOP is checked in both modes (`:406-412`, `:587-588`). Invalidation is
triggered from `keyModified()` — this tree's rename of `signalModifiedKey` — at `db.c:1209-1215`,
which calls `trackingInvalidateKey(c,key,1)`.

### TomoKV design sketch

**The good news is structural: `src/net/resp.h` is the only file that decides framing, and every
handler already writes through it.** The parser needs no change. The change is (1) get the
protocol version to the reply builders, and (2) add the divergent builders.

**Carrying the version.** Handlers run on executors and receive only `Shard&, Op&` — no `Client`.
Redis reads `c->resp` at every `addReply*`. tomokv's equivalent is a bit on the Op, and there is
one free: `Op::route_flags_` (`src/exec/op.h:123`) is a `uint8_t` with only bit 0 used
(`kAtomicHazard`, `op.h:229`). Reserve **bit 2 = `kResp3`** (bit 1 is claimed by §6's `kNoTouch`),
set on IO from the `Client`'s negotiated protocol at parse time, next to where `op->spec` is
assigned (`src/core/io_loop.h:403`). `sizeof(Op)` unchanged; `sizeof(Client)` needs one bit in the
existing bool run at `conn.h:495-504`, same question as §5's `monitor_` bit.

**The reply builders.** Add to `src/net/resp.h`, each taking the flag:

```c
template <typename Buf> void reply_null(Buf&&, bool resp3);        // "_\r\n"      vs "$-1\r\n"
template <typename Buf> void reply_null_array(Buf&&, bool resp3);  // "_\r\n"      vs "*-1\r\n"
template <typename Buf> void reply_bool(Buf&&, bool v, bool resp3);// "#t\r\n"     vs ":1\r\n"
template <typename Buf> void reply_map_header(Buf&&, uint64_t n, bool resp3);   // '%' n vs '*' 2n
template <typename Buf> void reply_set_header(Buf&&, uint64_t n, bool resp3);   // '~' n vs '*' n
template <typename Buf> void reply_verbatim(Buf&&, Slice, const char ext[3], bool resp3);
template <typename Buf> void reply_double(Buf&&, double, bool resp3);           // ',' vs bulk
template <typename Buf> void reply_push_header(Buf&&, uint64_t n);              // RESP3 only
```

`reply_double` is the delicate one and it is **already correct on the payload side**: it uses
`std::to_chars` shortest-round-trip precisely because `%.17g` produced
`-0.014999999999999999` where Redis says `-0.015` and the zset differ caught 91 of them
(`src/net/resp.h:194-201`). That is the same class as Redis's `fpconv_dtoa`. Only the framing byte
changes — plus the `inf`/`-inf` special case must gain `nan` to match `d2string`
(`util.c:718-749`), and the sign-aware `-0` case must be checked against
`std::to_chars`'s output. **Do not touch the payload formatter; only add the prefix branch.**

Skip `(` bignum and `|` attribute entirely — no core command emits either
(`debug.c:909-919` and `module.c:3330-3334` are the only sites in Redis).

**Call sites to change.** The commands in the divergence table above, each in its type lane:
`t_hash.cc` (HGETALL, HRANDFIELD WITHVALUES), `t_zset.cc` (ZSCORE, ZMSCORE, ZINCRBY, ZADD INCR,
ZRANK WITHSCORE, ZPOPMIN/MAX **with count only**, all the WITHSCORES range emitters via
`emit_score_range` `t_zset.cc:1624`, ZRANDMEMBER WITHSCORES), `t_set.cc` (SMEMBERS, SPOP with
count, SINTER/SUNION/SDIFF), `t_server.cc` (CONFIG GET, COMMAND DOCS, CLIENT INFO, HELLO),
`pubsub.inc` (`pubsub_delivery_frame` `:162-175` → `>` framing, and lift the RESP2 subscriber-mode
command restriction at `io_loop.h:414-441` for RESP3 clients). Plus `xshard_commands.inc` /
`scatter_engine.inc` wherever a cross-shard command reassembles one of the above (MGET is
unaffected; the set-operation STORE forms reply integers).

**HELLO.** `cmd_hello` (`t_server.cc:~370-388`) currently emits the flat 14-element array and
`HELLO 3` returns `-NOPROTO` deliberately (`NOTES-COMPAT.md`). The change: accept 3, set the
`Client` flag, and emit the reply through `reply_map_header(sink, 7, resp3)` so the RESP3 client
gets `%7` and the RESP2 client keeps the identical `*14` it gets today. `proto` must report the
**new** value (`networking.c:5110-5112`). Preserve the bare-`HELLO`-does-not-switch rule
(`networking.c:5100-5101`).

**CLIENT TRACKING is a separate item and should not ride along.** It needs a keyspace→client
index consulted on **every write**, on the shard-owning thread, plus cross-thread invalidation
delivery to arbitrary IO threads. That is a second always-on data-path mechanism, subject to the
≤3% rule, and it is architecturally much larger than the protocol work. Ship RESP3 first;
`CLIENT TRACKING` gets its own audit.

### Knobs and reply formats

**Knobs: none.** The protocol version is negotiated per connection by `HELLO`, which is the knob.

Formats are enumerated in the divergence table above; `NOTES-COMPAT.md` should get a table of
exactly which tomokv commands change shape, because that table *is* the compatibility contract.

The current `NOTES-COMPAT.md` client matrix becomes materially better: go-redis, redis-py and Jedis
all currently need explicit `protocol=2` configuration, and redis-cli `-3` fails by design. All of
that disappears.

### Build size — **M**

~500–650 lines. Roughly: `resp.h` builders ~150; the `route_flags_` plumbing and `HELLO` ~80;
per-command divergences across five type lanes ~250 (each is a two-line branch, but there are many);
pub/sub push framing and the subscriber-mode lift ~60; `NOTES-COMPAT.md` ~50.

No new subsystem, no new cross-thread mechanism, no footprint growth. This is the **highest
value-per-line item of the eight** — it is what every modern client negotiates by default.

### Risk

Low-to-moderate, but with a wide blast radius: it touches every type lane's reply path.

1. **Silent RESP2 regressions.** Every edited call site is on a path that currently works. The
   mitigation is mechanical: the differ already runs every existing suite in RESP2, so **run the
   full existing suite set unchanged, first**, and only then add RESP3 arms. A RESP3 feature that
   breaks RESP2 is the failure mode to design against.
2. **`ZPOPMIN`/`ZPOPMAX` without COUNT stays flat** (`t_zset.c:4374-4390`). This is exactly the kind
   of asymmetry that gets "tidied" into a bug. Same for the three non-diverging commands
   (`XPENDING`, `INCRBYFLOAT`, `GEODIST`) — assert their invariance in the tests.
3. **Deferred lengths.** Redis duplicates the map/set divergence in `setDeferredMapLen` /
   `setDeferredSetLen` (`networking.c:1086-1095`). tomokv's scatter/gather paths assemble lengths
   after the fact; every such site must take the same branch, or a cross-shard RESP3 reply will
   carry a RESP2 header.
4. **The `>` push assert.** Redis hard-asserts `c->resp >= 3` on push
   (`networking.c:1286-1290`). tomokv must keep the RESP2 pub/sub framing for RESP2 clients on the
   *same* server — the protocol is per connection, so both frame shapes are live simultaneously in
   `pubsub_delivery_frame`, which currently has one shape.

**No shelve criteria.** RESP3 is the item most visible to users and the one whose absence is
currently a documented workaround in `NOTES-COMPAT.md` for three of the five surveyed clients.

### Test plan sketch

The differ is the right instrument and it needs one change: a `-3` mode that sends `HELLO 3` on both
target and oracle before the stream.

- **Regression first**: run every existing suite (`string`, `list`, `set`, `zset`, `hash`, `xshard`,
  `bitmap`, `hll`, `cgaps`) unchanged in RESP2 and require zero diffs. This is the gate on the whole
  item.
- **Then re-run every one of those suites in `-3` mode.** Because the suites already cover the
  commands that diverge, this gives near-complete RESP3 coverage for free — and it diffs against the
  real Redis oracle, which is the only trustworthy definition of the new shapes.
- Directed, in both modes: `HELLO` bare (must not switch), `HELLO 2`, `HELLO 3`, `HELLO 1`,
  `HELLO 4`, `HELLO abc`, `HELLO 3 SETNAME x`, `HELLO 3 BOGUS` (verbatim
  `Syntax error in HELLO option 'BOGUS'`), and `RESET` after `HELLO 3` (protocol behaviour on reset
  must be decided and tested).
- Directed invariance: `XPENDING`-shaped commands not implemented here are moot, but
  **`INCRBYFLOAT` and `GEODIST` must be byte-identical in both modes**, and `ZPOPMIN key` (no count)
  must be flat in both while `ZPOPMIN key 2` nests only in RESP3.
- Nulls: a missing-key `GET` (`$-1` vs `_`), a null-array reply (`LPOS ... COUNT` / `ZMPOP` with no
  candidate → `*-1` vs `_`) — this is the `addReplyNull` / `addReplyNullArray` distinction and it is
  easy to get backwards.
- Doubles: `ZSCORE` over a set of scores chosen to stress formatting — `0`, `-0`, `inf`, `-inf`,
  `1e308`, `0.015`, `-0.015`, and the values that caught the original 91 diffs.
- Pub/sub: a RESP2 subscriber and a RESP3 subscriber on the **same channel simultaneously**, each
  receiving its own framing; and a RESP3 subscriber running an ordinary `GET` while subscribed,
  which RESP2 forbids and RESP3 permits (`server.c:4759-4769`).
- Cross-shard: `MGET`, `SINTER` across shards, and `HGETALL` on each shard, in `-3` mode, to prove
  the deferred/gathered length headers took the right branch.

Then `tests/gate.sh quick`.

---

## Ranking — all eight by value / effort

| # | Item | Effort | New mechanism? | Risk | Value | Verdict |
| ---: | --- | :---: | --- | :---: | --- | --- |
| 1 | **§8 RESP3 / HELLO** | M ~600 | none | low-mod | **Highest.** Every modern client negotiates RESP3 by default; three of the five clients surveyed in `NOTES-COMPAT.md` currently need an explicit `protocol=2` workaround, and `redis-cli -3` fails by design. | **BUILD FIRST** |
| 2 | **§1 BITFIELD / BITFIELD_RO** | S ~250 | none | low | Real (bitmap-counter workloads), and the file already contains every primitive. Best pure ratio of the eight. | **BUILD** |
| 3 | **§6 CLIENT subcommands** | M ~400 (Step 0) + S/M each | client catalog → IO scatter | mod, but *reduces* net risk | High operational value: `CLIENT LIST` and `CLIENT KILL` are what an operator reaches for when a client misbehaves. Step 0 also deletes a latent `Client*`-across-threads hazard. | **BUILD** (Step 0 first; `PAUSE` optional) |
| 4 | **§5 MONITOR** | M ~400 | monitor feed on the existing IO-to-IO transport | mod | High operational value, and tomokv's shape makes it **cheaper than Redis's**: the feed is IO-side, so no executor ever learns MONITOR exists. Two documented divergences. | **BUILD** |
| 5 | **§7a DUMP / RESTORE (self-compatible)** | S–M ~300 | none — the snapshot hooks are the engine | low | Genuine utility (key copy, targeted backup, tomokv↔tomokv migration) for very little code, because `SnapshotTypeHooks` already serialises all five types. | **BUILD** |
| 6 | **§2 GEO family** | M ~1000 + S ~200 | none — a layer over the zset lane | low-mod | Entirely demand-driven. Largest *volume* of the build-now items but among the lowest risk; touches no store, loop, footprint or cross-thread mechanism. | **BUILD when demanded** |
| 7 | **§4 FUNCTION family** | M ~800 | **long-lived, GC-bearing Lua heap on EX threads** | high | Low. A management surface over an engine that already exists — everything a library does, `EVALSHA` does today. Nothing degrades without it. | **SHELVE** |
| 8 | **§3 HEXPIRE family** | **L ~2150** | per-shard field-expiry index; lazy deletion inside read paths | **highest** | Moderate and narrow (session stores, rate limiters), against a workaround that already works: one key per field with an ordinary TTL. | **SHELVE** |
| — | **§7b DUMP / RESTORE (Redis-wire)** | **L ~1500** | listpack / ziplist / intset / zipmap parsers, LZF, CRC64 | high | ~1500 lines whose only purpose is to speak another server's private on-disk format, with a permanent legacy-tag liability. | **NON-GOAL** |

### The ordering logic, briefly

The top five are separated from the bottom three by one property: **none of them adds an always-on
mechanism.** RESP3 adds a branch on a flag the Op already has room for; BITFIELD adds a handler;
CLIENT and MONITOR reuse the pub/sub transport that already exists and is already tested;
self-compatible DUMP reuses the snapshot hooks. GEO adds volume but no mechanism.

The bottom three each add a standing cost to a hot path or a resident structure — a Lua GC inside
the executor pass (§4), a lazy-deletion check on every hash read plus a footprint regression on the
mdiet lane (§3), a parser suite for a foreign format (§7b). Under the tree's own rules — ≤3% for
always-on machinery, hardcode-or-delete, and "shrink duration/breadth, never discrimination" — those
three need a named consumer before they earn their cost. None currently has one.

### Cross-item dependencies — resolve these before starting

Three items compete for the same two scarce resources. Allocate deliberately rather than
first-come-first-served:

- **`Op::route_flags_`** (`src/exec/op.h:123`) is a `uint8_t` with only bit 0 used
  (`kAtomicHazard`, `op.h:229`). §6 wants a bit for `CLIENT NO-TOUCH`; §8 wants one for RESP3.
  Proposed allocation: `bit 0 kAtomicHazard` (existing), `bit 1 kNoTouch`, `bit 2 kResp3`,
  bits 3–7 free. Neither item changes `sizeof(Op) == 336`.
- **The `Client` bool run** at `src/net/conn.h:495-504`. §5 wants a `monitor_` bit and §8 wants a
  protocol bit; `subscriber_mode_` is documented as consuming existing alignment padding
  (`conn.h:503`). Whether two more bits fit without tripping
  `static_assert(sizeof(Client) == 1984)` (`conn.h:538`) must be checked **before** either item is
  designed around it, not discovered at compile time.
- **§6 Step 0 (client catalog → IO scatter) is a prerequisite for §5's cleanest form.** MONITOR needs
  per-connection registration replicated across IO threads; the CLIENT scatter needs the same
  request/result plumbing. Build Step 0 once and both items get shorter. Doing them in the other
  order means writing the transport twice.

### Suggested sequence

1. **§8 RESP3** — highest value, no dependencies, and its regression discipline (run every existing
   differ suite in RESP2 first, then re-run them all in `-3`) is a good forcing function.
2. **§1 BITFIELD** — small, isolated, ships independently at any point.
3. **§6 Step 0** — the client catalog scatter, which removes a latent hazard and unblocks §5.
4. **§6 remainder** (`LIST` fields, `KILL`, `NO-TOUCH`, `REPLY`, error-string fixes) and **§5
   MONITOR**, which now share transport.
5. **§7a DUMP/RESTORE** — independent, cheap, can slot in anywhere.
6. **§2 GEO** — when a consumer asks.
7. **§4 and §3** — document the refusals in `NOTES-COMPAT.md` and `NOTES-HASH.md` respectively, with
   the reasoning and the shelve criteria, so the analysis is not redone from scratch later.

### One standing caveat on the oracle

`tests/differ.py` diffs against the local Redis **8.9.241** build, and that tree is not stock
upstream: `RDB_VERSION` is 15 with type tags to 33 (`rdb.h:21,55-89`), it carries a hash-template
encoding family (`object.h:89-90`) and Valkey-derived HFE internals, and its CLIENT/`catClientInfoString`
field list includes fields upstream does not emit (`io-thread`, `read-events`,
`avg-pipeline-len-*`, `omem-shared`, `omem-unshared`; `networking.c:4277-4317`). For §6 and §7 in
particular, *matching this oracle byte-for-byte is not the same as matching Redis*. Decide which one
is the target before writing the `CLIENT LIST` field list or the DUMP footer, and write the decision
down.
