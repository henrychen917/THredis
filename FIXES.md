# Networking and command fixes

`SURVIVING.md` repeats its list. IDs below refer to its numbered findings, counted once.
This diff fixes 28 findings. Two additional findings remain open, with runnable failing
reproducers. Concurrency, storage, transaction-engine and boot-parser findings are left to
those lanes; the shared boundaries are listed below.

## Fixed findings and negative controls

`live` means `tests/netcmd.py`, added to the shared feature-battery list. `unit NAME` means
`./build/netcmd-unit NAME`. The unit binary includes the actual private command implementations
and omits their duplicate production objects at link time. Its allocation faults and file-open
barrier exist only in that executable. None of the selected regressions has a skip path.

| Finding | Change | Regression and how to make it fail |
|---|---|---|
| **6 — S cmdsem F15, runtime string limit (MISPRICED)** | APPEND, SETRANGE, bit offsets and BITFIELD use the published runtime bulk limit. BITFIELD checks the last accessed bit too. The atomic limit load stays in those existing cold bounds checks; GET/SET acquire no new load. | `live` confirms CONFIG SET/GET of 1 MiB, rejects growing past it, and verifies unchanged values/absent destination. Restore the private 512 MiB constants: these operations succeed and the row fails. |
| **14 — C cmdsem F01, XAUTOCLAIM** | Reserve at most the actual PEL size, not an arbitrary COUNT. | `live` creates an empty group and uses COUNT 9223372036854775807, requiring `[0-0, [], []]`. Restore `reserve(count)`: length_error terminates the server. |
| **15 — C cmdsem F02, notification double destruction** | Remove the catch that repeats the shared_ptr constructor's deleter and reservation release. | `unit notify-oom` fails exactly the control-block allocation after creating a nonempty chain, asserts that failure fired, and checks the reservation is zero. Its success control checks 0→1→0. Restore the catch: double free or reservation-underflow abort. |
| **19 — C deadconf F01 / S atomics 8, Lua conversion** | Inspect result/error-table fields with raw Lua lookups, including nested verbatim fields. Conversion cannot invoke `__index`. | `live` returns metatables whose `__index` either throws or loops forever, requiring an empty array. Restore metamethod-aware lookups: crash or bounded client timeout. |
| **20 — C netproto 2, disabled output limits** | Every untracked Wb specialization stops stale client accounting before staging uncounted bytes, including TLS preparation. | `unit output` starts with tracking armed and a drained zero balance, disables the global selector, stages a seven-byte reply through the real Wb prepare path, and completes its send. Remove the stop: subtraction from zero aborts, independently of cron timing. |
| **27 — S atomics 13 / S deadconf F07, Lua reply injection** | Sanitize CR/LF in Lua `ok` and `err` simple-frame values. | `live` pipelines each injected table and a uniquely labelled PING, requiring exactly the intended two responses. Restore direct append: injected integer 42 replaces the marker response. |
| **30 — S cmdsem F03, XREADGROUP delivery progress** | Allocate all PEL nodes/consumer strings privately, then transfer nodes or swap existing values without allocation. Advance the cursor only after successful preparation. | `unit streams` proves two entries and an existing consumer, fails the second libstdc++ PEL-node allocation, and requires an unchanged cursor and empty PEL. A retry must deliver both entries and leave two pending records. Restore incremental insertion/cursor advance: the state assertions fail. |
| **33 — S cmdsem F06, compact ZPOP** | Pass the already loaded compact index to rank erasure. A successful reply no longer precedes a second fallible load. | `unit zpop` proves compact encoding and exercises MIN/MAX. Failing the first load must return an error without removal. Arming failure for a second allocation must find only the first load, return the member, and remove it. Restore the second load: the allocation/state checks fail. |
| **34 — S cmdsem F07, stream-group observation** | XGROUP's successful CREATE/DESTROY/CREATECONSUMER/DELCONSUMER/SETID branches and XSETID record their stream events. Their armed registry entries install notification context. | `unit streams` arms only the save observer on an existing stream/group, creates a new consumer, and requires exactly one save change. Remove the CREATECONSUMER record or armed wrapper: the consumer exists but the save count stays unchanged. |
| **35 — S cmdsem F09, premature FLUSH invalidation** | Move tracking flush publication from dispatch to the completed scatter retirement hook, after all owner fragments finish. | `unit flush` sends the actual dispatch gate through a two-IO fixture and requires no foreign event before owner work, then requires TrackingFlush at completion. Restore early publication: the first assertion fails. `live` also requires an invalidation after FLUSHALL and an absent key on the following tracked GET; remove the completion call and it times out. |
| **36 — S cmdsem F10, dropped invalidation** | Keep the keyless record on failed allocation/post. Retries re-sample the current IO destinations so migration cannot strand the record or miss a moved tracker; previously delivered invalidations may conservatively repeat. | `unit notify-retry` fills the actual producer marker lane while no pubsub burst is notified, proves the failed post enqueued nothing and retained the record, drains the lane, then requires the exact invalidation and record completion. Restore delete-and-continue: the record is consumed prematurely. |
| **37 — S cmdsem F11, XREAD tracking** | Use XREAD's dynamic STREAMS parser to register its key half. Parse into a scratch Op so tracking cannot append a second error to a malformed command's reply. | `live` arms ordinary RESP3 tracking, performs XREAD without BCAST or a preceding static-key read, appends from another connection, and requires the exact invalidation. Restore the static first_key early return: no invalidation arrives. Malformed XREAD plus PING also checks that registration emits no extra error frame. |
| **39 — S cmdsem F13, empty XPENDING reply** | Parse both bounds/count first and explicitly emit an empty array when incrementing the exclusive maximum start ID overflows. | `live` pipelines that exact boundary and a labelled PING, requiring `[]` before the marker. Restore the silent false return: the PING is mistaken for XPENDING's reply. |
| **40 — S cmdsem F14, NOGROUP framing** | Both NOGROUP builders sanitize binary key/group line delimiters. | `live` exercises missing-key XPENDING and missing-group XGROUP with embedded CR/LF plus a following PING. Restore raw append: an injected/malformed frame consumes the marker position. |
| **45 — S deadconf F02, rewrite loses boot settings** | Preserve original directives absent from the runtime table, including repeatable inline users and `load`, while replacing the runtime-owned directives. | `unit config` starts with inline ACL and a recovery path containing spaces, rewrites, and requires both directives and a loadable token stream. Remove preservation: the directives disappear. |
| **46 — S deadconf F03, rewrite string round trip** | Quote/escape values that need it; preserve simple scalar spelling and the multi-token output-limit/save grammars. | `unit config` round-trips spaces, newline, CR, tab, quotes and backslash in the password through the actual config lexer/loader. Remove escaping: its one-token/value assertions fail. Existing `servertail.py` also retains its unchanged scalar-format and restart checks. |
| **47 — S deadconf F04, rewrite inode collision** | Use mkstemp for a private temporary inode per rewrite; each caller renames only its own file. | `unit config` wraps mkstemp and fopen at link time. Two writers stop after opening until both are present, then must have different inodes, both succeed, and preserve the complete expected file. Restore the shared `.rewrite.tmp` fopen: the inode assertion fails deterministically. |
| **50 — S deadconf F09, catchable instruction limit** | Treat `timed_out` as activation failure even when Lua's outer pcall returns success. | `live` catches the inner runaway-loop error and returns 42; the outer activation must still return an error. Restore status-only completion: 42 is returned. |
| **52 — S netproto 3, suppression bypasses TLS** | The cold suppression serve prepares bytes and selects the TLS pump for userspace TLS, retaining poll/error handling. | `tests/tls.py`'s handshake matrix now runs SKIP;PING;PING and another labelled PING after proving each negotiated version and the fallback counters. The existing fallback gate boot forces TLS1.2 CBC, so this cannot pass solely through kTLS encryption. Restore the plaintext pump: SSL record/framing checks fail. |
| **53 — S netproto 4, early errors escape suppression** | Apply the existing mark to early local completions that precede spec selection/ordinary marking. | `live` exercises unknown command, GET arity and XGROUP subcommand arity under both OFF and SKIP, with following ON/PING markers. Remove early marking: an error escapes or consumes the wrong position. |
| **54 — S netproto 5, PUBSUB inspection loses messages** | Append pending deferred publications for inspection/publish completions as well as subscription controls. | `unit pubsub` seeds an existing subscription and unfinished CHANNELS/NUMSUB/NUMPAT control, delivers a publication, asserts the deferred state is nonempty, and requires that frame after completion. Remove the inspection append: the frame vanishes. |
| **55 — S netproto 6, marked control swallows pushes** | Move a suppressed control's independent deferred publications to Wb's ordered OOB queue before discarding its command reply. | `unit pubsub` repeats the pending-state test with marked operations, including SUBSCRIBE, and requires exactly the independent message after suppressing retirement. Embed it in the marked Op again: retirement discards it. |
| **56 — S netproto 7, aggregate segment narrowing** | Split the two-piece overload using the same segment-length bound as the single-piece overload. | `unit receive` instantiates the real queue with an eight-byte segment bound, appends 11+8 bytes and requires three bounded segments and all 19 ordered bytes. Restore the unsplit overload: segment-count/bound assertions fail. This exercises the mechanism without allocating a >4 GiB list. The production bound remains UINT32_MAX. |
| **57 — S netproto 8, whole-command receive cap** | Distinguish the per-bulk parser bound from the 32-bit whole-command receive bound. Growth still requires ROB quiescence; an incomplete frame at the representation ceiling gets an explicit error. | `live` sets a 1 MiB bulk limit on a fresh connection and successfully sends/stores two legal 600 KiB MSET values. `unit receive` also requires growth beyond one bulk and forbids growth with outstanding slices. Restore a one-bulk whole-buffer cap: growth/response fails. |
| **58 — S netproto 9, epoll's default allowance** | Epoll passes the live limit, and the shared receive-cap correction above removes dependence on a stale per-bulk default altogether. | The directed live battery passed on epoll. `unit receive` reserves a 600 MiB receive allowance with the default argument and the 1 GiB setting without touching that payload. Restore read_space's old default-based 512 MiB ceiling: the default-allowance assertion fails. This tests the shared allowance directly; the live epoll battery does not send a 600 MiB payload. |
| **59 — S netproto 10, empty inline parsing** | Return a distinct Empty result and commit the consumed prefix without publishing an operation or consuming reply suppression. | `live` sends CRLF and whitespace-only lines before a valid RESP command. Restore Incomplete: the labelled reply times out. |
| **60 — S netproto 11, binary command-name framing** | The shared error formatter strips CR/LF from line text, covering unknown-command names and echoed arguments. | `live` uses an unknown bulk command containing an apparent integer frame and checks the following marker. Restore unsanitized reply_err: an extra frame is read. |
| **62 — S netproto 13, deferred output accounting** | Include Wb OOB queues and pending pubsub publications in hard/soft limit input. Deferred storage is inspected only after the chosen client's limit is enabled. | `unit output` holds an ordinary nonblocking ROB entry before Done, proves all nine bytes exist only in OOB deferral, and requires an eight-byte hard limit to close it before completion. Omit deferred bytes from the check: it does not fire. |

