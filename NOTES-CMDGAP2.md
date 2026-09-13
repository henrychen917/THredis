# NOTES-CMDGAP2 — second live command-inventory lane

## Outcome

The required live comparison reproduced the handoff exactly: the starting TomoKV binary reported
`COMMAND COUNT` 241, vanilla Redis 7.4.2 reported 250, and the reference-only top-level set was the
nine names below. One command is complete and self-contained:

- `PFDEBUG ENCODING|DECODE|GETREG|TODENSE <key>` now operates on the existing byte-compatible HLL
  image on exactly one shard owner. The observed reply types, sparse opcode text, 16,384-register
  result, sparse-to-dense mutations, TTL retention, type errors, corruption errors, subcommand
  errors, and arity edges byte-match the running reference.

The post-change live counts are 242 versus 250. There are eight reference-only top-level names and
no TomoKV-only names. Each remaining name needs an absent database, cluster, module, outbound-I/O,
or replication model; none was padded into the registry with a partial success/error subset.

No runtime knob was added, so `tomokv.conf` is intentionally unchanged. `sizeof(Op)` and
`sizeof(Client)` are unchanged and their build-time assertions pass. The ordinary GET/SET path has
no new branch, allocation, or call: `PFDEBUG` is a new cold registry family appended after every
existing family, the registry remains at its existing 512-slot capacity, and existing rows retain
their insertion/probe order. Work and allocation occur only when `PFDEBUG` is invoked.

## Live inventory method

Both binaries used this lane's assigned cores and ports. The reference and target had matching
debug/save posture; `--atomic` was varied on the target for the required two-mode runs.

```text
taskset -c 16-23 ./build/tomokv --bind 127.0.0.1 --port 7540 \
  --atomic 0 --enable-debug-command yes
taskset -c 24-31 /tmp/claude-1000/redis74/src/redis-server \
  --bind 127.0.0.1 --port 7541 --save '' --enable-debug-command yes

/tmp/claude-1000/redis74/src/redis-cli --raw -p 7540 COMMAND LIST \
  | awk 'index($0,"|")==0' | LC_ALL=C sort -u > target.list
/tmp/claude-1000/redis74/src/redis-cli --raw -p 7541 COMMAND LIST \
  | awk 'index($0,"|")==0' | LC_ALL=C sort -u > oracle.list
comm -23 oracle.list target.list
```

The baseline `comm` output was:

```text
cluster
migrate
module
move
pfdebug
psync
replconf
swapdb
sync
```

The post-change output is the same list without `pfdebug`. Every server was identified from its
listening socket and terminated by that exact PID. Ports 7540 and 7541 were confirmed released at
the end.

## All nine checks and exact replies

The byte strings below came from the two running binaries. `\r\n` and trailing spaces in unknown-
command replies are shown explicitly.

| Command | Probe | Redis 7.4 exact reply | TomoKV exact reply | Result and disposition |
|---|---|---|---|---|
| `CLUSTER` | `CLUSTER` | `-ERR wrong number of arguments for 'cluster' command\r\n` | `-ERR unknown command 'CLUSTER', with args beginning with: \r\n` | **DIFFERS; handed on.** A complete container needs the cluster slot map, node IDs, cluster bus, epochs, failover, and all 28 live subcommands. The server deliberately has no cluster model. |
| `MIGRATE` | `MIGRATE` | `-ERR wrong number of arguments for 'migrate' command\r\n` | `-ERR unknown command 'MIGRATE', with args beginning with: \r\n` | **DIFFERS; handed on.** Success needs owner-safe DUMP/RESTORE/DEL coordination, outbound authenticated networking, timeouts, and the multi-key `KEYS` form. That is not a local command and would require a dedicated coordinator/scatter lane. |
| `MODULE` | `MODULE` | `-ERR wrong number of arguments for 'module' command\r\n` | `-ERR unknown command 'MODULE', with args beginning with: \r\n` | **DIFFERS; handed on.** The five live subcommands require a module ABI, dynamic loading/unloading, configuration, command/type registration, and lifecycle tracking. None exists here. |
| `MOVE` | `MOVE cmdgap2:missing 1` | `:0\r\n` | `-ERR unknown command 'MOVE', with args beginning with: 'cmdgap2:missing' '1' \r\n` | **DIFFERS; handed on.** TomoKV exposes one flat keyspace and only `SELECT 0`; MOVE success requires a second database and atomic ownership across the source/destination database maps. |
| `PFDEBUG` | `PFDEBUG ENCODING cmdgap2:missing` | `-ERR The specified key does not exist\r\n` | baseline: `-ERR unknown command 'PFDEBUG', with args beginning with: 'ENCODING' 'cmdgap2:missing' \r\n`; post-change: `-ERR The specified key does not exist\r\n` | **DIFFERS before; resolved and CONSISTENT after.** The complete observed four-subcommand surface is implemented and differentially covered. |
| `PSYNC` | `PSYNC` | `-ERR wrong number of arguments for 'psync' command\r\n` | `-ERR unknown command 'PSYNC', with args beginning with: \r\n` | **DIFFERS; handed on.** Valid execution needs replication IDs, offsets, backlog lookup, partial/full resynchronization, and a connection transition into replication streaming. |
| `REPLCONF` | `REPLCONF` | `+OK\r\n` | `-ERR unknown command 'REPLCONF', with args beginning with: \r\n` | **DIFFERS; handed on.** This configures per-connection replica capabilities and state, including ACK/GETACK behavior. Returning a broad no-op `+OK` without the replication state consumed by PSYNC/SYNC would be a partial, misleading implementation. |
| `SWAPDB` | `SWAPDB 0 0` | `+OK\r\n` | `-ERR unknown command 'SWAPDB', with args beginning with: '0' '0' \r\n` | **DIFFERS; handed on.** General behavior swaps two database maps and reconciles clients watching or blocking on both. TomoKV has one database, so implementing only the `0 0` no-op would advertise unavailable semantics. |
| `SYNC` | `SYNC extra` | `-ERR wrong number of arguments for 'sync' command\r\n` | `-ERR unknown command 'SYNC', with args beginning with: 'extra' \r\n` | **DIFFERS; handed on.** The valid zero-argument command begins a full dataset replication stream and changes connection ownership/mode. There is no replication streaming subsystem. |

