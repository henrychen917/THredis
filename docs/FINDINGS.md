# FINDINGS

Documentation review of worktree `cx-docs`, commit `c8e61f646`, on 2026-09-08.
The supplied context names the older `78c3e5391` baseline. All new operational
and architectural descriptions were derived from code; existing notes/designs
were used only to identify disagreements. No build, server, benchmark, gate, or
unit binary was run. No source or test file was changed.

The top-level documentation of [Redis](https://github.com/redis/redis/blob/7.4/README.md),
[Dragonfly](https://github.com/dragonflydb/dragonfly/blob/main/README.md), and
[Garnet](https://github.com/microsoft/garnet/blob/main/README.md) was consulted for
organization and navigation, not as evidence about TomoKV behavior.

## Reader contract

1. **The active local MGET path retries.** In
   [ex_loop.h](../src/core/ex_loop.h), `drain_local_reads_bounded` selects
   `drain_local_reads_bounded_impl<true, ...>`, which selects
   `prepare_captured_local_mget`. That function sets `kAttempts = 2`, clears the
   private reply after failed validation, recaptures, and increments
   `mget_generation_retries` before its second attempt. This contradicts the
   supplied no-reader-retry law and the unqualified "Readers neither lock nor
   retry" statement in [DESIGN-w-atomlat.md](../DESIGN-w-atomlat.md).
   [DESIGN-W-RLIDEAS.md](../DESIGN-W-RLIDEAS.md) explicitly retains the retry,
   so the existing documents themselves disagree on the scope of that law.
   The active captured GET path instead breaks to owner fallback on topology
   churn. The new documents state this distinction; no implementation change
   was made.
2. **Nondefault compile-time variants permit in-place writes while armed.**
   [read_local_settax.h](../src/store/read_local_settax.h) retains selectors `1`
   and `3`, enabling sequence-protected raw-string overwrite through
   `FlatStore::try_overwrite_read_local`. These variants conflict with the
   stated no-in-place-overwrite/no-per-operation-seqlock laws. The default
   selector is `0`, for which overwrite returns `NotPossible`; ordinary
   Makefile builds therefore use immutable replacement. The generic point-read
   template also contains a non-capture three-attempt arm, but the current
   local drain statically selects capture. The documented contract is for the
   default build, not every retained research selector.
3. **MGET validation is no longer selected per shard's pending bit.**
   [docs/thread-mode.md](thread-mode.md) describes table generations for
   group-free shards and cell epochs only for pending shards. The implementation
   in `LocalMgetWindow`, `local_mget_window_open`, and
   `local_mget_window_close` uses cell epochs for every queried key when there
   are at most 128 keys; larger commands use touched-shard generations. This
   agrees with the later [DESIGN-W-RLIDEAS.md](../DESIGN-W-RLIDEAS.md) update.
4. **"Topology word" needs its group-publication qualification.**
   [flatstore.h](../src/store/flatstore.h) keeps ordinary immutable slot
   replacements out of the table guard. It does use `ReadLocalTableGuard` for
   structural moves, atomic physical exchanges, ownership rebinds, and the
   short filter-publication handshake in `foreign_read_scope_open_span` and
   broad poison publication. Thus "only structural moves" without mentioning
   the group safety handshake is too narrow, and "writes never obstruct reads"
   is too broad. The local lane can decline on these events or on unsafe-key
   filter hits. No no-fallback or universal progress guarantee was documented.

## Command inventory

The current registry has **246 top-level entries**, not 243. This is a static
count of the `kTable` initializers returned by every family enumerated in
`command_registry_init` in [commands.cc](../src/cmd/commands.cc). That function
copies every family row; `command_registry_size()` returns the resulting vector
size, and `COMMAND COUNT` uses it directly.

| Family file | Entries | Family file | Entries |
| --- | ---: | --- | ---: |
| `t_string.cc` | 50 | `t_hash.cc` | 16 |
| `t_hash_ttl.cc` | 9 | `t_list.cc` | 22 |
| `t_set.cc` | 17 | `t_zset.cc` | 28 |
| `t_zset_ops.cc` | 7 | `geo.cc` | 10 |
| `t_stream.cc` | 7 | `t_stream_groups.cc` | 8 |
| `t_server.cc` | 43 | `scripting.cc` | 5 |
| `functions.cc` | 3 | `server_tail.cc` | 13 |
| `slowlog.cc` | 2 | `lcs.cc` | 1 |
| `cmdgap.cc` | 4 | `pfdebug.cc` | 1 |

The 246 names include aliases, unsupported/standalone reply handlers, and the
TomoKV `FLIP` extension; this is not a count of fully implemented Redis command
semantics. [tests/cmdgap.py](../tests/cmdgap.py) and
[tests/cmdgap2.py](../tests/cmdgap2.py) still assert 243. The supplied context and
older live inventories in [NOTES-CMDGAP.md](../NOTES-CMDGAP.md) and
[NOTES-CMDGAP2.md](../NOTES-CMDGAP2.md) cannot be used as the current registry
count. No live `COMMAND COUNT` was requested or observed during this task.

The stated deliberately absent surfaces remain absent: `CLUSTER`, `MIGRATE`,
`MODULE`, `MOVE`, `PSYNC`, `REPLCONF`, `SENTINEL`, `SWAPDB`, and `SYNC` have no
entries. Related registered names are not implementations of those surfaces:
`REPLICAOF`/`SLAVEOF` return an unsupported error, and the cluster-mode switches
return the standalone error. `WAITAOF` with positive `numlocal` explicitly
returns an unimplemented error, including when AOF is enabled. These limits
are in [server_tail.cc](../src/cmd/server_tail.cc) and
[cmdgap.cc](../src/cmd/cmdgap.cc).

## Configuration round-tripping

`config_rewrite` in [server_tail.cc](../src/cmd/server_tail.cc) does not fulfill
the "complete, clean file of the knobs this build owns" description in
[NOTES-SERVERTAIL.md](../NOTES-SERVERTAIL.md):

- It writes only `command_config_snapshot`, whose entries come from
  `init_config` in [t_server.cc](../src/cmd/t_server.cc). The table omits
  `port`, `bind`, `unixsocket`, `ratio`, `place`, `shards`, pinning, `hash`,
  `load`, and inline `user` definitions. Rewriting a file configured with
  these can change the next boot's listeners, geometry, recovery input, or ACL
  users. `aof-use-rdb-preamble` is also deliberately skipped by the writer.
- Nonempty values are appended verbatim. A valid setting such as
  `requirepass "two words"` would be emitted as `requirepass two words`, which
  the shared parser does not accept as the same setting. Quotes, escapes, and
  paths with spaces have analogous problems. This is a source-derived
  counterexample, not a runtime reproduction.

The new configuration guide describes these limits instead of recommending
CONFIG REWRITE as a complete restart-safe configuration export.

## Configuration validation

- **Runtime narrowing:** `normalize_config` accepts a `u64` for
  `stream-node-max-bytes`, `stream-node-max-entries`, and
  `latency-monitor-threshold`; the application code casts them to `uint32_t`.
  For example, `4294967296` would normalize successfully, be stored in the CONFIG
  table, and become zero in effective state. The startup parser rejects that
  value. See [t_server.cc](../src/cmd/t_server.cc), `normalize_config` and
  `cmd_config`, versus [config.h](../src/core/config.h).
- **Permissive ratio parsing:** `parse_config_args` uses
  `sscanf(value, "%u:%u:%u", ...)` and requires two conversions, without checking
  complete consumption. An input such as `2:2junk` can pass this stage. The
  documented grammar is the intended positive `io:ex` form; no malformed
  spelling is promoted into an operational example.
- **Pinning is one-way in the loader:** `pin no` emits `--no-pin`, while
  `pin yes` emits nothing. A later `pin yes` in the same file does not restore
  pinning. The guide calls this out rather than promising last-value-wins for
  this exception.
- **Overflowing memory quantities:** `cfg_parse_memory` deliberately preserves
  unsigned saturation/multiplication behavior, including wrap for suffixed
  overflow. Syntactically valid large memory inputs can therefore produce a
  smaller value, including zero, rather than an out-of-range error. The guide
  does not claim strict overflow rejection.
- **Zero-copy off has a narrower scope than its name suggests:** `zc-min 0`
  disables the borrowed single-key GET reply, but the MGET gather arm in
  [scatter_engine.inc](../src/cmd/scatter_engine.inc) still computes
  `min(zc-min, ValueSlot::kInline)`. At zero it borrows nonempty non-integer
  payloads instead of copying them into the inline gather slot. It therefore
  cannot be documented as an off switch that removes all borrow allocations.

## Other stale design and source descriptions

The differences below concern present behavior, not the validity of measurements
or historical observations in those files. Historical documents were left intact.

| Existing description | Current code and documentation decision |
| --- | --- |
| [docs/flipctl-design.md](flipctl-design.md) says fingerprint windows close every `flip-work-window` commands. | [flipctl.h](../src/core/flipctl.h), `FlipFingerprintWriter`, samples complete parse passes with randomized gaps whose mean is the configured window. The writer is dark when the automatic controller is off. |
| The same design says another detected workload shift restarts an active seek. | [flipctl.cc](../src/core/flipctl.cc), `tick`, permits only a forced trigger to interrupt mid-maneuver; automatic shifts are considered again at the settled baseline. |
| The same design describes an overshoot and bracket/seek strategy. | Current `issue_initial_jump` and `advance_seek` use model-selected targets, verification, and optional refinement; the current code explicitly says "No walk, no overshoot." The guide describes the current controller without the older search recipe. |
| [DESIGN-SNAPSHOT.md](../DESIGN-SNAPSHOT.md) bases its cut proof on all commands being single-shard and requiring no atomics. | [snapshot.cc](../src/snapshot/snapshot.cc) closes new atomic admission and drains partially applied groups before Freeze/Mark. Cross-shard EXEC and other groups exist; owner batch boundaries alone do not prove a consistent cut. Armed storage also uses atomic slot/topology publication. |
| [DESIGN-TYPES.md](../DESIGN-TYPES.md) describes a six-family registry and replies solely through the byte sink. | [commands.cc](../src/cmd/commands.cc) aggregates 18 families. [t_string.cc](../src/cmd/t_string.cc) can put borrowed value descriptors into the reply path; IO can retain pointers to store-owned payload bytes until release. |
| [DESIGN-ZC.md](../DESIGN-ZC.md) says FlatStore contains no new atomics. | That is insufficient as a current whole-store description: the armed local-read implementation has atomic slot/topology and foreign-read publications. The owner-private borrow registry itself remains a separate mechanism. |
| [DESIGN-w-storettl.md](../DESIGN-w-storettl.md), [DESIGN-W-RLIDEAS.md](../DESIGN-W-RLIDEAS.md), and the supplied context retain `Config = 624`. | [config.h](../src/core/config.h) asserts `sizeof(Config) == 528`. [DESIGN-KNOBS.md](../DESIGN-KNOBS.md) records the subsequent reduction. Other listed hot-layout locks remain as documented. |
| Older examples in [DESIGN-P0REPLY.md](../DESIGN-P0REPLY.md) and [DESIGN-W-RLIDEAS.md](../DESIGN-W-RLIDEAS.md) use `--key-lb`/`--client-lb`; P0REPLY describes the old lazy sink rebind. | The parser accepts only `--lb` for those controls. [server.h](../src/core/server.h) eagerly rebinds the retirement sink in both quiesced transfer functions before publishing ownership. The historical failure is not described as current behavior. |
| The pre-existing root [control-plane draft](../ARCHITECTURE-CONTROLPLANE.md) exposes `ex-sched`, separate key/client LB knobs, and script staging controls. | [config.h](../src/core/config.h) accepts `x-ex-sched` and `lb`; staging limits now derive in [server.h](../src/core/server.h). Deleted names are rejected, not compatibility aliases. This pre-existing untracked draft was not edited. |
| [DESIGN-KNOBS.md](../DESIGN-KNOBS.md) requests omission of study controls from user documentation. | The current task explicitly asks for every knob. Both accepted `x-overlap` and `x-ex-sched` are therefore documented as study settings, without changing their parser or help visibility. |
| The opening comment in [flatstore.h](../src/store/flatstore.h) says there are no atomics/QSBR and that a KvObj is never copied. | Armed local reads use atomic publications and QSBR; writes can construct replacement objects and MVCC retains versions. Resize still moves slot pointers rather than relocating existing allocations. |
| [snapshot/format.h](../src/snapshot/format.h) says collection hooks return Unsupported until expanded types land. | The current hash/list/set/zset/stream families export concrete snapshot hooks. The guide documents the implemented per-type capture contract. |
| The opening comment in [persist/aof.h](../src/persist/aof.h) calls AOF single-file. | [aof.cc](../src/persist/aof.cc) uses a manifest, snapshot bases, and incremental files. One physical writer does not mean one lifetime file. |
| The configuration help and [tomokv.conf](../tomokv.conf) describe the reference file as the full set. | The parser also accepts the two intentionally hidden study options. The new configuration guide includes them and distinguishes the startup set from the smaller CONFIG table. |

## Additional protocol observation

In [resp.h](../src/net/resp.h), the general multibulk parser verifies that two
bytes remain after each bulk payload, but advances over them without checking
that they are CRLF. This leaves a malformed-frame acceptance path in the code.
The inline-command path also splits whitespace without implementing the quoted
inline grammar of the configuration parser. No strict malformed-input or full
inline-protocol equivalence was claimed, and neither observation was tested or
fixed in this documentation task.

## Verification limits and remaining uncertainty

- Every new build invocation was checked against the Makefile's targets,
  variables, source list, and link recipe. Every server invocation was checked
  against startup parsing, validation, geometry resolution, and load precedence.
  None was executed, as requested. Successful compilation, boot, TLS negotiation,
  client exchanges, and recovery still require maintainer-run verification.
- The source does not establish tested minimum GCC, liburing, OpenSSL, or kernel
  versions. The docs list required interfaces and libraries instead of inventing
  version guarantees. In particular, the uring setup-flag fallback alone does
  not establish that all later operations work on an arbitrary older kernel.
- jemalloc is optional only through the explicit non-jemalloc build path. With
  `JE=1`, absence of the system header selects a maintainer-local `JEDIR`; it does
  not automatically build without jemalloc. The README includes `JE=0` and the
  alternate-prefix rule so a fresh checkout does not depend silently on that path.
- Registry membership is statically verified; complete equivalence of every
  Redis subcommand, option, error, wire encoding, and persistence value tag was
  not established. The docs make a scoped compatibility statement, with concrete
  unsupported surfaces, rather than a version-wide drop-in guarantee.
- Source inspection can identify ownership/retry paths but is not a concurrency
  proof. No new claims about latency, rates, scalability, or a passing gate were
  made. No gate row was added or retired, so no change to `EXPECT_QUICK` or
  `EXPECT_FULL` is requested.
- No top-level project license file was found; only the bundled Lua license was
  present in the license-file search. No project license or contribution policy
  was invented. Authorship/provenance history cannot be proven by this source
  review alone.
- Short directory headers were supplied as directory `README.md` files, including
  tests/tools/third_party, to satisfy the navigation request while keeping every
  existing source file unchanged. The pre-existing modification to `CODEX-OUT.md`
  and both untracked root architecture drafts were left untouched.
