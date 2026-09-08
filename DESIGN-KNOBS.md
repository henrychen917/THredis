# TomoKV configuration: operator and researcher audit

> Integration update (2026-09-08): [ORTHOG.md](ORTHOG.md) supersedes this branch report
> for combined-mode support, INFO activation evidence, layout accounting, and validation.
> Both modes now support both local-read settings with either overlap/reorder setting;
> `Config` retains the required 624-byte stride. Historical branch findings below remain intact.

2026-09-08, `cx-overlap`, on `c8e61f646` plus the incoming worktree changes. This replaces the
previous “currently swept” retention test. A control stays if an operator can make an observable
production decision with it **or** its sweep answers a needed paper question. An existing harness
loop is neither necessary nor sufficient evidence. No build, server, benchmark, or gate was run.

The encoding deletion was wrong. Seven reference controls now replace the eight old compact
controls; the two list axes become one signed setting. Boot, CONFIG GET/SET, aliases, REWRITE,
collection mutations and recovery consume them. Defaults retain the incoming limits.

**Scripting remains unresolved, explicitly:** the deleted instruction control passes OPERATOR,
but its reference-name replacement cannot preserve today's execution semantics. Redis's
`busy-reply-threshold` (alias `lua-time-limit`) is a soft elapsed-time threshold, not an instruction
abort limit. This diff does not install a misleading alias or silently change the default. The
maintainer has been asked to choose between preserving the abort and implementing Redis's
continuing-script/BUSY/KILL behavior. The exact incompatibility and remaining work are below.

## Inventory and verdicts

The incoming parser has **64 controls**, excluding `--help`; all appear individually below.
The seven restorations bring it to **71**, plus five Redis encoding aliases. The positional config
path, conf-only `pin` spelling and the one accepted environment variable are accounted for
separately. Prefix table names with `--` on the CLI; conf files omit that prefix. CONFIG support
and mutability are stated independently of CLI acceptance.

No remaining incoming control fails both lenses. In particular, production compatibility keeps
controls whose performance sweep would be redundant. This is not a requirement to manufacture
another deletion. The NEITHER ledger records the already removed implementation choices and
duplicate spellings, with their fixed behavior. No speculative performance winner is selected.

“Gap” means a question needs a completed, attributable paper experiment; a correctness battery
or a list of planned cells does not fill it. Every RESEARCH/BOTH row names its question.

| Control | Default / accepted values | Verdict | Operator decision and/or paper question | Observation / experiment status |
| --- | --- | --- | --- | --- |
| `thread-mode` | `2s`; `1s`, `2s`, aliases `fused`, `split` | BOTH | Operator chooses a worker architecture for their latency/load shape. **Question:** when does sharing parsing/execution/replies beat dedicated IO and executor roles? | Boot; INFO mode and role counts. Match total cores and offered load. |
| `overlap` | `0`; `0\|1` | RESEARCH | **Question:** how much latency/throughput improvement comes from overlapping independent work at each pipeline depth in each architecture? | Boot; INFO `overlap`. **Gap:** complete [OVERLAP.md](OVERLAP.md)'s measured factorial cells. |
| `read-local` | `0`; `0\|1` | BOTH | Operator trades read latency against immutable-write cost on read-heavy fused workloads. **Question:** when do avoided hand-offs repay the local-read lane's writer cost? | Boot; effective INFO `read_local`, lane/fallback counters. **Gap:** mixed workloads across both overlap arms. In 2s requested 1 is inert. |
| `reorder` | `0`; `0\|1` | RESEARCH | **Question:** can cross-connection reordering improve short-request tails without unacceptable long-request delay or throughput loss? | Boot; INFO and permutation witnesses. **Gap:** [REORDER.md](REORDER.md)'s mixed-size latency study, including long-request tails. |
| `atomic` | `0`; `0\|1` | BOTH | Operator selects cross-shard command atomicity. **Question:** what is the cost of group-scoped MVCC as fan-out and contention grow? | Live CONFIG; effective INFO and atomic counters. Compare competitors with equivalent semantics; OFF is only an explicitly labelled ablation. |
| `key-lb` | `1`; `0\|1` | BOTH | Operator permits or stops autonomous shard movement. **Question:** when does key-skew correction repay migration/drain/cache-warmup cost? | Boot; INFO LB, current shard owners. **Gap:** independent key/client 2×2 skew and phase-change cells. |
| `client-lb` | `1`; `0\|1` | BOTH | Operator permits or stops connection movement. **Question:** how much imbalance is caused by connection placement after key ownership is held fixed? | Boot; INFO LB and client transfers. **Gap:** same independent 2×2, including hot clients with uniform keys. |
| `flip-auto` | `0`; `0\|1` | BOTH | Operator chooses adaptive IO/EX roles versus a stable manual split. **Question:** does adaptation outperform a fixed split on changing workloads after transition costs and false triggers are charged? | Boot, 2s; FLIPCTL/INFO. Include stationary controls and phase duration; a correctness flip is not a performance result. |
| `shards` | `-1` auto or `1..256`; auto `min(8*initial_executors,256)` | BOTH | Operator chooses shard inventory/migration granularity. **Question:** where does finer partitioning stop helping balance and start costing memory or scatter overhead? | Boot; actual INFO `shards`. **Gap:** shard count independent of executor count, not just the automatic diagonal. |
| `ratio` | Unset: even split of allowed CPUs; positive `io:ex` | BOTH | Operator allocates the CPU budget between network and execution. **Question:** how does the optimal split change with depth, value size and read/write mix? | Boot, 2s; INFO current roles and FLIPCTL. Hold placement rules and total CPUs constant. |
| `place` | Unset; explicit `ifid@cpu,ex@cpu,...` | BOTH | Operator isolates CPUs and chooses locality. **Question:** how much of the observed scaling limit is cross-CCX traffic rather than additional worker count? | Boot; INFO `thread_cpus`, DEBUG roles. **Gap:** matched counts on one CCX versus multiple L3 domains/SMT. |
| `shard-home` | Unset: round-robin; complete `shard:executor_tid,...` | RESEARCH | **Question:** does dispatch cost grow with idle executor inventory when the number of active owners stays fixed? | Boot; INFO initial/current maps and DEBUG inventory. Restored `xshard_dispatch_scale` guard; paper profile/rates still required. |
| `no-pin` | Flag absent; valueless flag disables worker affinity | OPERATOR | Let the OS/cgroup scheduler place workers instead of pinning each worker to one CPU, e.g. when sharing a CPU quota. | Boot; INFO `pin_threads`. This cannot be expressed by a different pinned `place` list. |
| `hash` | `mix64`; `mix64\|siphash` | OPERATOR | Choose hash-flood resistance for untrusted keys versus the measured benign-key cost. | Boot; INFO reports the active hash, including recovered hash material. Repeating a known two-hash point-rate comparison is not itself a paper question. |
| `net-io` | `uring`; `uring\|epoll`, case-insensitive | BOTH | Operator can run on hosts where io_uring is unavailable/disallowed. **Question:** how much gain is attributable to the event engine versus the IO/EX architecture? | Boot; CONFIG/INFO and `multiplexing_api`. **Gap:** matched architecture/engine ablation with persistence disabled; this knob also selects persistence IO. Fused overlap requires uring. |
| `zc-min` | `16384`; uint32 bytes, `0` off | BOTH | Operator chooses the copy/borrow crossover for reply sizes and outstanding memory. **Question:** where does zero-copy's fixed ownership cost amortize on the actual NIC path? | Live CONFIG and INFO, borrow counters. **Gap:** separate GET and multi-key cutovers; gather uses `min(zc-min,1024)`. Loopback cannot answer send-path questions. |