## Open, with failing reproducers

- **32 — S cmdsem F05: partial collection mutation on error. NOT FIXED.** Existing
  expanded hash/set/list/zset updates and field-TTL operations do not have one shared
  failure-atomic preparation/publication contract. Correcting only HSET's late TTL clear or
  notification would leave the prefix mutation and the other collection paths. A rollback
  that allocates can fail under the same condition. This diff does not claim to solve that.
  `./build/netcmd-unit collection-oom` is a diagnostic, outside the passing gate loop. It
  proves expanded encoding, arms `a`'s field TTL, fails the second 256-byte replacement, and
  checks the old value. **Run here: it failed at the changed-prefix assertion after proving
  exactly one injected allocation failure.** The same injector can verify a future fix.
- **61 — S netproto 12: kTLS KeyUpdate. NOT FIXED.** The raw receive path needs ancillary
  control-record handling and coordinated receive/transmit rekeying; changing an errno
  branch cannot supply those protocols. TLS1.3 kTLS remains enabled and its existing
  engagement checks are unchanged. The kernel's [control-record and rekey contract](https://docs.kernel.org/networking/tls.html)
  explains why this requires more than ignoring the receive error. Build the diagnostic with
  `g++ -std=c++20 tests/ktls_keyupdate.cc -o build/ktls-keyupdate -lssl -lcrypto`, then run
  `build/ktls-keyupdate 127.0.0.1 TLS_PORT` on a default AES-128 TLS1.3 boot. It requires its
  sole TLS connection to report active kTLS before sending a legal requested KeyUpdate.
  **Run here: kTLS arming passed, then the post-update TLS read failed.** No skip or userspace
  fallback counts as a pass. This open diagnostic is not added to the passing gate loop.

