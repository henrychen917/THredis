# Persistence IO engines

`persist-io normal|uring` is one boot-only choice for both the AOF writer and the snapshot writer.
The default is `uring`. The option is accepted on the command line and in a configuration file,
`CONFIG GET persist-io` reports the latched value, and `CONFIG SET persist-io ...` rejects the
change as immutable.

`normal` is the blocking control implementation. AOF headers and frames use `write`/`writev`, AOF
policy syncs use `fdatasync`, and snapshot headers, frames, footer, file sync, and directory sync use
the existing syscall path. The `normal` choice never submits persistence work through io_uring.

`uring` keeps file offsets explicit and retains every source chunk until its write CQE:

- An AOF writer pass combines as many as 256 complete frames into one `IORING_OP_WRITEV`. Under
  `appendfsync always`, that write is linked to one `IORING_OP_FSYNC` with `IOSQE_IO_LINK`; up to
  eight such chains may be in flight. The durability frontier advances from the sync CQEs, in post
  sequence order, and wakes the same reply gates as the blocking path.
- Under `appendfsync everysec`, frame batches are submitted throughout the interval. At a policy
  beat, older batches are allowed to complete before the beat-ending WRITEV is linked to the one
  data sync. An idle tail sync covers only the already-completed written frontier.
- Snapshot headers, frames, and the footer use explicit-offset `IORING_OP_WRITEV` requests. A
  `SnapshotChunk` stays owned by its request through completion, with at most 64 requests in flight.
  File and directory syncs are also io_uring operations.
- A positive short write advances through its iovecs and resubmits the remaining range at the next
  explicit offset. AOF short writes force a conservative follow-up data sync before their posts can
  become durable. Write failures retain the existing INFO/log surface and shorten the failed AOF to
  a safe frame boundary after outstanding requests retire.

The shared ring retains unrelated CQEs while a blocking `SAVE` or rewrite mark pumps persistence
completions. This is needed because those commands can run inside a receive-CQE callback; the
already-consumed receive prefix is retired before nested pumping so it cannot be delivered twice.

## Validation

The quick gate runs the snapshot and AOF sections under both engines. The final run was:

```
GATE_PORT=7955 GATE_CORES=224-231 tests/gate.sh quick
GATE(quick): 117 ok, 0 FAIL
```

Focused ASAN/UBSAN runs covered blocking `SAVE`, typed snapshot reload, AOF `always`, and shutdown
under both engines. The rewrite and rewrite-trigger matrices passed under both atomic modes and both
engines. Directed unclean-stop checks passed for `always`, the `everysec` durability window, and an
incomplete atomic group. With AOF enabled, Redis differential runs were zero-diff for both engines:

| Suite | Operations | Differences |
|---|---:|---:|
| string | 4,033 | 0 |
| xshard | 4,276 | 0 |

## Performance

The pre-change binary is commit `ce2fd9726`: synchronous AOF data writes with an asynchronous AOF
sync. Post-change cells use the same release flags. Loopback SET cells used server cores 224-231,
load cores 232-239, 8 threads x 8 clients, pipeline 32, 32-byte values, a one-million-key random
range, five seconds per run, and the median of three runs.

| AOF policy | Pre-change hybrid SET/s | Post `normal` SET/s | Post `uring` SET/s | `uring` vs `normal` |
|---|---:|---:|---:|---:|
| always | 1,204,795 | 891,175 | 2,076,618 | +133.0% |
| everysec | 1,444,650 | 1,454,290 | 2,651,426 | +82.3% |

The uring result is +72.4% over the pre-change hybrid for `always` and +83.5% for `everysec`. These
AOF results are why `uring` remains the default.

Snapshot cells started with one million 128-byte keys, kept the same random SET load active during
`BGSAVE`, and measured command start through `rdb_bgsave_in_progress=0`:

| Snapshot writer | Median wall time | Three runs |
|---|---:|---:|
| Pre-change | 372.065 ms | 351.391, 384.745, 372.065 ms |
| `normal` | 358.208 ms | 338.202, 358.208, 359.631 ms |
| `uring` | 433.243 ms | 397.337, 436.453, 433.243 ms |

The measured uring snapshot tradeoff is +20.9% wall time versus `normal` in this cell.

For the append-only-disabled hot-path guard, `perf stat` counted server instructions during the same
five-second SET workload. Both boot choices remain below the allowed +1% change:

| Binary / boot choice | Median instructions/op | Change from pre-change |
|---|---:|---:|
| Pre-change | 3019.342 | baseline |
| Post `normal` | 3010.788 | -0.283% |
| Post `uring` | 3001.647 | -0.586% |

Raw results are in `/tmp/tomo-pio-perf-throughput.tsv`, `/tmp/tomo-pio-perf-uring-final.tsv`,
`/tmp/tomo-pio-perf-snapshot.tsv`, and `/tmp/tomo-pio-perf-instructions.tsv` on the validation host.
