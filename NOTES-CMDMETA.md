# NOTES-CMDMETA — cold command/subcommand metadata

## Outcome

This lane resolves the three requested metadata gaps without changing `CommandSpec`:

- `COMMAND LIST` now exposes the exact 129 Redis 7.4 pipe-qualified subcommand names in addition
  to TomoKV's 241 executable top-level commands.
- `COMMAND INFO <container|subcommand>` returns a first-class 10-field row with the subcommand's
  own arity, flags, ACL categories, tips, key specifications, and empty child set. Top-level INFO
  rows use the same rich metadata; populated container rows contain their child rows.
- `COMMAND GETKEYSANDFLAGS` resolves container arms and returns each key specification's own
  `RO`/`RW`/`OW`/`RM` and access/update/insert/delete intent. `COMMAND GETKEYS` shares the metadata
  extractor, preserving its key-list behavior.

`CommandSpec` remains 48 bytes. `Op` and `Client` were not changed. The implementation is a cold
side table: 370 immutable generated rows (241 implemented top-level commands plus all 129 Redis
subcommands), 48 deduplicated key-spec shapes, and one boot-built vector indexed by the existing
top-level command id. At this inventory size the only new dynamic allocation is 241 pointers
(1,928 bytes on this build). Pipe-qualified rows are metadata, not executable top-level aliases,
so `COMMAND COUNT`, dispatch, ACL command bits, and the single-owner store path retain their
existing meanings.

No runtime knob was added, so `tomokv.conf` is unchanged. No GET/SET dispatch branch or call was
added: the new code runs at registry initialization or only from the cold `COMMAND`/`ACL CAT`
introspection paths. The MVCC resolver, scatter engine, and store ownership code are untouched.

The checked-in include is produced by `tools/gen_cmdmeta.py`. The generator reads TomoKV's local
registry declarations and the public `COMMAND` reply from a running Redis 7.4 binary; it neither
reads nor copies Redis source. Rows are sorted because Redis dictionary traversal order varies
between boots.

## Live audit: every area checked

The status column describes the post-change state. “Baseline” means commit `4759a5cb6^`; it was
built from `git archive` in a temporary directory inside this worktree, run on the assigned cores
and port, and removed after its exact listening PID terminated.

| Check | Baseline TomoKV | Redis 7.4.2 reference | Post-change status |
|---|---|---|---|
| Pipe-qualified `COMMAND LIST` set | empty set | exact 129-name set in the appendix | **CONSISTENT**: target set is the same 129 names |
| `COMMAND COUNT` meaning | 241, equal to its non-pipe LIST cardinality | 250, equal to its non-pipe LIST cardinality | **CONSISTENT semantics**; raw counts still differ because of nine unrelated top-level gaps |
| `COMMAND INFO config|get` | null row | rich row shown below | **CONSISTENT**, byte-exact in RESP2 and RESP3 |
| `COMMAND INFO object|encoding` | null row | rich row shown below | **CONSISTENT**, including tip and key spec |
| Rich top-level `COMMAND INFO get` | minimal flags; no ACL categories, tips, or key specs | rich row shown below | **CONSISTENT**, byte-exact |
| Container `COMMAND INFO config` children | no child rows | five child rows | **CONSISTENT as a child-name set**; reference order varies between boots |
| Unknown `COMMAND INFO nope|nope` | null row | null row | **CONSISTENT** negative control |
| Per-key intent, 20 observed-different cases | blanket command-wide intent | per-key specification intent | **CONSISTENT**; exact matrix below |
| Existing intent controls: `GET`, `SET ... GET`, `BITFIELD ... SET` | already exact | exact | **CONSISTENT** negative controls, still exact |
| Noncanonical key counts `+1` and `01` | keys found with blanket flags | keys found with an empty flag set | **CONSISTENT** |
| Repeated `SORT ... STORE d STORE e` | first destination `d` | last destination `e` | **CONSISTENT** |
| Unknown container arm `OBJECT BOGUS k` | treated as broad `OBJECT` | invalid command | **CONSISTENT** |
| `ACL CAT stream/pubsub/admin/connection` pipe subsets | all four empty | respectively 10/5/49/25 names | **CONSISTENT** as sets; 89 pipe-category memberships exercised |
| Direct INFO rows | subcommands absent; top rows minimal | 370 generated rows considered | **CONSISTENT**: exhaustive direct-row RESP2 probe and all direct subcommand RESP3 probes had zero differences |
| 58 valid key-spec shapes | blanket intent | command-specific intent | **CONSISTENT** in the exhaustive shape probe |
| 1,550 randomized valid/invalid GETKEYS cases | existing behavior | reference | **CONSISTENT** after the shared extractor change |
| `COMMAND DOCS config|get` prose | absent for a qualified name | full human-authored Redis prose | **DIFFERS deliberately**; minimal TomoKV documentation is returned, exact boundary below |

