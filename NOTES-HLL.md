# HyperLogLog lane notes

## Stored representation is the contract

`PFADD`, `PFCOUNT`, and `PFMERGE` use Redis's HLL string format directly. An HLL remains
`Type::String`; no `Type`, `Enc`, collection wrapper, pointer graph, or alternate native sketch was
added. Consequently `GET`, `STRLEN`, ordinary string snapshots, and reloads observe and preserve the
same bytes Redis does:

```text
[ "HYLL" ][ encoding ][ 3 reserved bytes ][ little-endian cached cardinality ][ registers ]
     4          1                 3                         8                    variable
```

Encoding 0 is the fixed 12,304-byte dense image: a 16-byte header followed by 16,384 packed 6-bit
registers, least-significant bits first. Encoding 1 is Redis's positional ZERO/XZERO/VAL opcode
stream. A new empty HLL is the exact 18-byte sparse image. Sparse writes use Redis's opcode split
and five-opcode VAL coalescing order, then promote when a value exceeds sparse's 32 limit or the
result would exceed the default 3,000-byte `hll-sparse-max-bytes` ceiling. Promotion is one-way.

Elements use Redis's endian-neutral MurmurHash64A with seed `0xadc83b19`. Cardinality uses the same
16,384-register histogram and Ertl sigma/tau estimator. Dense register order, sparse mutation order,
rounding, and the cached-cardinality bytes are therefore deterministic and byte-comparable to the
oracle. The high bit of header byte 15 invalidates the cache; PFADD and PFMERGE set it after a
change, while single-key PFCOUNT recomputes and writes the little-endian cache value.

This is intentionally a foreign-format port rather than a native representation. Redis
`src/hyperloglog.c` is cited in `src/cmd/hll.cc` because format fidelity is more important here than
inventing a locally convenient layout: the stored bytes are a public interoperability surface.

## Owner command paths

- `PFADD key element [element ...]` is an ordinary owner write and is registered `DenyOom`. It
  copies the current string image, applies every element, and installs a changed image through the
  common string funnel. Duplicates return 0 and do not rewrite the image. An existing TTL is kept.
- Single-key `PFCOUNT key` is an ordinary owner read. A valid cache answers without decoding. An
  invalid cache is recomputed and installed through the string funnel while preserving TTL, matching
  Redis's logically-readonly cache mutation. Its internal `SnapshotWrite` flag captures a pre-image
  when a snapshot is active without changing the public readonly command metadata.
- `PFADD` and `PFCOUNT` reject non-string values with the normal Redis WRONGTYPE reply. A string that
  is not a valid HLL header gets
  `WRONGTYPE Key is not a valid HyperLogLog string value.` A structurally valid header whose sparse
  opcode stream is corrupt gets `INVALIDOBJ Corrupted HLL object detected` when decoded.

The string snapshot hook already saves raw/external strings byte-for-byte. Since HLLs use that
unchanged hook, snapshots and reloads preserve the HYLL representation and cache bytes without an
HLL-specific dump codec.

## SCATTER-V2 union and merge

Multi-key PF commands deliberately bypass same-owner localfast so their public execution shape is
always the requested SCATTER-V2 client:

```text
PFCOUNT k1 k2 ...
  owners gather private string images
             -> last completer validates + register-wise MAX + estimates
             -> IO emits one integer reply

PFMERGE dst src1 ...
  owners gather dst and every source image
             -> last completer validates + register-wise MAX
             -> recreates Redis's destination image from dst's current bytes
             -> hop 2 installs dst through the string funnel
             -> IO emits OK
```

Missing keys contribute empty registers. PFMERGE includes the destination in hop 1 because Redis
merges argv 1 through the last source; a missing/all-empty merge still writes the exact empty HLL
image with an invalid cache. If any input is dense, the result is dense. Otherwise the coordinator
starts from the destination's current sparse bytes and raises registers in ascending order, which
preserves Redis's exact in-place sparse normalization rather than merely producing an equivalent
sketch. Destination TTL is carried to hop 2.

PFMERGE is registered `DenyOom`. Scatter tasks use the existing insert-level maxmemory admission on
the destination apply hop, as other SCATTER-V2 whole-image writes do. The two-hop barrier prevents a
later command on the same connection from overtaking destination installation. As with the existing
xshard layer, source validation and destination apply are separate and not cross-shard atomic.

The gather keeps HLLs as `ObjectImage` strings; no `KvObj` crosses owners. Coordinator scratch is a
fixed 16,384-byte raw-register array. PFCOUNT discards it after estimating. PFMERGE serializes one
result string into its destination apply image and the owner creates the final `KvObj` through
`xshard_store_string(..., integer_encode=false)`.

## Differential coverage

`gen_hll` in `tests/differ.py` covers:

- randomized PFADD streams with within-call and across-call duplicates;
- byte-exact single-key and multi-key PFCOUNT replies on identical inputs;
- GET/STRLEN comparisons after sparse mutations, cache fills, and dense promotion;
- the default 3,000-byte sparse-to-dense transition and exact 12,304-byte dense image;
- sparse and dense PFMERGE results with overlapping sources, a pre-existing destination, missing
  sources, and destination-only merge;
- normal WRONGTYPE, integer/raw invalid-HLL WRONGTYPE, and corrupt sparse INVALIDOBJ paths.

The suite uses smaller transport batches because its directed PFADDs carry many arguments and its
GET replies carry full dense images; this keeps the differential focused on HLL semantics rather
than the server's unrelated fixed request-buffer rollover.
