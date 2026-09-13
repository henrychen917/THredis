// Included inside ExLoopT. O11's 1s rotation uses the existing read-local fairness quanta:
// owner bucket hints -> one lane drain -> owner execution. IO parses before this turn and
// retires its replies afterwards, so a clean local reply does not wait for another turn's
// preselected WB batch. No task or foreign capture is staged across a call.
//
// The pure-read tail and exceptional owner debt stay in fused_pass_impl. With no local work,
// that envelope keeps the existing 128-task owner batch; a mixed turn uses the same 32-task
// producer quantum as overlap 0. This changes scheduling only: no command-path hooks, counters,
// allocation, or reader validation are added. The only witness is per batch, in existing stats.
// The empty disabled witness occupies no argument register in the shared producer-drain helper.
struct NoLocalReadGap {};

uint32_t fused_local_read_pass(bool& gap_used) {
    static_assert(Fused);
    if (!read_local_enabled()) std::abort();
    return fused_pass_impl<kGenthreadPipelineExBatchOps, true, true, true, true, void, true>(
        &gap_used);
}

template <size_t Capacity>
uint32_t exec_batch_local_read_gap(Task (&batch)[Capacity], uint32_t held, bool& gap_used) {
    static_assert(Fused);
    static_assert(Capacity == kReadLocalOwnerTaskChunkOps);
    if (!xshard_retries_.empty()) {
        // An undecided owner unit keeps its established deferred suffix. Do not open a
        // new gap in that suffix, reorder it, or bypass the dispatch-time parking rule.
        exec_batch<true>(batch, held);
        return 0;
    }
    // A preceding batch may have installed the last group fragment. Publish its epoch
    // before any local probe. Prefetch retains no store pointer or version: a lane
    // fallback can safely post a new owner task without invalidating a captured value.
    flush_xshard_commits();
    if (__builtin_expect(reorder_enabled_, false))
        srv_->mode_schedule_stats(self_->id()).note_reorder(
            held, ex_schedule_batch(batch, held));
    prefetch_exec_batch(batch, held);

    // LANE: the same bounded capture/copy/demotion helper used by overlap 0. All its
    // foreign pointers die on return, before owner mutation, WB, or the QSBR tick.
    const uint32_t local = drain_local_reads_bounded(kReadLocalDrainChunkOps);
    gap_used |= local != 0;

    // OWNER: consume this whole warmed batch. The producer's recv/callback/retire
    // sequence is unchanged. A demotion whose reservation finds the lane full simply
    // remains pending until these owner tasks retire; it cannot recurse or spin here.
    exec_batch_prefetched<true>(batch, held);
    return local;
}
