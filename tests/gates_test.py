#!/usr/bin/env python3
"""Server-less negative controls for the release gate's verdict logic.

These deliberately poison valid evidence. Run manually while developing the harness; they
do not add ledger rows or replace the live feature/performance cells.
"""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import feature_gate as feature
import perf_gate as perf


def feature_evidence():
    knobs = {'thread-mode': '2s', 'read-local': 1, 'overlap': 1, 'reorder': 1,
             'flip-auto': 1, 'atomic': 1, 'key-lb': 1, 'client-lb': 1}
    keys = ('overlap_passes', 'overlap_interleaved_passes', 'reorder_batches',
            'reorder_multi_client_runs', 'reorder_permuted_runs', 'atomic_groups',
            'tomokv_keylb_ticks', 'tomokv_keylb_bucket_moves', 'tomokv_keylb_client_moves',
            'flipctl_forced_triggers', 'flipctl_triggers')
    before = {key: '0' for key in keys}
    before.update(read_local_active_threads='1', schedule_stats_threads='2',
                  tomokv_keylb_bucket_weight_spread_current='0',
                  tomokv_keylb_client_weight_spread_current='0',
                  overlap_schedule='split-io-overlap',
                  read_local_thread_0='role=ifid,shards=0,active=1,hits_total=0,mget_hits_total=0',
                  read_local_thread_1='role=ex,shards=16,active=0,hits_total=0,mget_hits_total=0')
    after = dict(before, **{key: '10' for key in keys})
    after['read_local_thread_0'] = 'role=ifid,shards=0,active=1,hits_total=9,mget_hits_total=8'
    after['tomokv_keylb_bucket_weight_spread_current'] = '3'
    after['tomokv_keylb_client_weight_spread_current'] = '4'
    return before, after, knobs


def perf_evidence():
    row = dict(thread_mode='2s', read_local='0', atomic='1', flip_auto='0', overlap='0', reorder='0',
               keyspace_misses='0', rejected_connections='0', send_errors='0', peer_aborts='0',
               read_local_hits='0', read_local_mget_local_hits='0', cmdstat_get='calls=100')
    thread = dict(role='ex', clients=0, ops=100, busy_ns=1000, idle_ns=1000)
    before = dict(info=row, lb={'threads': {0: thread}}, dbsize=1000,
                  midpoint_ns=1000000000, collection_ns=1000)
    after = copy.deepcopy(before)
    after.update(midpoint_ns=2000000000)
    after['info']['cmdstat_get'] = 'calls=1000100'
    after['lb']['threads'][0].update(ops=1000100, busy_ns=999001000, idle_ns=1001000)
    return before, after


class FeatureFailures(unittest.TestCase):
    def test_full_inventory_and_values(self):
        feature.inventory('8-15', '6:2')
        self.assertEqual(len(feature.CELLS), 35)
        self.assertEqual(sum(cell.endswith('-1') and cell.startswith('1s-')
                             for cell in feature.MATRIX), 8)
        saved = feature.MATRIX
        try:
            feature.MATRIX = saved[:-1]
            with self.assertRaisesRegex(AssertionError, 'lost a row'):
                feature.inventory('8-15', '6:2')
        finally:
            feature.MATRIX = saved

    def test_every_enabled_witness_must_fire(self):
        b, a, knobs = feature_evidence()
        feature.check_activity(b, a, knobs, 2, True)
        for key in ('overlap_passes', 'overlap_interleaved_passes', 'reorder_batches',
                    'reorder_multi_client_runs', 'reorder_permuted_runs', 'atomic_groups',
                    'tomokv_keylb_ticks', 'flipctl_forced_triggers',
                    'tomokv_keylb_bucket_weight_spread_current', 'tomokv_keylb_client_weight_spread_current'):
            with self.subTest(key=key), self.assertRaises(AssertionError):
                feature.check_activity(b, dict(a, **{key: '0'}), knobs, 2, True)

    def test_reader_roles_and_each_hit_counter(self):
        b, a, knobs = feature_evidence()
        for old, new in (('active=1', 'active=0'), ('hits_total=9', 'hits_total=0'),
                         ('mget_hits_total=8', 'mget_hits_total=0')):
            poisoned = dict(a)
            poisoned['read_local_thread_0'] = poisoned['read_local_thread_0'].replace(old, new)
            with self.subTest(new=new), self.assertRaises(AssertionError):
                feature.check_activity(b, poisoned, knobs, 2, True)
        poisoned = dict(a)
        poisoned['read_local_thread_1'] = poisoned['read_local_thread_1'].replace('hits_total=0', 'hits_total=1')
        with self.assertRaisesRegex(AssertionError, 'nonreader'):
            feature.check_activity(b, poisoned, knobs, 2, True)

    def test_disabled_features_cannot_fire(self):
        b, a, knobs = feature_evidence()
        for key in ('overlap', 'reorder', 'atomic', 'key-lb', 'client-lb', 'flip-auto'):
            with self.subTest(key=key), self.assertRaises(AssertionError):
                feature.check_activity(b, a, dict(knobs, **{key: 0}), 2, True)


