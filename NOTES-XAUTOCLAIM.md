# XAUTOCLAIM forward merge scan

## Premise and baseline

The premise was true; the pre-change implementation was not already a single scan.

- In the parent of `25f8dc479`, `src/cmd/t_stream_groups.cc:832-857` starts at
  `group->pending.lower_bound(start)`, examines at most `COUNT * 10` PEL entries, and calls
  `stream_object_find()` once for every examined entry (`:839-842`). The loop stops when attempts
  are exhausted, the PEL ends, or claimed plus deleted reaches `COUNT`; its iterator supplies the
  returned cursor (`:835-857`).
- `stream_object_find()` delegates each lookup to a one-record `stream_object_collect()`
  (`src/cmd/t_stream.cc:1424-1433`). That collector calls `scan_object_from()`
  (`src/cmd/t_stream.cc:1396-1421`), whose expanded-stream path performs an `upper_bound` over the
  macro-node index and decodes from the selected node (`src/cmd/t_stream.cc:372-404`). Thus this was
  a new index seek and node-prefix decode per PEL candidate, not one reusable traversal.

Let `p <= 10 * COUNT` be examined PEL entries, `B` the macro-node count, and `K` the records decoded
inside the selected node(s) for one lookup. The old expanded-stream traversal cost was
`O(log P + p * (log B + K))`, plus ownership copies for every found record (including `JUSTID` and
idle-ineligible candidates). With both stream node limits disabled, `K` can itself grow with the
stream; under the defaults it is normally bounded by the 100-entry/4096-byte node budgets.
Embedded streams use a linear packed scan per candidate, but their byte footprint is bounded.

## Why a merge is valid

- The PEL is a `std::map<StreamID, StreamPending, IdLess>`
  (`src/cmd/t_stream_groups.cc:29-37,137-143`), so iteration is ascending by `(ms, seq)`.
- XADD rejects an explicit ID at or below `last_id`, and auto-ID generation advances it
  (`src/cmd/t_stream.cc:684-721`). Records are delta-encoded from the preceding ID
  (`src/cmd/t_stream.cc:221-260`), appended to the tail node, and new node bases are appended to the
  index (`src/cmd/t_stream.cc:530-572,633-668`). The physical stream and its macro-node index are
  therefore ascending by the same ID comparison used by the PEL.
- The representation records each node's base/last IDs and keeps a sorted node index
  (`src/store/typeval.h:716-741`); existing forward readers already depend on this ordering
  (`src/cmd/t_stream.cc:372-404`).

## Change

- `stream_object_merge_scan()` exposes a narrow borrowed-entry merge surface
  (`src/cmd/t_stream.h:45-55,76-79`; `src/cmd/t_stream.cc:1436-1505`). It consumes strictly
  increasing target IDs, walks matching records forward, reports a null entry for a target absent
  from physical storage, and exposes tombstones without copying payloads. Expanded streams never
  revisit a macro node; when a sparse target passes the current node's `last_id`, the sorted node
  index jumps forward before decoding resumes.
- `XAUTOCLAIM` now supplies PEL IDs directly to that scan and applies the old mutation/stop rules in
  the visit callback (`src/cmd/t_stream_groups.cc:845-919`). `JUSTID` no longer copies field/value
  strings that it will not return. Non-`JUSTID` materializes only records that pass the idle test.
- `XCLAIM` is unchanged. Its explicit input IDs need not be ordered, so it retains independent
  lookups.

For a dense ordered PEL, the new expanded-stream traversal is
`O(log P + log B + p + R + output_bytes)`, where `R` is the physical records decoded once across
the visited nodes. Sparse jumps may add `O(q log B)`, `q <= p`, while bounding decoded gaps to the
current/target macro nodes; no earlier node is revisited.

The Redis change in PR #15505 was read only to confirm the traversal shape (one ordered iterator,
deleted-ID preservation, and a bounded sparse case). This implementation was derived from tomo's
own `Compact`/`StreamNode`/`std::map` representation and does not reuse Redis source.

## Observable-contract audit

- Deleted entry handling is unchanged: both an XDEL tombstone
  (`src/cmd/t_stream.cc:1240-1297`) and an ID physically removed by XTRIM
  (`src/cmd/t_stream.cc:821-895`) are appended to reply element three and erased from the PEL
  (`src/cmd/t_stream_groups.cc:879-884,918-919`). The latter is handled while advancing to a later
  physical ID and again at physical EOF; it is not inferred only from tombstones.
- `COUNT * 10` still bounds examined PEL candidates, and claimed plus deleted still stops at
  `COUNT` (`src/cmd/t_stream_groups.cc:849,868-875`).
- Each visit advances exactly one PEL candidate before applying the same idle/claim/delete rules
  (`src/cmd/t_stream_groups.cc:876-904`). The first unvisited iterator remains the reply cursor, or
  `0-0` at PEL end (`src/cmd/t_stream_groups.cc:910-919`).
- The reply remains `[cursor, claimed, deleted_ids]`; `JUSTID` still suppresses delivery-count
  increments and entry bodies (`src/cmd/t_stream_groups.cc:901-919`).

## Commands and measurement surface

Only `XAUTOCLAIM` can change. `XCLAIM` and every non-stream command are unchanged. In particular,
GET, SET, MGET, and MSET do not include or call this scan path; acceptance gate 2 is a formality for
this lane. Gate 1 (`XAUTOCLAIM`) is the material measurement.

Do not run these in this lane; `LANE_RULES.md` reserves all servers/tests/benchmarks for the main
session.

Correctness/differ geometry for the main session:

- One executor, one shard, one connection, one explicit-ID stream key (XAUTOCLAIM is single-key and
  therefore same-owner only). Force multiple macro nodes with more than 100 entries at default
  limits. Deliver them into one group PEL, then interleave live IDs, XDEL tombstones, and an XTRIMmed
  prefix. Compare full and `JUSTID` replies against Redis for `COUNT` 1, small counts, and terminal
  counts; assert cursor, claimed order/bodies, deleted-ID order, and the resulting XPENDING rows.
- Include a run with at least `10 * COUNT + 1` idle-ineligible PEL entries, plus missing entries near
  the attempt boundary. It must prove the exact attempt cursor and fail loudly if the PEL was not
  populated to that size.
- Existing deterministic coverage already checks interleaved tombstone/live results and trim-absent
  cleanup (`tests/streamgroups.py:182-187,205-211`). The randomized stream differ mixes
  XAUTOCLAIM, XDEL, and XTRIM (`tests/differ.py:410-445`). Run both; any wire change must fail.

Performance geometry for gate 1:

- Dense PEL: one executor, one shard, one stream key, 100k+ expanded entries all pending, explicit
  IDs, `min-idle-time=0`, `COUNT=100`; measure non-pipelined latency with one connection and
  saturated throughput with several connections. Record both full replies and `JUSTID`. Keep every
  request on the same key/owner and use the identical preloaded snapshot for A/B.
- Sparse PEL: same geometry and stream size, but leave pending IDs separated by several macro nodes;
  use a cursor that spans the stream. This checks that node-index jumps prevent a dense intervening
  stream from becoming the work surface. Fail setup if the stream did not expand to multiple nodes
  or if the requested PEL density was not established.
- Report XAUTOCLAIM throughput plus p50/p99 latency for A/B. Gate 1 requires a win in throughput,
  latency, or memory and no throughput loss. Gate 2 should still record GET, SET, MGET, and MSET on
  the program's standard geometry, but no code path from this lane reaches those commands.

## Lane-local validation

- Per binding rules, no build, server, load generator, or test script was run.
- `git diff --check` was clean after the implementation pass.