These production controls pass without inventing a performance sweep for every TLS path, file
name or monitoring bound. Known compatibility limits are not reasons to delete their useful
surface; they are listed in the reverse audit.

| Control | Current default / grammar | Verdict | Operator decision / observable effect |
| --- | --- | --- | --- |
| `port` | `6379`; `0..65535`, 0 disables plaintext TCP | OPERATOR | Select the service endpoint or require TLS/Unix-only access; inspect listeners/INFO `tcp_port`. Boot. |
| `bind` | `127.0.0.1`; one address | OPERATOR | Choose the listening interface; inspect sockets. Boot; multi-address/IPv6 compatibility remains a gap. |
| `unixsocket` | Unset; socket path | OPERATOR | Enable a local filesystem endpoint; unset allocates no listener. Boot. |
| `maxclients` | `10000`; positive uint32 | OPERATOR | Bound accepted connection demand; CONFIG and rejected-connection counters. Live; concurrent acceptors can overshoot by an IO-thread-scale burst. |
| `timeout` | `0`; seconds `0..INT_MAX` | OPERATOR | Reclaim idle normal connections; 0 disables that timer. Live; blocked and RESP2 pub/sub clients are exempt. |
| `tcp-keepalive` | `300`; seconds `0..INT_MAX` | OPERATOR | Detect dead TCP peers; 0 disables probes. Live for newly accepted connections. |
| `tcp-backlog` | `511`; `0..INT_MAX` | OPERATOR | Size the kernel accept backlog for connection bursts. Boot; CONFIG SET refuses mutation. |
| `client-output-buffer-limit` | `normal 0 0 0`, `replica 256mb 64mb 60`, `pubsub 32mb 8mb 60`; repeated class/hard/soft/seconds | OPERATOR | Bound slow-consumer memory by class; inspect clients and disconnects. Live. `slave`/`replica` share the reference slot; no replication service exists. |
| `tls-port` | `0`; `0..65535` | OPERATOR | Enable/locate encrypted ingress. Boot; 0 creates no TLS context/BIO registry. |
| `tls-cert-file` | Unset; path | OPERATOR | Install the server certificate chain; inspect the served certificate. Boot. |
| `tls-key-file` | Unset; path | OPERATOR | Select the server private key matching that certificate. Boot. |
| `tls-ca-cert-file` | Unset; path | OPERATOR | Choose a CA bundle for client verification. Boot. |
| `tls-ca-cert-dir` | Unset; directory | OPERATOR | Use a managed CA directory instead of, or alongside, a bundle. Boot. |
| `tls-auth-clients` | `yes`; `yes\|no\|optional` | OPERATOR | Require, omit or optionally verify client certificates. Boot; handshake outcomes are observable. |
| `tls-protocols` | Unset selects TLSv1.2/TLSv1.3; space-separated versions | OPERATOR | Match protocol policy and client support. Boot; negotiated version is observable. |
| `tls-ciphers` | Unset selects the existing AES-GCM-first list | OPERATOR | Choose suites for TLS ≤1.2. Boot; handshake inspection. |
| `tls-ciphersuites` | Unset selects the existing TLS 1.3 suite list | OPERATOR | Choose TLS 1.3 suites; OpenSSL uses a separate API/protocol namespace. Boot. |
| `tls-prefer-server-ciphers` | `no`; `yes\|no` | OPERATOR | Choose whose preference orders eligible suites. Boot. |
| `requirepass` | Empty/unset | OPERATOR | Manage the default user's password using existing Redis runbooks. Live; AUTH and ACL default-user state. Existing sessions are not deauthenticated by rotation. |
| `protected-mode` | `yes`; boot/live also accept `0\|1` | OPERATOR | Reject unauthenticated remote access when no password is configured. Live; CONFIG emits yes/no. |
| `enable-debug-command` | `no`; `no\|yes\|local` | OPERATOR | Limit diagnostic commands to an explicit maintenance posture. Boot. |
| `aclfile` | Unset; path | OPERATOR | Persist/load named-user policy separately from service configuration. Boot; ACL LOAD/SAVE. |
| `user` | No inline definitions; repeatable `name rules...` | OPERATOR | Declare users and command/key/channel permissions in a config file. Boot parser; later ACL commands manage users. Mutually exclusive with aclfile. |
| `acl-pubsub-default` | `resetchannels`; `allchannels\|resetchannels` | OPERATOR | Choose channel permission on new/reset ACL users. Live. |
| `acllog-max-len` | `128`; unsigned count, 0 keeps no entries | OPERATOR | Budget ACL-denial history; inspect ACL LOG. Live; bound is per IO thread. |
| `dir` | `.`; directory | OPERATOR | Select persistent storage/recovery location. Boot; CONFIG SET is immutable. |
| `dbfilename` | `dump.tomo`; plain filename | OPERATOR | Name the snapshot/recovery artifact. Boot; no slash allowed. |
| `save` | `3600 1`, `300 100`, `60 10000`; repeatable seconds/changes, empty disables | OPERATOR | Set snapshot cadence/recovery exposure; INFO persistence records saves/failures. Live; off removes mutation-observer work. |
| `appendonly` | `no`; `yes\|no` | OPERATOR | Choose a write log for recovery. Boot-only here; live enablement is a compatibility gap. |
| `appendfsync` | `everysec`; `always\|everysec\|no` | OPERATOR | Choose durability versus sync latency using existing runbooks. Live; AOF status/errors. |
| `appendfilename` | `appendonly.aof`; plain filename | OPERATOR | Choose the AOF family basename. Boot; distinct from its containing directory. |
| `appenddirname` | `appendonlydir`; plain directory name | OPERATOR | Place the multipart AOF family under dir. Boot. |
| `auto-aof-rewrite-percentage` | `100`; uint32, 0 disables automatic trigger | OPERATOR | Choose tolerated log growth before compaction; INFO AOF sizes/rewrite state. Live. |
| `auto-aof-rewrite-min-size` | `64mb`; bytes with Redis suffixes | OPERATOR | Avoid compaction churn on small logs even when percentage growth is high. Live. |
| `aof-use-rdb-preamble` | Only `yes` | OPERATOR | Keep a runbook's format declaration and reject unsupported recovery expectations. A compatibility assertion, not an experimental dimension; TomoKV snapshot format is not Redis RDB. |
| `aof-timestamp-enabled` | `no`; `yes\|no` | OPERATOR | Include timestamps in the log for inspection/recovery tooling. Live. |
| `databases` | Only `1` | OPERATOR | Assert the required keyspace count and fail an incompatible config. Single-value compatibility assertion; no multi-DB implementation or sweep is proposed. |
| `proto-max-bulk-len` | `512mb`; `1 MiB..4294901759` | OPERATOR | Bound individual request bulks; accepted/rejected requests and memory use. Live; locked uint32 Slice ABI prevents Redis's larger ceiling. |
| `maxmemory` | `0`; bytes with Redis suffixes | OPERATOR | Set the dataset memory budget; inspect INFO memory/evictions/OOM. Live; 0 removes eviction work. |
| `maxmemory-policy` | `noeviction`; eight existing Redis LRU/LFU/random/TTL policies | OPERATOR | Choose write rejection or the appropriate cache victim policy. Live; hit/miss/eviction/OOM observations. |
| `maxmemory-samples` | `5`; `1..64` | OPERATOR | Trade eviction selection quality for eviction CPU cost. Live; the supported upper bound is narrower than Redis's. |
| `notify-keyspace-events` | Empty; Redis flag string | OPERATOR | Enable the events consumed by application/monitoring subscribers. Live; empty arms no notification machinery. |
| `tracking-table-max-keys` | `1000000`; unsigned count, 0 unlimited | OPERATOR | Budget client-side-cache remembering state; invalidations and table occupancy. Boot; per-owner bound and missing live SET are compatibility gaps. |
| `slowlog-log-slower-than` | `10000`; microseconds ≥−1; −1 off, 0 all | OPERATOR | Choose which slow commands need investigation; SLOWLOG. Live. |
| `slowlog-max-len` | `128`; unsigned count, 0 keeps no entries | OPERATOR | Budget diagnostic history/retention; SLOWLOG LEN. Live; per-recording-thread bound. |
| `latency-monitor-threshold` | `0`; uint32 milliseconds, 0 off | OPERATOR | Choose the latency events worth recording; LATENCY commands. Live; differs from per-command slow-log records. |
| `stream-node-max-bytes` | `4096`; current uint32 parser, 0 ignores this axis | OPERATOR | Trade stream-node memory against traversal/allocation work; CONFIG, MEMORY and workload latency. Live; reference MEMORY grammar/range repair remains missing. |
| `stream-node-max-entries` | `100`; current uint32 boot parser, 0 ignores this axis | OPERATOR | Bound stream entries per macro-node independently of payload size. Live; reference 64-bit range is not correctly enforced. |
| `hash-max-listpack-entries` | `512`; canonical integer `0..LONG_MAX` | OPERATOR | Choose the count at which small hashes become tables; OBJECT ENCODING/MEMORY USAGE. Restored, live. |
| `hash-max-listpack-value` | `64`; bytes `0..LONG_MAX`, memory suffixes | OPERATOR | Bound field-name/value length in packed hashes; same observations. Restored, live. |
| `list-max-listpack-size` | `-2`; signed int32; count or byte modes below | OPERATOR | Choose packing density/node size for lists; OBJECT ENCODING and MEMORY USAGE. Restored, live. |
| `set-max-listpack-entries` | `128`; canonical integer `0..LONG_MAX` | OPERATOR | Choose the count crossover for string sets. Restored, live; independent of the fixed integer-set limit. |
| `set-max-listpack-value` | `64`; canonical integer `0..LONG_MAX`, **no suffixes** | OPERATOR | Bound string-member length in packed sets. Restored, live. |
| `zset-max-listpack-entries` | `128`; canonical integer `0..LONG_MAX` | OPERATOR | Choose the count crossover for packed sorted sets. Restored, live. |
| `zset-max-listpack-value` | `64`; bytes `0..LONG_MAX`, memory suffixes | OPERATOR | Bound member length in packed sorted sets. Restored, live. |