class PerformanceFailures(unittest.TestCase):
    def test_saturation_is_per_thread_and_p32_only(self):
        b, a = perf_evidence()
        result = perf.summarize_window(b, a, 'GET-p32-2s-rl0', 1000)
        result.update(cell='GET-p32-2s-rl0', instances=2, generator_max_cpu=.2)
        perf.capacity(result)
        # A busy peer must not hide an idle executor in a role-average percentage.
        b['lb']['threads'][1] = dict(b['lb']['threads'][0])
        a['lb']['threads'][1] = dict(a['lb']['threads'][0], busy_ns=1001000, idle_ns=999001000)
        result = perf.summarize_window(b, a, 'GET-p32-2s-rl0', 1000)
        result.update(cell='GET-p32-2s-rl0', instances=2, generator_max_cpu=.2)
        with self.assertRaisesRegex(AssertionError, 'could not saturate'):
            perf.capacity(result)
        result['cell'] = 'GET-p1-2s-rl0'
        perf.capacity(result)  # p1 must NOT inherit the p32 assertion.
        result['generator_max_cpu'] = .5
        perf.p1_validity(result, 64)
        result['generator_max_cpu'] = .99
        with self.assertRaisesRegex(AssertionError, 'headroom'):
            perf.p1_validity(result, 64)

    def test_population_counter_and_clock_tripwires(self):
        for mutation in ('empty', 'miss', 'no_work', 'frozen', 'missing_busy'):
            b, a = perf_evidence()
            if mutation == 'empty':
                a['dbsize'] = 0
            elif mutation == 'miss':
                a['info']['keyspace_misses'] = '1'
            elif mutation == 'no_work':
                a['info']['cmdstat_get'] = b['info']['cmdstat_get']
            elif mutation == 'frozen':
                a['lb']['threads'] = copy.deepcopy(b['lb']['threads'])
            else:
                del a['lb']['threads'][0]['busy_ns']
            with self.subTest(mutation=mutation), self.assertRaises((AssertionError, KeyError)):
                perf.summarize_window(b, a, 'GET-p32-2s-rl0', 1000)

    def test_null_derivation_rejects_noisy_or_non_null_experiments(self):
        samples = [dict(rate=1000000 + i % 2 * 1000, instances=2,
                        binary_sha256='same', timing_uncertainty=.00001) for i in range(12)]
        bound = perf.null_bound(samples)
        self.assertGreater(bound['loss_fraction'], .0009)
        self.assertLess(bound['loss_fraction'], .002)
        reference = dict(rate=1000000, bound=bound)
        perf.compare_rate('GET-p32-2s-rl0', 1000000, reference)
        with self.assertRaisesRegex(AssertionError, 'regression'):
            perf.compare_rate('GET-p32-2s-rl0', 980000, reference)
        samples[3]['rate'] *= .9
        with self.assertRaisesRegex(AssertionError, '2% box law'):
            perf.null_bound(samples)
        samples[3]['binary_sha256'] = 'different'
        with self.assertRaisesRegex(AssertionError, 'different binaries'):
            perf.null_bound(samples)

    def test_adjacent_null_pairs_cannot_hide_drift(self):
        # Each adjacent pair is exactly equal, but the experiment drifts by 5% overall.
        samples = [dict(rate=1000000 * (1 + .01 * (i // 2)), instances=2,
                        binary_sha256='same', timing_uncertainty=.00001) for i in range(12)]
        with self.assertRaisesRegex(AssertionError, '2% box law'):
            perf.null_bound(samples)

    def test_ladder_requires_both_saturation_and_plateau(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent.parent / 'build') as tmp:
            for case in ('idle', 'gaining', 'p1'):
                cell = 'GET-p1-2s-rl0' if case == 'p1' else 'GET-p32-2s-rl0'
                args = SimpleNamespace(output=str(Path(tmp) / case), max_instances=4)
                reference = dict(rate=1000000, bound=dict(log_bound=.001, loss_fraction=.001))
                def fake_trial(args, cell, instances, tag):
                    return dict(cell=cell, instances=instances, saturated=case == 'gaining',
                                minimum_busy=1.0 if case == 'gaining' else .01,
                                generator_max_cpu=.2,
                                rate=1000000 * (instances if case == 'gaining' else 1))
                with patch.object(perf, 'trial', fake_trial), contextlib.redirect_stdout(io.StringIO()):
                    result = perf.collect(args, cell, reference)
                self.assertEqual(len(result['ladder']), 4)
                self.assertEqual(result['verdict'], 'ok' if case == 'p1' else 'FAIL')
                if case != 'p1':
                    self.assertIn('could not saturate / establish generator plateau', result['reason'])

    def test_missing_and_unarmed_refs_exit_three_loudly(self):
        # TemporaryDirectory is inside the worktree as required for this project.
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent.parent / 'build') as tmp:
            path = Path(tmp) / 'refs.json'
            for exists in (False, True):
                if exists:
                    path.write_text(json.dumps({'schema': 1, 'armed': False, 'reason': 'test'}))
                output = io.StringIO()
                with contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as exc:
                    perf.load_reference(path)
                self.assertEqual(exc.exception.code, 3)
                self.assertIn('UNARMED', output.getvalue())

    def test_reference_cannot_omit_cells(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent.parent / 'build') as tmp:
            path = Path(tmp) / 'refs.json'
            path.write_text(json.dumps({'schema': 1, 'armed': True, 'cells': {}}))
            with self.assertRaisesRegex(AssertionError, 'incomplete'):
                perf.load_reference(path)


class LedgerWiring(unittest.TestCase):
    def run_block(self, kind, rc=0):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        if kind == 'feature':
            start = gate.index('# ---- A. mandatory feature')
            end = gate.index('if [ "$TIER" = quick ]; then', start)
        else:
            start = gate.index('# ---- B. mandatory loopback')
            end = gate.index('# ---- 4. full tier:', start)
        # Exercise ONLY the newly added shell loops, with all CPU work stubbed. This never
        # invokes gate.sh, starts a server, builds, or runs any pre-existing battery.
        prelude = '''PASS=0
FAIL=0
CORES=8-15
GATE_RATIO=6:2
py(){ return "$WIRE_RC"; }
timeout(){ return "$WIRE_RC"; }
quiet_wait(){ :; }
ok(){ PASS=$((PASS+1)); }
bad(){ FAIL=$((FAIL+1)); }
say(){ :; }
'''
        tail = '\nprintf "%s %s %s\\n" "$PASS" "$FAIL" "${PERF_UNARMED:-0}"\n'
        with tempfile.TemporaryDirectory(dir=root / 'build') as directory:
            env = dict(os.environ, WIRE_RC=str(rc), GATE_FEATURE_OUTPUT=directory, GATE_PERF_OUTPUT=directory)
            result = subprocess.run(['taskset', '-c', str(min(os.sched_getaffinity(0))), 'bash', '-c',
                                     prelude + gate[start:end] + tail], cwd=root, env=env,
                                    text=True, capture_output=True, check=True)
        return result.stdout.strip()

    def test_feature_rows_precede_quick_exit(self):
        self.assertEqual(self.run_block('feature'), '35 0 0')
        self.assertEqual(self.run_block('feature', 1), '0 35 0')

    def test_full_perf_counts_missing_refs_as_failures(self):
        self.assertEqual(self.run_block('performance'), '32 0 0')
        self.assertEqual(self.run_block('performance', 3), '0 32 1')
        self.assertEqual(self.run_block('performance', 1), '0 32 0')


if __name__ == '__main__':
    unittest.main()
