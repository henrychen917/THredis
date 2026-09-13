# XMOVE: targeted cross-owner element mutations

## Result

The default `--atomic 0` LMOVE/RPOPLPUSH/SMOVE path no longer serializes, decodes, re-encodes,
loads, and replaces both collections. Hop one carries one selected list element (or the SMOVE
membership verdict) plus type/emptiness metadata. Hop two asks each live owner to mutate that one
element. The owner rule, two-hop barrier, phase-two admission, `state.apply[]`/`hop2[]` layout,
snapshot gates, notification batching, and AOF post-image emission are unchanged.

No `Op` or `Client` field changed; the existing footprint static assertions pass. No runtime knob
was added, so `src/core/config.h` and `tomokv.conf` need no entry. GET/SET dispatch has no new test,
allocation, or call.

## Reproduction before the change

Measured before editing, optimized HEAD, one connection, 20 warmups plus 200 sequential LMOVEs per
cell. Each measured pair alternated `LMOVE a b RIGHT LEFT` and `LMOVE b a LEFT RIGHT`, keeping both
lists at N elements. TomoKV used CPUs 32-35, port 7100, one IO plus three EX threads, three shards;
Redis 7.4.2 used CPUs 40-43, port 7101.

| elements in each list | TomoKV HEAD | Redis 7.4.2 |
|---:|---:|---:|
| 100 | 50.4 us | 30.0 us |
| 1,000 | 147.0 us | 30.9 us |
| 10,000 | 1,296.4 us | 29.9 us |

TomoKV grew 25.7x from N=100 to N=10,000; Redis stayed flat. The 10k cell reproduced the reported
millisecond-scale owner stall.

The stale-image lost update also reproduced five times out of five. A 500k-element LREM on the
destination owner held its hop-one probe while the already-enqueued source probe completed. A
concurrent `RPUSH source concurrent-N` returned 3 before LMOVE completed, but the final source was
only `[keep]`: stale hop-two replacement discarded the acknowledged push.

## Root cause

The mover arms used the generic whole-image protocol:

1. Both live owners ran `serialize_object()` over the complete source and destination.
2. `finish_phase1()` decoded both payloads into `vector<string>` (one string allocation per
   element), edited one element, then encoded both complete collections again.
3. Hop two loaded both images through the snapshot type hooks and replaced both live objects.

That is six proportional passes plus per-element allocation for a one-element operation.
`decode_elements()` also omitted the already-known `image.entries` reserve. Localfast used the same
image edit/reload algorithm, so registration through `cmd_xshard_only` did not avoid it.

## Design

- `xshard_peek_list()` and `xshard_set_contains()` perform bounded hop-one probes in their owning
  type lanes. The selected list bytes use the existing cold `ResultHeap`; SMOVE uses the existing
  result integer. Destination hop one carries only presence/type.
- `xshard_push_list_element()` / `xshard_remove_list_element()` and their set siblings mutate the
  live owner in hop two, reusing the list deque/compact and set compact/hashtable rules. Compact
  conversions remain bounded by the configured compact thresholds; large list/set mutations are
  O(1) in the uncontended case.
- List removal verifies that the live edge is the element hop one selected. A concurrent push on
  the selected edge therefore is not accidentally popped. On mismatch, the cold concurrency-only
  fallback removes the closest matching occurrence from that edge; the ordinary path stays one
  edge read plus one edge pop.
- Live insert/erase continues through `Shard::store_*<kNotify>`, preserving generic new/delete
  events. Existing cross-shard completion emits the type event. `tests/notify.py` passed with 1,608
  fired events.
- `decode_elements()` now reserves `image.entries`, reducing allocator churn for the remaining
  generic image users and the deliberately retained atomic-on path.

The acknowledged concurrent push is now retained. The directed battery proves both an opposite
edge push (`[keep, concurrent]`) and a selected-edge push (`[new-edge, keep]`). Its negative control
runs without the destination hold and requires the window detector to report zero before the
positive cells.

## Performance after the change

Same optimized topology and procedure as the reproduction:

| elements in each list | TomoKV fixed | Redis 7.4.2 |
|---:|---:|---:|
| 100 | 37.8 us | 30.1 us |
| 1,000 | 40.9 us | 30.8 us |
| 10,000 | 37.2 us | 30.2 us |

The TomoKV N=10,000 cell improved 34.9x and is 1.23x Redis instead of 43.4x in my reproduced HEAD
cell. N=10,000/N=100 is 0.98, so the collection-size slope is gone.

### INDICATIVE p32 plain-path guard

Loopback only, not a NIC result. Final binary on CPUs 32-39 (`4 io + 4 ex`, 16 shards), memtier on
CPUs 40-47, atomic off, 100k/100k keys populated, 32-byte values, 64 connections, pipeline 32,
three 4-second runs:

| command | runs (ops/s) | median | reported p99 at median |
|---|---|---:|---:|
| SET | 7,998,864; 8,040,772; 8,053,704 | 8.041 M/s | 3.023 ms |
| GET | 9,016,278; 9,025,812; 9,040,898 | 9.026 M/s | 2.735 ms |

This is an absolute guard, not an A/B claim. The source diff touches only xshard, list, and set type
lanes plus tests/notes; it does not touch IO dispatch or `t_string.cc`. The final clean build also
retains the footprint static assertions. Together those checks show the plain path did not acquire
feature machinery.

## Atomic-mode scope decision

Stage two (`--atomic 1`) is intentionally documented and unshipped. `is_atomic_apply_kind()` sends
these movers through epoch MVCC, where every installed candidate must be a deep clone so a protected
predecessor is never mutated in place. Reusing the atomic image path remains correct and avoids
introducing a second candidate builder without a distinct asymptotic win: even clone-then-mutate
must walk the complete collection.

Measured optimized atomic-on evidence was 53.4 us at N=100 and 1,253.0 us at N=10,000 (23.45x).
The directed battery runs all ordinary semantics in atomic mode, records that ratio, and explicitly
does not claim the atomic-off ratio/concurrency improvement. Implementing a type-specific MVCC
clone-then-mutate candidate is the remaining stage-two work.

Same-shard localfast also retains its image implementation. It is not the reproduced two-owner
head-of-line cliff, and replacing it safely needs a one-owner preflight that preserves the current
strong OOM behavior across source and destination. Same-key LMOVE/RPOPLPUSH/SMOVE semantics remain
covered in both atomic modes.

## Verification commands and evidence

Standing scratchpad note: `scratchpad/wave3/PREAMBLE.md` was requested but is absent from this
worktree and its parent project directories. The complete rules in the lane request were followed.

Build:

```text
make -j12 clean && make -j12
make asan
```

Representative optimized boots:

```text
taskset -c 32-35 build/tomokv --port 7100 --bind 127.0.0.1 \
  --place ifid@32,ex@33,ex@34,ex@35 --shards 3 --atomic 0 \
  --appendonly no --enable-debug-command yes
taskset -c 40-43 /tmp/claude-1000/redis74/src/redis-server \
  --port 7101 --bind 127.0.0.1 --save '' --appendonly no --daemonize no
python3 tests/xmove.py 127.0.0.1 7100
```

The target was rebooted with `--atomic 1` and the same battery repeated. Final tails:

```text
atomic 0:
  ok   targeted source removal retains concurrent push
  ok   forced inter-hop window fired and retained the unrelated write
  ok   targeted source removal preserves a new edge element
PASS xmove atomic=0

atomic 1:
  info LMOVE N=100 53.4 us N=10000 1253.0 us ratio=23.45
  skip atomic-on ratio bound: MVCC deep-candidate stage two is documented scope
  skip concurrent inter-hop preservation: atomic-on mover stage two is documented scope
PASS xmove atomic=1
```

Differential commands, repeated after both atomic-mode boots:

```text
for suite in xshard string list; do
  for seed in 7 29; do
    python3 tests/differ.py 127.0.0.1 7100 127.0.0.1 7101 "$suite" "$seed"
  done
done
for seed in 7 29; do
  python3 tests/differ.py 127.0.0.1 7100 127.0.0.1 7101 xmove "$seed"
done
```

Tail for each atomic mode:

```text
DIFFER xshard: 4276 ops, 0 diffs -> PASS
DIFFER xshard: 4276 ops, 0 diffs -> PASS
DIFFER string: 4033 ops, 0 diffs -> PASS
DIFFER string: 4033 ops, 0 diffs -> PASS
DIFFER list: 3521 ops, 0 diffs -> PASS
DIFFER list: 3521 ops, 0 diffs -> PASS
DIFFER xmove: 4263 ops, 0 diffs -> PASS
DIFFER xmove: 4263 ops, 0 diffs -> PASS
```

The new feature-specific differ stream exceeds 4,000 operations and heavily weights LMOVE,
RPOPLPUSH, and SMOVE across random long keys, same keys, missing keys, and wrong types.

Sanitizer boots used:

```text
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
taskset -c 32-35 build/tomokv-asan --port 7100 --bind 127.0.0.1 \
  --place ifid@32,ex@33,ex@34,ex@35 --shards 3 --atomic {0,1} \
  --appendonly no --enable-debug-command yes
python3 tests/xmove.py 127.0.0.1 7100
```

Both sanitizer batteries printed `PASS xmove`; both servers terminated with exit 0 and no
AddressSanitizer, LeakSanitizer, or UBSan diagnostic. `python3 tests/notify.py 127.0.0.1 7100`
also passed:

```text
notify: ok (notify_events_fired=1608)
```

Every server was resolved from its listening socket before signaling its exact PID, and ports
7100-7105 were confirmed released at the end.