| Other accepted input / alias | Verdict | Meaning |
| --- | --- | --- |
| `TOMOKV_L3_DOMAINS` | OPERATOR | Correct missing/broken topology discovery using known CPU/L3 membership. Unset/empty uses sysfs; comma-separated domains, ranges and `+` joins. Invalid, duplicate and out-of-affinity CPUs fail. It changes topology facts used by placement, not individual worker placement. |
| `pin yes\|no` (conf) | OPERATOR | Existing spelling for normal affinity / `--no-pin`; same production decision, no second feature. |
| Positional conf path | OPERATOR | Boot an existing configuration and give CONFIG REWRITE a destination. CLI overrides file settings. |
| `help` | OPERATOR | Invocation discovery; excluded from the knob count. |
| `hash-max-ziplist-entries` | OPERATOR | Reference alias of hash-max-listpack-entries; same value, validation and live behavior. |
| `hash-max-ziplist-value` | OPERATOR | Reference alias of hash-max-listpack-value. |
| `list-max-ziplist-size` | OPERATOR | Reference alias of list-max-listpack-size. |
| `zset-max-ziplist-entries` | OPERATOR | Reference alias of zset-max-listpack-entries. |
| `zset-max-ziplist-value` | OPERATOR | Reference alias of zset-max-listpack-value. |