`COMMAND LIST` membership after the change is **CONSISTENT** for `PFDEBUG` and still **DIFFERS** for
the eight explicitly handed-on rows. The 241 names already common before this lane were not claimed
as semantically re-audited here.

## PFDEBUG semantics observed and implemented

- The command has exactly three arguments. Missing and extra arguments use the ordinary
  `pfdebug` arity error.
- Key lookup and HLL validation precede subcommand selection: a missing key returns
  `ERR The specified key does not exist`, a non-string uses the ordinary WRONGTYPE reply, and a
  string without a valid HYLL header uses the HLL-specific WRONGTYPE reply.
- `ENCODING` returns a RESP simple string (`+sparse` or `+dense`), not a bulk string.
- `DECODE` renders physical sparse opcodes as lowercase `z:length` for ZERO, uppercase `Z:length`
  for XZERO, and lowercase `v:value,length` for VAL. Dense images return
  `ERR HLL encoding is not sparse`.
- `GETREG` returns exactly 16,384 RESP integers. On a sparse image it first validates and rewrites
  the stored image as dense; the directed battery proves the encoding changed and the logical
  register values survived.
- `TODENSE` returns `1` when it converts a sparse image and `0` for an already-dense image. The
  rewrite preserves TTL and produces the 12,304-byte HYLL dense image.
- Incomplete/corrupt sparse streams remain visible to `ENCODING` and physical `DECODE`, but
  `GETREG` and `TODENSE` return `INVALIDOBJ Corrupted HLL object detected` when logical decoding
  cannot cover all registers. For a truncated XZERO, `DECODE` safely reproduces the reference's
  diagnostic use of the string terminator as the missing low byte (`Z:1`) without reading beyond
  the Slice.

## Design

- `src/cmd/pfdebug.cc` owns the handler pair and its one registry row. Routing hashes argv 2 and
  dispatches only to that key's owner; no store is touched outside its executor thread.
- The clean and notification-aware handlers are compile-time specializations. Sparse promotion
  uses the existing string-store helpers, preserves the object's deadline, and retains the normal
  owner-side maxmemory/MVCC/AOF path.
- `src/cmd/hll.cc` exposes two representation helpers: validated sparse-to-dense conversion and
  physical sparse-opcode formatting. Existing PFADD/PFCOUNT/PFMERGE behavior is unchanged.
- The generated Redis 7.4 ACL mask is `write hyperloglog admin slow dangerous`, and the generator's
  source inventory includes the new family.
- Invoked cost is intentionally diagnostic rather than hot-path optimized: `ENCODING` is O(1),
  `DECODE` is O(sparse bytes), and `GETREG`/sparse promotion are O(16,384 registers) with a cold
  12,304-byte dense image allocation.

## Test evidence

The detector failed before implementation. The initial directed form had 33 checks; later oracle
probing split GETREG and TODENSE into independent mutation arms and added the truncated-XZERO edge,
raising the final form to 41.

```text
CMDGAP2 FAIL: 33 checks, 22 failures
DIFFER cmdgap2: 4229 ops, 2884 diffs -> FAIL
```

Build and generated metadata validation:

```text
make clean && make -j8
# exit 0
python3 tools/gen_acl_categories.py --check src/cmd/acl_categories_generated.h
# exit 0
make asan
# exit 0
```

Final release, `--atomic 0`, seeds 101/202 and RESP3 seed 303:

```text
CMDGAP2 PASS: 41 checks; PFDEBUG sparse decode/getreg and TODENSE rewrite fired
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
```

Final release, `--atomic 1`, the same seeds:

```text
CMDGAP2 PASS: 41 checks; PFDEBUG sparse decode/getreg and TODENSE rewrite fired
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
```

That is 25,446 finalized release differential operations across three seeds and both atomic modes,
with zero diffs. Relevant regression tails were also clean:

```text
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
DIFFER hll: 3057 ops, 0 diffs -> PASS
python3 tests/acl_categories.py 127.0.0.1 7540
# exit 0 in both atomic modes
```

ASAN+UBSAN ran the finalized 41-check battery and a full differ seed in each mode:

```text
--atomic 0, seed 404:
CMDGAP2 PASS: 41 checks; PFDEBUG sparse decode/getreg and TODENSE rewrite fired
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS

--atomic 1, seed 405:
CMDGAP2 PASS: 41 checks; PFDEBUG sparse decode/getreg and TODENSE rewrite fired
DIFFER cmdgap2: 4241 ops, 0 diffs -> PASS
```

Both sanitizer logs were checked after the exact listening PID terminated. Neither contained an
AddressSanitizer, LeakSanitizer, nor UBSAN runtime report. No loopback performance number was taken;
there is no INDICATIVE measurement to report.