### Exact inventory differences

`COMMAND LIST` is specified by this lane as a set comparison because both binaries emit registry
order, not a stable sorted order. The exact post-change sets are:

```text
TomoKV = 241 top-level names + the exact 129 pipe names in the appendix
Redis  = 250 top-level names + the exact 129 pipe names in the appendix
Redis-only = {cluster, migrate, module, move, pfdebug, psync, replconf, swapdb, sync}
TomoKV-only = {}
```

Those nine top-level names were classified in `NOTES-CMDGAP.md` and are not subcommand-metadata
work. Accordingly the remaining exact raw COUNT difference is:

```text
TomoKV: b':241\r\n'
Redis:  b':250\r\n'
```

Both values exactly equal their own non-pipe `COMMAND LIST` cardinality. Subcommands therefore do
not inflate COUNT on either server.

Before this lane the exact `COMMAND INFO config|get` difference was:

```text
TomoKV: b'*1\r\n$-1\r\n'
Redis:  b'*1\r\n*10\r\n$10\r\nconfig|get\r\n:-3\r\n*4\r\n+admin\r\n+noscript\r\n+loading\r\n+stale\r\n:0\r\n:0\r\n:0\r\n*3\r\n+@admin\r\n+@slow\r\n+@dangerous\r\n*0\r\n*0\r\n*0\r\n'
```

The post-change target reply is byte-for-byte the Redis line above. The other explicitly requested
subcommand example was also null at baseline; target and Redis now both return exactly:

```text
COMMAND INFO object|encoding
b'*1\r\n*10\r\n$15\r\nobject|encoding\r\n:3\r\n*1\r\n+readonly\r\n:2\r\n:2\r\n:1\r\n*3\r\n+@keyspace\r\n+@read\r\n+@slow\r\n*1\r\n$23\r\nnondeterministic_output\r\n*1\r\n*6\r\n$5\r\nflags\r\n*1\r\n+RO\r\n$12\r\nbegin_search\r\n*4\r\n$4\r\ntype\r\n$5\r\nindex\r\n$4\r\nspec\r\n*2\r\n$5\r\nindex\r\n:2\r\n$9\r\nfind_keys\r\n*4\r\n$4\r\ntype\r\n$5\r\nrange\r\n$4\r\nspec\r\n*6\r\n$7\r\nlastkey\r\n:0\r\n$7\r\nkeystep\r\n:1\r\n$5\r\nlimit\r\n:0\r\n*0\r\n'
```

Rich INFO was not limited to qualified names. `GET` demonstrates the top-level difference. The
baseline target emitted this exact minimal row:

```text
b'*1\r\n*10\r\n$3\r\nget\r\n:2\r\n*1\r\n$8\r\nreadonly\r\n:1\r\n:1\r\n:1\r\n*0\r\n*0\r\n*0\r\n*0\r\n'
```

The post-change target and Redis both emit:

```text
b'*1\r\n*10\r\n$3\r\nget\r\n:2\r\n*2\r\n+readonly\r\n+fast\r\n:1\r\n:1\r\n:1\r\n*3\r\n+@read\r\n+@string\r\n+@fast\r\n*0\r\n*1\r\n*6\r\n$5\r\nflags\r\n*2\r\n+RO\r\n+access\r\n$12\r\nbegin_search\r\n*4\r\n$4\r\ntype\r\n$5\r\nindex\r\n$4\r\nspec\r\n*2\r\n$5\r\nindex\r\n:1\r\n$9\r\nfind_keys\r\n*4\r\n$4\r\ntype\r\n$5\r\nrange\r\n$4\r\nspec\r\n*6\r\n$7\r\nlastkey\r\n:0\r\n$7\r\nkeystep\r\n:1\r\n$5\r\nlimit\r\n:0\r\n*0\r\n'
```