## Reference mapping and semantics

Names and numeric kinds were checked against the official
[Redis 8.8.1 registry](https://github.com/redis/redis/blob/8.8.1/src/config.c) and the
[8.10.0 registry](https://github.com/redis/redis/blob/8.10.0/src/config.c), including their aliases,
rather than inferred from old ziplist names. The memory-versus-integer distinction for set values
is intentional. All three count limits and set value use canonical decimal; hash/zset byte limits
use the reference memory parser. Values retain the full reference range in cold configuration;
owner-local limits saturate at UINT32_MAX, the largest representable collection/element size.
CONFIG GET/REWRITE retain the original full-range value, not the saturated implementation bound.

| Removed TomoKV name | Reference name / status | Default-preserving translation |
| --- | --- | --- |
| `hash-max-compact-entries` | `hash-max-listpack-entries` | 512 entries |
| `hash-max-compact-value` | `hash-max-listpack-value` | 64 bytes; restore MEMORY grammar |
| `list-max-compact-entries` | `list-max-listpack-size` | The default −2 retains UINT32_MAX entries; nonnegative settings select count mode |
| `list-max-compact-value` | `list-max-listpack-size` | −2 maps to the existing 8192-byte small-list payload budget and expanded-node budget |
| `set-max-compact-entries` | `set-max-listpack-entries` | 128 string members; integer sets retain their independent fixed 128-entry bound |
| `set-max-compact-value` | `set-max-listpack-value` | 64 bytes; integer grammar, not MEMORY grammar |
| `zset-max-compact-entries` | `zset-max-listpack-entries` | 128 entries |
| `zset-max-compact-value` | `zset-max-listpack-value` | 64 bytes; restore MEMORY grammar |
| `script-instruction-limit` | **No equivalent reference alias.** Expected control is `busy-reply-threshold`, alias `lua-time-limit`; implementation pending | No elapsed-time value reproduces an instruction-count abort |

No active consumer in this worktree needs the old TomoKV compact spellings. They stay rejected.
The five reference aliases are accepted at boot and by live CONFIG, share canonical storage,
and REWRITE emits canonical names only. Canonical+alias duplicates in one CONFIG SET fail before
fan-out rather than giving one control two independent values.

For lists, −1, −2, −3, −4, −5 select 4, 8, 16, 32, 64 KiB; values below −5 clamp to 64 KiB.
Positive values bound elements per node, subject to the 8 KiB safety limit; 0 allows one element.
The full signed-int32 grammar is accepted, including INT_MIN without negation overflow.
These are reference exceptions to the project's usual 0=off/−1=auto convention. See the
[reference node-limit implementation](https://github.com/redis/redis/blob/8.8.1/src/quicklist.c).

TomoKV still has its own compact serialization. To obey the unchanged-default requirement, its
small-list crossover counts aggregate payload bytes as before; expanded nodes count encoded bytes
as before. Thus this is not a claim of byte-identical Redis listpack occupancy or automatic
Redis-style demotion. The restored control selects the count/size policy for both representations.
This retained representation difference is explicit, not hidden behind a reference spelling.
Existing nodes are not eagerly repacked on CONFIG SET; subsequent builders/inserts apply the
current limits, including LSET/LINSERT, list moves, deletion rebuilds, snapshot/AOF and RESTORE.

The default EncodingConfig translates field-for-field to the prior TypeLimits. CONFIG fan-out
updates each shard only on that shard's executor, before the existing barrier completes.
Limits remain inside Shard and move with it. The script workbench receives the executing shard's
live limits at each activation, including same-coordinator reuse; two stale references to the
removed Config::type_limits were also repaired. No new per-owner sidecar or migration hand-off is
introduced, and no unused interpreter or encoding-specific allocation is created by parsing.

### The scripting conflict

The [reference scripting contract](https://redis.io/docs/latest/develop/programmability/eval-intro/)
keeps an over-threshold script running, rejects most other clients with BUSY, and permits selected
administrative commands. SCRIPT KILL/FUNCTION KILL can kill only an activation that has not
issued a write. `busy-reply-threshold` is live, in milliseconds, default 5000, range 0..LONG_MAX;
0 disables this soft threshold. Its old name is `lua-time-limit`. Although redis.conf prose also
mentions negative values, the checked current configuration registries reject them.

Today TomoKV executes Lua synchronously inside one owner task and aborts after **more than
100000 instructions**, checked every 1000 instructions, replying BUSY to the invoking client.
Prior writes stand. KILL commands always reply NOTBUSY. There is no mapping from 100000 VM
instructions to milliseconds that preserves that behavior for every script and load. Replacing
100000 with 5000 ms, keeping an abort under the new name, or accepting a soft threshold that can
never be serviced would each violate a stated requirement.

The instruction control therefore earns **OPERATOR**, not NEITHER. Restoring it as an explicitly
TomoKV instruction bound would preserve the default and restore the production decision, but
would not satisfy the requested reference-only repair. A complete reference implementation needs
a responsive script-control path in **both** modes, timeout visibility to other connections,
correct KILL versus UNKILLABLE behavior after writes, and continued execution without breaking
owner exclusivity, connection ordering or QSBR. That necessarily changes the default behavior of
scripts that currently reach the instruction bound. This decision is pending; no script restoration
is claimed in this diff and no infinite-script runtime test is added to the existing default mode.

## Near-duplicates and redundant experiments

- **ratio/place/no-pin/L3 environment/shard-home:** ratio specifies counts and lets placement derive;
  place specifies concrete CPUs; no-pin controls whether workers obey those CPU targets; L3 input
  corrects discovered domain membership; shard-home sets initial key ownership. Place subsumes the
  ability to spell ratio's resulting layout, but does not replace its stable operator intent across
  hosts. A sweep varying all of these at once is confounded. Hold ownership and role counts fixed
  when testing locality; disable movement when testing filler executors.
- **key-lb/client-lb/flip-auto:** move different objects (shards, connections, roles). They are not
  three spellings for balancing. Their interaction needs independent arms; the old merged `lb`
  switch concealed that question and remains deleted.
- **overlap/read-local/reorder:** respectively schedule overlap, locality of clean reads, and
  ordering across connections. Their OFF arms answer different latency/amortization questions.
  Shallow/deep implementation-selector sweeps do not deserve separate public controls.
- **zc-min's two thresholds:** this is an actual coupled control. Above 1024, a sweep changes GET
  while leaving the multi-key gather cutoff at 1024. Below 1024 it changes both. Keep one knob,
  but label effective cutovers and include separate command families; do not claim it independently
  measures both mechanisms.
- **hash:** the known benign point-operation cost is not a forward-looking paper question by
  itself. Keep it for the operational trust decision; omit repetitive benign-key sweep cells.
- **net-io/persist-io:** one current selector controls both engines. AOF-on engine comparisons
  cannot attribute their difference solely to networking. Keep the useful operational fallback
  and test the networking question with AOF/save work disabled; do not resurrect persist-io.
- **requirepass/user/aclfile:** a default-user password, inline users, and an external policy file
  overlap in what policy they can express. Existing runbooks and ACL LOAD/SAVE require all three;
  use canonical ACL state to interpret them, not independent authentication systems.
- **tls-ciphers/tls-ciphersuites** cover different TLS protocol generations; **CA file/directory**
  cover different certificate deployment formats. Similar names are not redundant decisions.
- **timeout/tcp-keepalive/output-buffer limits/maxclients** address idle sessions, dead peers,
  slow consumers and admission respectively. **maxmemory/proto-max-bulk-len** bound different
  resources. Do not delete one because another is also a memory number.
- **slowlog threshold/length/latency threshold:** event selection, record retention and aggregate
  latency history differ. **AOF percentage/minimum size** are an AND policy that prevents small-log
  churn; **stream bytes/entries** independently bound variable-size nodes.

## Retired implementation controls: NEITHER ledger

These remain absent from CLI/conf/CONFIG. Fixed or derived behavior is inherited, not newly
benchmarked here. None earns a new paper cell merely because a historical harness mentioned it.
Restored encoding functionality and the unresolved instruction control are excluded from this
NEITHER classification.

| Retired control / spelling | Verdict | Fixed / derived behavior and reason |
| --- | --- | --- |
| `read-local-prefetch-capture` | NEITHER | Capture enabled inside armed read-local; the established winner, not an operator policy. |
| `read-local-atomic-filter` | NEITHER | Precise fail-closed safety filter enabled; weakening safety is not a production choice or defensible arm. |
| `read-local-interleave` | NEITHER | Bounded local chunks and owner quanta enabled; no independent policy question established. |
| `flip-auto-band` | NEITHER | Automatic learned jitter/resolution/baseline floors; exposes internal detector tuning otherwise. |
| `flip-work-window` | NEITHER | Mean one-in-100 whole-pass sampler when flip-auto=1, zero work when off; repeated sampler implementation cells do not answer the adaptation question. |
| `lb` | NEITHER | Removed merged alias; independent key/client controls each default 1. |
| `lb-sample-rate` | NEITHER | Derive from visits/duration, target 4096 observations per sustained decision. |
| `lb-age-sample-rate` | NEITHER | Derive only during a flip maneuver; anchor/idle stays dark. |
| `lb-tick-ms` | NEITHER | Fixed 1000 ms observation beat, three-tick sustain. |
| `lb-imbalance-pct` | NEITHER | Separate key/client learners; band twice observed quiet jitter. |
| `lb-move-cap` | NEITHER | Start at one shard, derive pacing from completed drain/install cost. |
| `lb-cooldown-ms` | NEITHER | Derive from completed transfer cost. |
| `l3-domains` | NEITHER | Redundant CLI spelling removed; discovery plus documented environment correction remains. |
| `smt-mode` | NEITHER | Derive sibling placement/FLIP units from the allowed topology. |
| `genthread-schedule` | NEITHER | Duplicate schedule selector; thread-mode plus overlap selects the retained behavior. |
| `ex-sched`, `x-ex-sched` | NEITHER | Old names rejected; public reorder defaults to FIFO/0. |
| `x-overlap`, `thread-pipeline` | NEITHER | Old names rejected; public overlap defaults to 0. |
| `atomic-window` | NEITHER | `min(16*resolved_shards,1024)`; safe live atomic reconfiguration retained. A second credit number adds no named paper question. |
| `persist-io` | NEITHER | Uring with net-io uring, syscall IO with epoll; the old selector duplicated a dependency. |
| `lru-clock-shift` | NEITHER | Fixed 8: 256-second buckets, 8192-second wrap; public eviction policy/sample controls remain. |
| `script-crossshard-max-bytes` | NEITHER | `max(4 MiB,min(boot_maxmemory/shards/16,64 MiB))`, 4 MiB without a ceiling; internal staging partition. Aggregate transient-memory control is a separate missing operator capability. |
| `script-crossshard-workbench-bytes` | NEITHER | Twice staging budget, lazy allocation; duplicate internal partition. |
| `script-crossshard-conflict-retries` | NEITHER | Fixed 8 in the existing explicit-conflict path. |
| `script-crossshard-cut-slots` | NEITHER | Fixed 4 snapshot reservations per IO owner. |
| `tls-ktls` | NEITHER | Attempt kTLS when TLS is enabled, with userspace fallback; TLS off creates no context. A losing implementation arm alone is not a useful knob. |
| `load` | NEITHER | Recover `<dir>/<dbfilename>` with existing AOF precedence; duplicate recovery-path selection. |
| `conf` | NEITHER | Redundant flag spelling removed; positional conf path remains. |

The earlier removal of `load` changed recovery: an existing configured snapshot is now loaded
without an explicit load flag. That change predates this review and is preserved. Missing files
boot empty; present corrupt/unreadable files fail. Gate launches already isolate persistence
folders. This review does not present that earlier behavior change as an unchanged default.

## Reverse audit: expected controls still missing or incomplete

These are **OPERATOR compatibility gaps**, not NEITHER deletions. A setting should be accepted
only when its behavior exists. Replication, cluster, sentinel, modules and multi-DB controls remain
outside the project's target; this list does not propose reopening them.

| Missing/incomplete control | Operator decision / concrete gap |
| --- | --- |
| `busy-reply-threshold`, `lua-time-limit` | Script responsiveness and intervention; unresolved semantics/default conflict above. The deleted instruction control is useful and should not have been classified NEITHER. |
| `set-max-intset-entries` | Integer-set memory/CPU crossover. Still fixed at 128 (Redis defaults to 512); restored listpack settings do not masquerade as this control. |
| `list-compress-depth` | Compress interior list nodes while keeping ends cheap. No compression machinery; do not accept an inert setting. |
| `hll-sparse-max-bytes` | Tune HLL sparse/dense crossover; `hll.cc` fixes 3000. |
| `unixsocketperm` | Set socket access using the expected octal grammar; no corresponding config entry exists. |
| Multi-address `bind`, IPv6 listener support | A single address is not full Redis bind grammar/behavior. |
| `include` | Compose shared/base/secret configuration using existing config-file organization. The current loader treats it as an unknown directive. |
| `logfile`, `loglevel`, syslog controls | Choose log destination/volume through standard service configuration; current stdout/stderr redirection is an external workaround. |
| `daemonize`, `supervised`, `pidfile` | Integrate existing service-manager runbooks. Foreground operation is useful but does not provide the reference directives. |
| `client-query-buffer-limit`, `maxmemory-clients` | Bound aggregate query buffers / evict clients for process memory pressure. proto-max-bulk-len limits a single bulk, and output limits are per client/class. CLIENT NO-EVICT currently has no maxmemory-clients enforcement to exempt. |
| `stop-writes-on-bgsave-error` | Select write availability after snapshot failure. Current behavior is fixed; no reference control. |
| `aof-load-truncated`, `no-appendfsync-on-rewrite` | Select incomplete-tail recovery and rewrite-time sync policy instead of fixed behavior. |
| `rdbcompression`, `rdbchecksum`, `aof-use-rdb-preamble no` | Reference-format/policy expectations are not fulfilled by the custom TomoKV snapshot. Do not add parser-only acceptance. |
| `lfu-log-factor`, `lfu-decay-time` | Tune reference LFU aging/probabilistic frequency behavior; current eviction metadata is a separate implementation. |
| `active-expire-effort`, `hz`, `dynamic-hz`, `lazyfree-*`, `activedefrag` | Choose background CPU/reclamation policy. Several require implementation work, not just exposing an internal number. |
| Live `appendonly`, `dir`, `dbfilename`, TLS settings, `tracking-table-max-keys` | Useful accepted boot controls still cannot be changed as Redis runbooks expect. Live TLS rotation particularly needs lifecycle work. |
| Process-wide `tracking-table-max-keys`, `slowlog-max-len`, `acllog-max-len` | Present names currently bound owner/thread-local state, so process totals can exceed the configured count. |
| `stream-node-max-bytes` grammar; stream ranges | Redis accepts MEMORY suffixes/full reference ranges. Current boot parsing is uint32 decimal; live setters can truncate larger values. A large positive value can become zero and disable a rollover axis. |
| Existing numeric ceilings/grammar | maxmemory-samples is capped at 64; proto-max-bulk-len is ABI-bounded; latency threshold stores uint32. Other old unsigned parsers accept noncanonical leading zeros. These are retained limitations, not proof of exact Redis parity. |
| Complete CONFIG GET/REWRITE for boot geometry | CLI-only hash/port/bind/topology are not all in the CONFIG registry; REWRITE drops explicit geometry. `pin yes` in a later conf line also cannot undo an earlier `pin no`. Operator runbooks need a complete round trip, not a claim that the file already captures everything. |

An accepted compatibility declaration (`databases 1`, preamble yes, replica buffer class) is useful
when it states the sole supported contract honestly. A configurable value with no implementing
mechanism would be a different, misleading surface.

## Default, layout and lazy-work accounting

| Layout | Incoming worktree | This diff |
| --- | ---: | ---: |
| Config | 488 (original baseline 624) | **544** |
| Op | 336 | 336 |
| Client | 1984 | 1984 |
| ThreadCtx | 1408 | 1408 |
| Shard | 1440 | 1440 |
| FlatStore | 944 | 944 |
| Rob<64> | 192 | 192 |
| AtomicEntry | 144 | 144 |

Only the Config assertion changes: **488 + 7×8 = 544**. Restoring full reference ranges requires
seven int64 values, instead of the former eight uint32 implementation thresholds. Config's
embedded Server footprint changes; every hot structure size and Shard's store/stats offsets stay
locked. A host-ABI ctypes model agrees for the incoming and new Config; the maintainer's build
must prove the C++ assertion. No assertion for a hot object was relaxed.

Default list node checks are still 8192 bytes / UINT16_MAX entries. Small-list limits still map to
UINT32_MAX / 8192; hash to 512 / 64; set/zset to 128 / 64. Integer-set default decisions stay at
128 independently. These restored settings add no hot global CONFIG lookups. Values are parsed
at boot or on the existing CONFIG control path, then copied to owner-local limits. List node
operations now read those limits instead of constants, so unchanged decisions are not a claim
of unchanged cycles; the PRE/POST comparison remains required.

No hot argument enumeration was moved ahead of key/client/flip/read-local checks. Off still
removes feature-specific allocations: key-lb leaves sample rate zero and owns no census arrays;
client-lb collects no observations; both off allocate no shared LB policy/windows; flip-auto off
has no fingerprint work; atomic off has no MVCC allocation; overlap/reorder/read-local retain their
incoming guards. Encoding CONFIG SET does not walk or eagerly rebuild existing objects.

## Validation and maintainer hand-off

Performed: source/reference inspection, Python AST parsing (without imports/execution),
changed-file whitespace checks, parser/table inventory comparison, and the Config ABI model.
**Not performed:** C++ compilation/parsing, runtime tests, server boots, the gate, benchmarks,
performance measurement or negative-control binaries.

Existing tests now cover boot defaults, all seven controls/five aliases, file/CLI precedence,
reference numeric types/ranges, negative/zero/positive list modes, canonical rewrites/reboot,
live count/value promotion on every shard, RESTORE using live limits, separate integer sets,
and expanded list-node budget effects. Existing boundary tests read live reference names.
`tests/knobs.py` remains in its existing gate row and restores settings/cleans its keys.

Maintainer negative controls: ignore EncodingConfig when initializing shards (boot/rewrite
behavior checks must fail); skip owner-local CONFIG application (all-shard promotion checks must
fail); leave expanded_push's budget fixed at 8192 (the expanded-node memory witness must fail);
keep aliases in independent storage (alias readback/duplicate/rewrite checks must fail). No such
binary was built or run here. The node-memory witness uses equal-length keys and identical list
contents, both necessarily expanded, so the small-list promotion alone cannot satisfy it.

**This review adds/retires no gate rows.** The inherited working diff still requires quick **327**,
full **344** (345 with optional NIC), versus untouched constants 325/342. Count by emitting line:
reorder mechanism at line **329** and dispatch scaling at **694**, both before the quick exit at
**1264**; each contributes +1 to quick and full. The existing knobs result emits at **987**, also
before that exit; changing its checks contributes zero rows. The parser, encoding and server-tail
batteries likewise keep their existing emitting rows. EXPECT_QUICK/EXPECT_FULL were not edited.

| Required matched-load PRE/POST comparison | PRE | POST |
| --- | --- | --- |
| Default GET/SET and collection commands, both modes, same explicit geometry | Incoming binary/rate/cycles/instructions/IPC | Pending; default decisions preserved, rates unmeasured |
| Default list promotion, expanded pushes/rebuilds, integer/generic sets | Incoming fixed limits | Pending; restored defaults, including node behavior |
| Nondefault encodings and scripts using collection workbenches | Prior knob-enabled/reference-aligned control, labelled separately | Pending; new controls are functionality, not a claimed optimization |
| Paper research questions in the inventory | Recorded/predeclared baseline arms | Complete gaps at matched offered load, plus separately labelled saturation sweeps |

Use the gate's actual 16 shards, 6:2 split, cores 0–7 when reproducing gate rows, and test 1s as
well. Use the NIC rig for zero-copy/send-path claims; rate is the verdict and instructions/IPC
explain it. No “winner” or PRE/POST number is inferred from this source-only review.

## Retained correctness finding and evidence limits

The captured and uncaptured local MGET paths in `src/core/ex_loop.h` still contain
`kAttempts=2` retry loops and increment `mget_generation_retries`. The original reduction already
reported these as an inherited violation of the supplied **no reader retries** law. This surface
change preserves those paths; it does not silently weaken the law or claim to resolve the issue.

The prior overlap/reorder audit also found compile-time SET-tax variants 1 and 3 in
`src/store/read_local_settax.h` that permit sequence-protected in-place writes while read-local
is armed. Those optional builds conflict with the immutable-write law; the shipped variant 0
is unchanged. These compile-time experiments are not runtime surface controls.

The earlier shard-count/LB derivations are implementation policy, not newly measured optima.
Transfer duration does not include subsequent cache warmup; the maintainer's rate/cycles/IPC
comparison remains necessary. Derived SMT placement retains the earlier complete-pair/same-role
restrictions, including rejection of odd logical ratios when pair mode is active. The fixed LRU
clock keeps the earlier real age-witness waits. None of these prior findings is hidden by the
restored feature surface.

The following evidence and line numbers describe the original reduction, before this final
surface update. They are retained as the witness-repair record, not as current line references.

## Historical atomic-window repair record (retained from the original reduction)

The reported failure is **(a)**: the test never separated release of its artificial hold from
its resume assertion. It is not evidence of a newly broken derived window. This verdict was
stated before editing the repair; no server code is changed by this follow-up.

The evidence, at the gate's actual geometry:

- `tests/gate.sh:29` selects cores `0-7`; line 31 resolves `6:2`; line 370 supplies `--shards 16`
  and line 390 supplies that ratio. Release and ASAN use this boot helper at lines 433 and 1249.
  The bound is **256**, not a bound inferred from a default boot on all machine cores.
- Baseline `78c3e5391:src/core/server.h:196` resolves AUTO with
  `min(16 * cfg.shards, 1024)`. Current `src/core/server.h:250` retains exactly that formula.
  Admission borrowing/stalling (`:2497`), retirement and idle lease return (`:2541`), and
  backpressure release (`src/core/io_loop.h:6698`, also `:5684`/`:6381`) are unchanged by the
  configuration reduction. The deleted setter was only a wrapper around credit reconfiguration.
- In the **reported failing version** of `tests/atomicwindow.py`, lines 13–15 submit
  `2 * ceil(256 / 64) + 2 = 10` connections, each with 64 groups: **640** total. Line 55 arms
  `ATOMIC-COMMIT-DELAY 100000`; lines 59–67 wait for every reply and raise “did not resume” at
  30 seconds; line 70 clears the hook only in the ensuing `finally`. No released completion
  interval was tested, and a timeout bypassed the nominal three-attempt discovery loop.
- `src/core/server.h:2615` applies the delay to **each commit batch**, and `:3036` spins for the
  full captured duration. Batching cannot justify the old deadline: default slow-log threshold
  is 10,000 us (`src/core/config.h:356`); escalation (`src/core/ex_loop.h:2658`) selects per-op
  timing, whose `:2620` flushes after every operation. At 100 ms per individual commit, 640 groups
  can consume 64 executor-seconds, or 32 seconds even if shared evenly by two executors. That is
  an allowed schedule exceeding 30 seconds, **not a measurement of the failing run's rate**.
- The saved release and ASAN logs both have the timeout at line 21, successful subsequent
  `CONFIG SET atomic` traffic at line 22, and `inflight=0 pending=0` after churn at line 23
  (`/tmp/gate-atomic-torn.txt`, `/tmp/gate-atomic-torn-asan.txt`). Both following RYOW logs report
  `limit=256 peak=24 stalls=0 credits=256`. This supports the release/accounting trace above;
  those logs contain no peak, stall count, or reply count for the failed global burst, so they
  cannot establish whether that particular attempt filled the window or its per-attempt rate.

The repair in `tests/atomicwindow.py` pre-encodes each burst and starts senders at one barrier.
It uses a five-second polling budget to find a held witness, clears the DEBUG delay in `finally`,
and only after the clear replies OK starts the unchanged 30-second resume deadline. At least two
groups must still be live when the window is witnessed, and some replies must remain outstanding
when release is requested. The global arm additionally requires an increase in window stalls;
a witness seen only during drain cannot rescue a miss. All replies must be OK. Cleanup wakes
blocked readers before closing buffered files, joins helpers with a shared bound, and requires
zero live groups/debt and the full credit pool before another attempt.

Only a clean, joined, fully reclaimed **witness miss** can re-arm, on new writer/control
connections and newly resolved disjoint cross-owner keys. Four misses still fail. Reply errors,
excess live groups, debt, missing credits, or failure to resume **after release** fail immediately;
none can be retried into a pass. This matches the policy in `tests/multirace.py:464` and the OFF
RENAME control at `tests/atomic_torn.py:744`, without importing either control's measured hit rate.
Each attempt prints its outcome, held/total stalls, peak, carried groups, exact reply counts and
elapsed arm/resume times with measured replies/second. The result also prints observed attempts
over attempted bursts. **The new runtime arming rate remains unmeasured:** the maintainer must
collect these lines separately on release and ASAN at 16 shards, 6:2, cores 0–7. Four is an explicit
discovery budget, not an asserted residual failure probability; no percentage is guessed here.

**Row-collapse decision: reverted.** `tests/atomic_torn.py:874` separately reports “derived atomic
window stalls and resumes”; `:882` reports “atomic reconfiguration preserves derived bound and
reclaims leases”, using a second fresh burst. Resizing coverage is obsolete with the removed
grammar, but live reconfiguration is not: `CONFIG SET atomic 1` unconditionally reaches
`set_atomic_enabled` (`src/cmd/t_server.cc:1440`), which rebuilds the credit generation even when
already ON (`src/core/server.h:2462`, `:3343`). Keeping ON preserves the same bound for all traffic.
The second row requires surviving **pre-CONFIG** admissions: it subtracts every possible newer
admission, sampled after the post-CONFIG live count, and demands a positive remainder. This cannot
pass solely on fresh post-CONFIG groups. Prepared groups abandoned for task-queue backpressure
(`src/core/io_loop.h:5077`) also count as admissions; their inclusion makes this witness stricter,
so total admissions are deliberately not equated to total replies. Both rows retain exact credit
reclamation checks; the second specifically exercises old-generation lease return (`server.h:3324`).

Repair validation was server-free: Python AST parsing, `git diff --check`, and 12 in-memory
transport fault-injection cases covering release-before-completion, the one-connection arm,
live reconfiguration, fresh retry, four-miss failure, and immediate failure on no resume, bad
reply, excess live groups, and lost credits. A control containing only newer post-CONFIG groups
also exhausted discovery rather than passing. Stalls first seen during drain cannot rescue an
unarmed attempt, and extra admissions from abandoned preparations need not equal reply counts.
These are harness checks, not measurements of
TomoKV. No build, server, benchmark, or gate was run for the repair. For maintainer-only broken
server controls: disabling admission-backpressure release must fail the resume deadline; dropping
old-generation credit returns must fail pool reclamation; suppressing the stall witness must
exhaust discovery. None of those server mutations was applied or run here.