## Other lane boundaries and disputes

No findings were disputed. In particular, these shared command/network-facing findings remain
real and were not silently counted as fixed:

- **3 / cmdsem F08**: WATCH dirtying after unsuccessful writes is rooted in ordinary executor
  completion and transaction observation; the transaction/concurrency lane owns it.
- **4 / deadconf F08**: reading INFO/RESETSTAT counters safely requires changing the plain
  counter writers too; a cmd-only atomic read would retain the race.
- **17 / concurrency 3 + netproto 1**: the Done-to-notify Client lifetime belongs to the
  executor/IO reclamation protocol. This diff does not add or claim an executor-tail reference.
- **31 / cmdsem F04 + storage 10** and **38 / cmdsem F12 + storage 12**: snapshot-safe field
  expiry and both lazy/active hash accounting are storage-lane work. Neither half was patched
  independently here.
- **48–49 / deadconf F05–F06**: boot-password NUL and missing-value validation are in the config
  parser. Rewriter quoting is not a fix for either boot-parser defect.

The remaining MVCC, transaction, script/function-library lifecycle, AOF, storage, scheduler,
migration and read-local retry findings are outside this lane. Core edits in this diff are
restricted to networking integration and a zero-layout-cost test friend declaration.

## Gate accounting

No EXPECT constant was edited. The new rows are all **above** the quick-tier exit:

- One `netcmd regression build` row in the static section.
- Nine rows from `for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config`.
- Four `netcmd` feature rows: two atomic settings in the split loop and two in the fused/armed loop.

Thus **+14 quick and +14 full**. From the documented inherited **327 quick / 344 full**
program, the expected totals become **341 quick / 358 full**, without the optional NIC row.
The inherited literal constants are already 325/342 (two below that program); simply adding
14 to those stale literals would still be wrong. The TLS additions strengthen an existing row
and add no ledger row. The two open diagnostics add no passing row.

## Validation and limits

- Release build and the serverless unit build succeeded with existing footprint assertions.
  No locked size or assertion changed. No Op, Client, ThreadCtx, Shard, FlatStore, Rob,
  AtomicEntry or Config layout changes.
- All nine selected serverless sections passed. Their tests force and assert their relevant
  allocation, queue, completion or file-open states; there are no timing-luck passes.
- The new live battery passed on split/io_uring, split/epoll, fused, and fused with read-local
  armed, with both atomic settings represented. Split boots used cores 0–7, 16 shards, 6:2;
  fused used the same cores/shards and correctly omitted the unsupported ratio option.
- Existing streamgroups, tracking, climon2, limits, lua_scripting, bitfield, servertail,
  zsetops and edgeproto batteries were exercised on split and fused/armed configurations.
  A scalar-format regression found by servertail was corrected in the writer; its test was
  not weakened.
- The TLS1.2/TLS1.3 fallback handshake/suppression matrix passed in both thread modes.
- The two open diagnostics failed as described above. No full gate, benchmark or performance
  comparison was run. Only individually owned test-server PIDs were stopped.

Performance and the maintainer's full gate remain unmeasured here. These are correctness fixes;
there is no PRE/POST performance claim. The user-owned CODEX-OUT.md change and duplicated
SURVIVING.md input are not part of these edits.