### Exact per-key intent differences

For compactness, the table uses exact RESP2 notation `K(key; +flag,...)`: each `K` expands to
`*2`, a bulk-string key, then an array of the listed simple-string flags; the surrounding list is
an RESP array in the displayed order. Thus `[K(a; +RO), K(b; +OW,+update)]` is an unambiguous
wire-shape abbreviation, not a set-normalized description. All baseline/reference pairs below were
observed different before the change and are byte-equal after it.

| Full command after `COMMAND GETKEYSANDFLAGS` | Exact baseline TomoKV reply | Exact Redis reply (and post-change target) |
|---|---|---|
| `ZADD z 1 m` | `[K(z; +RW,+access,+update)]` | `[K(z; +RW,+update)]` |
| `DEL a b` | `[K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(a; +RM,+delete), K(b; +RM,+delete)]` |
| `EXISTS a b` | `[K(a; +RO,+access), K(b; +RO,+access)]` | `[K(a; +RO), K(b; +RO)]` |
| `RENAME a b` | `[K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(a; +RW,+access,+delete), K(b; +OW,+update)]` |
| `MSET a 1 b 2` | `[K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(a; +OW,+update), K(b; +OW,+update)]` |
| `COPY a b` | `[K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(a; +RO,+access), K(b; +OW,+update)]` |
| `SMOVE a b m` | `[K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(a; +RW,+access,+delete), K(b; +RW,+insert)]` |
| `BITOP AND d a b` | `[K(d; +RW,+access,+update), K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(d; +OW,+update), K(a; +RO,+access), K(b; +RO,+access)]` |
| `ZUNIONSTORE d 2 a b` | `[K(d; +RW,+access,+update), K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(d; +OW,+update), K(a; +RO,+access), K(b; +RO,+access)]` |
| `GEORADIUS g 0 0 1 km STORE d` | `[K(g; +RW,+access,+update), K(d; +RW,+access,+update)]` | `[K(g; +RO,+access), K(d; +OW,+update)]` |
| `OBJECT ENCODING k` | `[K(k; +RO,+access)]` | `[K(k; +RO)]` |
| `XGROUP CREATE x g $` | `[K(x; +RW,+access,+update)]` | `[K(x; +RW,+insert)]` |
| `SET k v` | `[K(k; +RW,+access,+update)]` | `[K(k; +OW,+update)]` |
| `BITFIELD k GET u8 0` | `[K(k; +RW,+access,+update)]` | `[K(k; +RO,+access)]` |
| `SORT s STORE d` | `[K(s; +RW,+access,+update), K(d; +RW,+access,+update)]` | `[K(s; +RO,+access), K(d; +OW,+update)]` |
| `PFMERGE d a b` | `[K(d; +RW,+access,+update), K(a; +RW,+access,+update), K(b; +RW,+access,+update)]` | `[K(d; +RW,+access,+insert), K(a; +RO,+access), K(b; +RO,+access)]` |
| `ZUNION +1 a b` | `[K(a; +RO,+access)]` | `[K(a;)]` |
| `LMPOP 01 a b LEFT` | `[K(a; +RW,+access,+update)]` | `[K(a;)]` |
| `SORT s STORE d STORE e` | `[K(s; +RW,+access,+update), K(d; +RW,+access,+update)]` | `[K(s; +RO,+access), K(e; +OW,+update)]` |
| `OBJECT BOGUS k` | `[K(k; +RO,+access)]` | `b'-ERR Invalid command specified\r\n'` |

The exact raw form for four representative fixed rows was recaptured after the final build; each
target line equaled the adjacent Redis line byte-for-byte:

```text
ZADD:      b'*1\r\n*2\r\n$1\r\nz\r\n*2\r\n+RW\r\n+update\r\n'
RENAME:   b'*2\r\n*2\r\n$1\r\na\r\n*3\r\n+RW\r\n+access\r\n+delete\r\n*2\r\n$1\r\nb\r\n*2\r\n+OW\r\n+update\r\n'
MSET:     b'*2\r\n*2\r\n$1\r\na\r\n*2\r\n+OW\r\n+update\r\n*2\r\n$1\r\nb\r\n*2\r\n+OW\r\n+update\r\n'
GEORADIUS:b'*2\r\n*2\r\n$1\r\ng\r\n*2\r\n+RO\r\n+access\r\n*2\r\n$1\r\nd\r\n*2\r\n+OW\r\n+update\r\n'
```

### DOCS prose decision and exact remaining difference

Full Redis `COMMAND DOCS` is a large, human-authored, version-specific prose/history/argument
corpus. Carrying it would duplicate documentation unrelated to dispatch correctness, add ongoing
sync obligations, and risk importing licensed text. It is not needed for the three requested
behavioral fixes. TomoKV therefore extends its existing compact generated documentation to direct
subcommand names but deliberately does not embed Redis prose. This keeps `redis-cli` discovery
usable and makes the boundary explicit rather than returning an empty result.

The exact remaining reply difference is:

```text
TomoKV:
b'*2\r\n$10\r\nconfig|get\r\n*8\r\n$7\r\nsummary\r\n$36\r\ntomokv compatible config|get command\r\n$5\r\nsince\r\n$5\r\n0.1.0\r\n$5\r\ngroup\r\n$6\r\nserver\r\n$10\r\ncomplexity\r\n$37\r\nO(1) or proportional to returned work\r\n'

Redis:
b'*2\r\n$10\r\nconfig|get\r\n*12\r\n$7\r\nsummary\r\n$57\r\nReturns the effective values of configuration parameters.\r\n$5\r\nsince\r\n$5\r\n2.0.0\r\n$5\r\ngroup\r\n$6\r\nserver\r\n$10\r\ncomplexity\r\n$62\r\nO(N) when N is the number of configuration parameters provided\r\n$7\r\nhistory\r\n*1\r\n*2\r\n$5\r\n7.0.0\r\n$65\r\nAdded the ability to pass multiple pattern parameters in one call\r\n$9\r\narguments\r\n*1\r\n*8\r\n$4\r\nname\r\n$9\r\nparameter\r\n$4\r\ntype\r\n$6\r\nstring\r\n$12\r\ndisplay_text\r\n$9\r\nparameter\r\n$5\r\nflags\r\n*1\r\n+multiple\r\n'
```

## Code and scope decisions

- `src/cmd/cmdmeta.cc` is the only feature implementation file. The public header deliberately
  keeps `CommandMetadata` opaque so the generated representation cannot leak into hot dispatch.
- Startup validates that every executable registry row has exactly one top-level metadata row and
  builds the dense id-to-metadata pointer vector. Failure to allocate or a missing row terminates
  startup.
- Generic index/keyword plus range/key-count key specs cover the generated corpus. Small cold
  specializations reproduce Redis's context-dependent `SET`, `BITFIELD`, and `SORT` behavior.
- `COMMAND LIST FILTERBY ACLCAT` and `ACL CAT` read the same metadata categories. This lane does
  not add 129 executable ACL permission bits: actual command authorization remains attached to the
  container's existing dispatch row. Expanding first-argument ACL rule enforcement is outside the
  requested LIST/INFO/GETKEYSANDFLAGS work and the existing fixed ACL-bit design; it is explicitly
  handed on rather than being represented as completed.
- Metadata for `cluster|*` and `module|*` remains discoverable because the requested Redis metadata
  set includes it. Their absent parents remain non-executable, as classified in `NOTES-CMDGAP.md`.
- A populated container's child array is emitted in deterministic generated order. Live Redis
  reordered that array between boots, so tests compare its exact membership; direct qualified rows
  are stable and remain byte-compared.

## Exact commands used

Comparable release boots, repeated with `--atomic 0` and `--atomic 1`:

```text
taskset -c 0-7 ./build/tomokv --bind 127.0.0.1 --port 7530 \
  --atomic <0-or-1> --enable-debug-command yes
taskset -c 8-15 /tmp/claude-1000/redis74/src/redis-server \
  --bind 127.0.0.1 --port 7531 --save '' --enable-debug-command yes
```

Build, generator validation, directed/differential tests, and regression battery:

```text
make clean && make -j8
python3 tools/gen_cmdmeta.py --port 7531 --check src/cmd/cmdmeta_generated.inc
python3 tests/cmdmeta.py 127.0.0.1 7530
python3 tests/differ.py 127.0.0.1 7530 127.0.0.1 7531 cmdmeta 7
python3 tests/differ.py 127.0.0.1 7530 127.0.0.1 7531 cmdmeta 37
python3 tests/differ.py 127.0.0.1 7530 127.0.0.1 7531 cmdmeta 99
python3 tests/differ.py 127.0.0.1 7530 127.0.0.1 7531 cmdmeta 303 -3
python3 tests/differ.py 127.0.0.1 7530 127.0.0.1 7531 compatintro <seed>
python3 tests/servertail.py 127.0.0.1 7530 --binary ./build/tomokv \
  --cores 0-3 --spare-port 7533
```

Sanitizer build and boots (both atomic modes; the oracle stayed on port 7531):

```text
make asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
taskset -c 0-7 ./build/tomokv-asan --bind 127.0.0.1 --port 7532 \
  --atomic <0-or-1> --enable-debug-command yes
python3 tests/cmdmeta.py 127.0.0.1 7532
python3 tests/differ.py 127.0.0.1 7532 127.0.0.1 7531 cmdmeta 7
python3 tests/differ.py 127.0.0.1 7532 127.0.0.1 7531 cmdmeta 37 -3
```

Every server was resolved from its listening socket before `SIGTERM`; every assigned listener was
confirmed released before the next boot. No process-name selection was used.

## Test evidence

The required fail-before proof fired against the baseline implementation:

```text
CMDMETA FAIL: 36 checks
DIFFER cmdmeta: 4202 ops, 3636 diffs -> FAIL \
  (info=2097, intent=2103, pipes=0, categories=0, docs_boundary=0, zero_controls=2)
```

The directed test subsequently grew four edge checks. Final clean release build and generator
check both exited 0 without warnings or generated drift:

```text
make clean && make -j8
# exit 0
python3 tools/gen_cmdmeta.py --port 7531 --check src/cmd/cmdmeta_generated.inc
# exit 0
```

Release `--atomic 0`:

```text
CMDMETA PASS: 40 checks; pipes=129 rich_rows=2 intent_rows=22 edge_rows=1 controls=3
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2122, intent=2078, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 7
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2044, intent=2156, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 37
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2089, intent=2111, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 99
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2146, intent=2054, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 303, RESP3
DIFFER compatintro: 5630 checks, 0 diffs -> PASS (getkeys=1550, config_multi=631, client_rows=262, acl_mutations=401, notify_events=15, silence_controls=5)
```

Release `--atomic 1` repeated the same four cmdmeta seeds and produced the same zero-diff tails:

```text
CMDMETA PASS: 40 checks; pipes=129 rich_rows=2 intent_rows=22 edge_rows=1 controls=3
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2122, intent=2078, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 7
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2044, intent=2156, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 37
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2089, intent=2111, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 99
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2146, intent=2054, pipes=129, categories=89, docs_boundary=1, zero_controls=2)  # seed 303, RESP3
DIFFER compatintro: 5603 checks, 0 diffs -> PASS (getkeys=1550, config_multi=650, client_rows=246, acl_mutations=402, notify_events=15, silence_controls=5)
```

That is 33,616 randomized cmdmeta operations across four seeds and both atomic modes with zero
differences. The adjacent regression battery also passed:

```text
servertail: 101 checks, 0 failures -> PASS
```

`make asan` exited 0. ASAN+UBSAN `--atomic 0` ran the 40-check directed battery and seed 7; atomic
1 ran the directed battery and RESP3 seed 37:

```text
CMDMETA PASS: 40 checks; pipes=129 rich_rows=2 intent_rows=22 edge_rows=1 controls=3
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2122, intent=2078, pipes=129, categories=89, docs_boundary=1, zero_controls=2)
CMDMETA PASS: 40 checks; pipes=129 rich_rows=2 intent_rows=22 edge_rows=1 controls=3
DIFFER cmdmeta: 4202 ops, 0 diffs -> PASS (info=2044, intent=2156, pipes=129, categories=89, docs_boundary=1, zero_controls=2)
```

Both sanitizer processes were stopped by their exact listening PID and exited 0. Their shutdown
logs reported `live_conns=0`, no unsent bytes, and no AddressSanitizer, LeakSanitizer, or UBSAN
diagnostic.

No loopback performance number was requested or recorded.

## Appendix: exact 129 pipe-qualified names

| Parent | Count | Names |
|---|---:|---|
| `acl` | 13 | `acl|cat`, `acl|deluser`, `acl|dryrun`, `acl|genpass`, `acl|getuser`, `acl|help`, `acl|list`, `acl|load`, `acl|log`, `acl|save`, `acl|setuser`, `acl|users`, `acl|whoami` |
| `client` | 18 | `client|caching`, `client|getname`, `client|getredir`, `client|help`, `client|id`, `client|info`, `client|kill`, `client|list`, `client|no-evict`, `client|no-touch`, `client|pause`, `client|reply`, `client|setinfo`, `client|setname`, `client|tracking`, `client|trackinginfo`, `client|unblock`, `client|unpause` |
| `cluster` | 28 | `cluster|addslots`, `cluster|addslotsrange`, `cluster|bumpepoch`, `cluster|count-failure-reports`, `cluster|countkeysinslot`, `cluster|delslots`, `cluster|delslotsrange`, `cluster|failover`, `cluster|flushslots`, `cluster|forget`, `cluster|getkeysinslot`, `cluster|help`, `cluster|info`, `cluster|keyslot`, `cluster|links`, `cluster|meet`, `cluster|myid`, `cluster|myshardid`, `cluster|nodes`, `cluster|replicas`, `cluster|replicate`, `cluster|reset`, `cluster|saveconfig`, `cluster|set-config-epoch`, `cluster|setslot`, `cluster|shards`, `cluster|slaves`, `cluster|slots` |
| `command` | 7 | `command|count`, `command|docs`, `command|getkeys`, `command|getkeysandflags`, `command|help`, `command|info`, `command|list` |
| `config` | 5 | `config|get`, `config|help`, `config|resetstat`, `config|rewrite`, `config|set` |
| `function` | 9 | `function|delete`, `function|dump`, `function|flush`, `function|help`, `function|kill`, `function|list`, `function|load`, `function|restore`, `function|stats` |
| `latency` | 7 | `latency|doctor`, `latency|graph`, `latency|help`, `latency|histogram`, `latency|history`, `latency|latest`, `latency|reset` |
| `memory` | 6 | `memory|doctor`, `memory|help`, `memory|malloc-stats`, `memory|purge`, `memory|stats`, `memory|usage` |
| `module` | 5 | `module|help`, `module|list`, `module|load`, `module|loadex`, `module|unload` |
| `object` | 5 | `object|encoding`, `object|freq`, `object|help`, `object|idletime`, `object|refcount` |
| `pubsub` | 6 | `pubsub|channels`, `pubsub|help`, `pubsub|numpat`, `pubsub|numsub`, `pubsub|shardchannels`, `pubsub|shardnumsub` |
| `script` | 6 | `script|debug`, `script|exists`, `script|flush`, `script|help`, `script|kill`, `script|load` |
| `slowlog` | 4 | `slowlog|get`, `slowlog|help`, `slowlog|len`, `slowlog|reset` |
| `xgroup` | 6 | `xgroup|create`, `xgroup|createconsumer`, `xgroup|delconsumer`, `xgroup|destroy`, `xgroup|help`, `xgroup|setid` |
| `xinfo` | 4 | `xinfo|consumers`, `xinfo|groups`, `xinfo|help`, `xinfo|stream` |
