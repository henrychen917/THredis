# Armed local-read capture-at-prefetch audit

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Baseline: `b7fd92c0736618c719f29d0202aea009edc6adcb`.

## Phase 1 finding: execute reloads the slot

The armed point-read batch in `ExLoopT::drain_local_reads()` has separate prefetch and execute
walks, but the prefetch walk does not retain a lookup result.  `FlatStore::read_local_prefetch()`
only snapshots eligible table topology and issues a hardware hint for each table's home slot.  The
later `prepare_local_read()` call invokes `FlatStore::read_local_probe()`, whose open-addressing
walk acquire-loads the slot words at execute time and selects the `KvObj` then.

Consequently, a plain cross-client writer can replace a prefetched slot between those walks.  Such
an immutable-object replacement deliberately does not advance the read-local table generation, so
execute accepts the newer pointer and can stall on an object that the batch did not prefetch.  The
same shape exists inside MGET for its at-most-32-key prefetch window.

This is not an ordering requirement.  Parser-side write-ring and owner-tail conflict checks keep a
connection's own unresolved writes out of the local execution path.  A read racing only another
connection's committed replacement may place its read cut before that replacement and serve the
already-observed immutable object.

## Capture constraints

Capture must perform the complete validated probe at prefetch time, not retain only the home word:
linear probing must still skip tombstones and tag/key mismatches, stop at empty, and search the
current table before the old table.  The capture therefore consists of the observed slot address,
the `KvObj` pointer decoded from that slot (or a miss), and the store publication generation.
Execute may read the captured object directly but must not reload the slot.

The capture is safe only as stack-local state for one bounded local-drain call.  Every such call
runs inside the fused pass's rotation boundary; its `ThreadCtx` tick is published only after the pass
returns.  Writers unlink first and defer both displaced objects and retired tables until every
active participant publishes a tick newer than the retirement stamp.  Thus the captured slot/table
and object remain allocated through execute, while the final publication validation still rejects
an open atomic group, table resize/move, bulk clear, or ownership handoff.  Plain object replacement,
DEL, and TTL replacement leave that generation unchanged and may legally be observed on either
side of the read cut.

Every later batch must probe and capture afresh; no capture may be stored in the persistent local
lane or survive a rotation boundary.  Stable misses retain their existing command behavior: GET
demotes to the owner, while MGET emits a nil element.

## Phase 2 implementation

`--read-local-prefetch-capture 1` (the default) keeps the legacy home-slot hint pass, then captures
the exact result of a complete open-address probe: result, slot address, decoded immutable `KvObj`
pointer, and publication state.  Execute copies from that captured object without loading through
the slot.  `--read-local-prefetch-capture 0` selects the old hint-only prefetch plus execute-time
probe for A/B measurement.

Point-only client chunks use three passes so slot and object latency can overlap across the batch:
I0 hints home words, C0 captures and hints the selected objects/values, and E0 copies the captured
versions.  Mixed GET/MGET chunks capture and consume commands in connection order.  MGET processes
arbitrary key counts in bounded 32-key I0/C0/E0 windows and rebuilds all of its window captures on
an internal retry.  All capture buffers are stack locals inside the bounded drain and are gone
before the enclosing rotation publishes its QSBR tick.

The post-copy publication validation remains unchanged in purpose.  It rejects pending atomic work
and completed pending-bit intervals, table resize/move, clear, and ownership changes, while stable
immutable SET/DEL/TTL slot swaps remain intentionally invisible.  Same-connection write-ring and
owner-tail gates continue to keep RYOW reads out of this local path; only cross-client publication
can replace a captured object between C0 and E0.
