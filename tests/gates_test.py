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
import shlex
import signal
import sys
import time
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
    def run_block(self, kind, rc=0, abba_rc=0):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        if kind == 'feature':
            start = gate.index('# ---- A. mandatory feature')
            end = gate.index('if [ "$TIER" = quick ]; then', start)
        else:
            marker = gate.index('# ---- B. mandatory headline performance')
            start = gate.index('python3 tests/abbagate.py "${ABBA_ARGS[@]}"', marker)
            end = gate.index('\nesac', start) + len('\nesac')
        definitions = ''
        if kind == 'feature':
            definitions = gate[gate.index('job_feature_cell(){'):gate.index('job_asan_batteries(){')]
        # Execute the production shell verdict branches, replacing only the CPU-work boundary.
        # In the quick block the 35 feature rows and ABBA self-test remain separate assertions.
        prelude = '''PASS=0
FAIL=0
CORES=8-15
PORT=19000
CANDIDATE_BINARY=/unused
GATE_RATIO=6:2
ABBA_ARGS=()
FEATURE_OUTPUT="$GATE_FEATURE_OUTPUT"
collect_job(){
  case "$1" in
    feature-cell-*) job_feature_cell "$1";;
    abba_selftest) job_abba_selftest;;
    *) return 90;;
  esac
}
py(){
  if [ "$1" = tests/abbagate.py ] || [ "$1" = tests/gate_history.py ]; then return "$WIRE_ABBA_RC"; fi
  return "$WIRE_RC"
}
python3(){ return "$WIRE_ABBA_RC"; }
quiet_wait(){ :; }
row_begin(){ :; }
ok(){ printf 'ok\\t%s\\t\\n' "$1" >> "$WIRE_LEDGER"; }
bad(){ printf 'FAIL\\t%s\\t%s\\n' "$1" "${2:-}" >> "$WIRE_LEDGER"; }
say(){ :; }
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as directory:
            ledger = Path(directory) / 'rows.tsv'
            env = dict(os.environ, WIRE_RC=str(rc), WIRE_ABBA_RC=str(abba_rc),
                       WIRE_LEDGER=str(ledger), TMPDIR=directory, GATE_FEATURE_OUTPUT=directory)
            subprocess.run(['taskset', '-c', str(min(os.sched_getaffinity(0))), 'bash', '-uc',
                            prelude + definitions + gate[start:end]], cwd=root, env=env,
                           text=True, capture_output=True, check=True, timeout=10)
            return [line.split('\t') for line in ledger.read_text().splitlines()]

    def test_feature_rows_precede_quick_exit(self):
        for rc in (0, 1, 3):
            with self.subTest(rc=rc):
                rows = self.run_block('feature', rc)
                self.assertEqual([row[1] for row in rows[:-1]], ['feature ' + cell for cell in feature.CELLS])
                self.assertEqual([row[0] for row in rows[:-1]], ['FAIL' if rc else 'ok'] * 35)
                self.assertEqual(rows[-1][:2], ['ok', 'ABBA comparison + saturation negative controls'])

    def test_quick_abba_self_test_is_one_independent_failure_row(self):
        for rc in (1, 3):
            rows = self.run_block('feature', abba_rc=rc)
            self.assertEqual([row[0] for row in rows], ['ok'] * 35 + ['FAIL'])
            self.assertEqual(rows[-1][1], 'ABBA comparison + saturation negative controls')

    def test_full_abba_counts_missing_refs_and_measurement_errors_as_failures(self):
        for rc in (0, 1, 3):
            with self.subTest(rc=rc):
                rows = self.run_block('performance', abba_rc=rc)
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0][:2], ['FAIL' if rc else 'ok', 'headline ABBA vs last pushed binary'])
                if rc == 3:
                    self.assertIn('SKIPPED -- NOT A PASS', rows[0][2])


class ABBATermination(unittest.TestCase):
    def test_terminating_gate_reaps_driver_and_owned_child_but_not_foreign_process(self):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        cleanup = gate[gate.index('cleanup(){ # EXIT/INT/TERM:'):gate.index('\nport_listeners(){')]
        marker = gate.index('# ---- B. mandatory headline performance')
        start = gate.index('python3 tests/abbagate.py "${ABBA_ARGS[@]}"', marker)
        launch = gate[start:gate.index('\nesac', start) + len('\nesac')]
        with tempfile.TemporaryDirectory(dir=root / 'build') as tmp:
            directory = Path(tmp)
            driver = directory / 'mock-driver.py'
            # Only the measuring boundary is fake. The real ABBA Children class starts and
            # reaps a harmless sleeping child in its private session, exactly like a server.
            driver.write_text('''import json, os, signal, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from abbagate import Children
out=Path(sys.argv[2]); children=Children()
def interrupted(signum, frame):
    raise InterruptedError(signum)
signal.signal(signal.SIGTERM, interrupted)
try:
    child=children.start([sys.executable, '-c', 'import time; time.sleep(60)'], out/'child.log', out)
    (out/'ready.json').write_text(json.dumps({'driver':os.getpid(), 'child':child.pid}))
    while True: signal.pause()
except InterruptedError:
    pass
finally:
    children.close()
    (out/'cleaned').write_text('owned child reaped')
''')
            prelude = '''set -u
SRV=0; GLOBCASE_ORACLE=0; MMPID=0; PAUSABLE_PID=0; ABBA_PID=0; ABBA_ARGS=()
stop_workers(){ :; }
python3(){ exec "$WIRE_PYTHON" "$WIRE_DRIVER" "$WIRE_TESTS" "$WIRE_OUTPUT"; }
ok(){ printf 'ok\\n' >> "$WIRE_OUTPUT/verdict"; }
bad(){ printf 'FAIL\\n' >> "$WIRE_OUTPUT/verdict"; }
'''
            env = dict(os.environ, WIRE_PYTHON=sys.executable, WIRE_DRIVER=str(driver),
                       WIRE_TESTS=str(root / 'tests'), WIRE_OUTPUT=str(directory))
            foreign = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
            process = subprocess.Popen(['bash', '-c', prelude + '\n' + cleanup + '\n' + launch],
                                       cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            pids = {}
            try:
                deadline = time.monotonic() + 5
                ready = directory / 'ready.json'
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        pids = json.loads(ready.read_text())
                        break
                    except (FileNotFoundError, json.JSONDecodeError):
                        time.sleep(.01)
                self.assertTrue(pids, 'mock ABBA never reached its measurement boundary')
                process.terminate()  # Only the gate PID receives TERM from the caller.
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 130, (stdout, stderr))
                self.assertTrue((directory / 'cleaned').exists(), 'gate left its ABBA driver running')
                self.assertFalse(Path(f"/proc/{pids['child']}").exists(), 'owned child was not reaped')
                self.assertFalse(Path(f"/proc/{pids['driver']}").exists(), 'owned driver was not reaped')
                self.assertIsNone(foreign.poll(), 'unrelated process received a signal')
                self.assertFalse((directory / 'verdict').exists(), 'interrupted measurement emitted a verdict')
            finally:
                # The negative version of this test leaves a driver behind. Reap only the PIDs
                # this fixture recorded, so a failed cleanup assertion cannot contaminate a run.
                if process.poll() is None:
                    process.kill()
                if pids and not (directory / 'cleaned').exists():
                    try:
                        os.kill(pids['driver'], signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    deadline = time.monotonic() + 5
                    while not (directory / 'cleaned').exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    if not (directory / 'cleaned').exists():
                        for pid in pids.values():
                            try:
                                os.kill(pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                process.communicate(timeout=5)
                foreign.terminate()
                foreign.wait(timeout=5)


class SchedulerWiring(unittest.TestCase):
    # Enumerate the real collector loops, not a hand-maintained approximation of their inventory.
    # This invokes only collect_job stubs: no compiler, server, battery or benchmark is started.
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        quick = gate[gate.index('\nstart_workers\n'):gate.index('\nif [ "$TIER" = quick ]; then\n  join_workers')]
        full = gate[gate.index('\ncollect_job asan_batteries\n'):gate.index('\n# Every worker has reaped')]
        stub = 'start_workers(){ :; }; collect_job(){ printf "%s\\n" "$1"; };\n'
        result = subprocess.run(['bash', '-uc', stub + quick + full], cwd=root,
                                text=True, capture_output=True, check=True)
        cls.canonical = result.stdout.splitlines()
        if len(cls.canonical) != len(set(cls.canonical)):
            raise AssertionError('collector inventory repeats a job')

    def run_scheduler(self, *, reverse=False, slots=None, failure='', behavior='', ordered=True,
                      delayed_completion=False, dependency_probe=False):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        ledger_functions = gate[gate.index('say(){'):gate.index('\nledger_labels(){')]
        placement = gate[gate.index('set_slot(){'):gate.index('\nset_slot 0')]
        scheduler = gate[gate.index('WORKER_PIDS=()'):gate.index('# ---- 0. preflight:')]
        order = self.canonical[::-1] if reverse else self.canonical
        prelude = '''set -u
PASS=0; FAIL=0; TIER=full; CORES=0; LOAD_CORES=0; PORT=19000; GATE_RATIO=6:2; ALL_BUILD_CORES=0
LEDGER="$RUN_DIR/ledger"; TIMINGS="$RUN_DIR/timings"; ROW_T=$(date +%s.%N)
: > "$LEDGER"; : > "$TIMINGS"
mkdir -p "$RUN_DIR/jobs" "$RUN_DIR/started" "$RUN_DIR/completed"
mkfifo "$RUN_DIR/pause"; exec 3<>"$RUN_DIR/pause"
pause(){ read -r -t .02 -u 3 ignored || :; }
phase(){ :; }
quiet_wait(){ :; }
ROW_HISTORY="$RUN_DIR/history"; ROW_RUN_ID=test; ROW_PLAN="$RUN_DIR/plan.json"
python3 tests/gate_history.py prepare --history "$ROW_HISTORY" --output "$ROW_PLAN"
cleanup(){ row_unwatch; [ -z "${name:-}" ] || : > "$TMPDIR/cleaned"; }
'''
        if delayed_completion:
            # Widen the real open-before-write window past the collector's polling interval.
            # Readers must wait for publication even when the writer is descheduled here.
            prelude += '''
printf(){
  local writer_pid=$BASHPID target
  target=$(readlink "/proc/$writer_pid/fd/1")
  case "$target" in */done|*/done.tmp) sleep .6;; esac
  builtin printf "$@"
}
'''
        # File barriers force the opposite completion order without relying on sleep durations
        # or machine scheduling. No stub opens a socket or invokes a server/build/benchmark.
        stub = '''
job_body(){
  local current=$1 dependency
  : > "$RUN_DIR/started/$current"
  if ! job_ready "$current"; then echo "started $current before dependency completed" >&2; exit 18; fi
  if [ "$current" = production_units ]; then
    : > "$RUN_DIR/completed/$current"
    return 0
  fi
  if [ "$DEPENDENCY_PROBE" = 1 ]; then
    if [ "$current" = release ]; then
      while [ ! -f "$RUN_DIR/started/asan" ]; do pause; done
    elif [ "$current" = asan ]; then
      # Release-only correctness must be dispatched before the independent ASAN build finishes.
      while [ ! -f "$RUN_DIR/started/release_batteries" ]; do pause; done
    fi
  fi
  if [ "$GATE_SLOTS" != 1 ] && [ "$FORCE_ORDER" = 1 ]; then
    for dependency in "${CANONICAL[@]}"; do
      while [ ! -f "$RUN_DIR/started/$dependency" ]; do pause; done
    done
  fi
  if [ "$GATE_SLOTS" != 1 ] && [ "$FORCE_ORDER" = 1 ]; then
    for dependency in "${COMPLETION_ORDER[@]}"; do
      [ "$dependency" != "$current" ] || break
      while [ ! -f "$RUN_DIR/completed/$dependency" ]; do pause; done
    done
  fi
  row_begin "$(job_label "$current")"
  if [ "$current" = "$FAILED_JOB" ] && [ "$FAILURE_BEHAVIOR" = crash ]; then exit 17; fi
  if [ "$current" = "$FAILED_JOB" ] && [ "$FAILURE_BEHAVIOR" = red ]; then
    bad "$(job_label "$current")" 'deliberately broken mechanism'
  elif [ "$current" != "$FAILED_JOB" ] || [ "$FAILURE_BEHAVIOR" != empty ]; then
    ok "$(job_label "$current")"
  fi
  printf '%s\\n' "$current" >> "$RUN_DIR/completion-order"
  : > "$RUN_DIR/completed/$current"
  if [ "$current" = "$FAILED_JOB" ] && [ "$FAILURE_BEHAVIOR" = return ]; then return 17; fi
  return 0
}
# Completion-order probes deliberately remove dependency edges; the separate two-slot probe
# exercises the real readiness graph and proves release work does not wait for ASAN.
if [ "$FORCE_ORDER" = 1 ]; then job_dependencies(){ :; }; fi
start_workers
for requested in "${CANONICAL[@]}"; do collect_job "$requested"; done
join_workers
printf '%s %s\\n' "$PASS" "$FAIL" > "$RUN_DIR/counts"
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as tmp:
            directory = Path(tmp)
            env = dict(os.environ, RUN_DIR=tmp, FAILED_JOB=failure, FAILURE_BEHAVIOR=behavior,
                       FORCE_ORDER=str(int(ordered)), DEPENDENCY_PROBE=str(int(dependency_probe)),
                       GATE_FEATURE_OUTPUT=str(directory / 'features'))
            count = slots or len(self.canonical) + 1
            cpu = str(min(os.sched_getaffinity(0)))
            arrays = '\n'.join([
                f'GATE_SLOTS={count}',
                'CANONICAL=(' + ' '.join(map(shlex.quote, self.canonical)) + ')',
                'COMPLETION_ORDER=(' + ' '.join(map(shlex.quote, order)) + ')',
                'SLOT_CORES=(' + ' '.join([cpu] * count) + ')',
                'SLOT_LOAD_CORES=(' + ' '.join([cpu] * count) + ')',
                'SLOT_PORTS=(' + ' '.join(str(19000 + 3 * index) for index in range(count)) + ')',
            ])
            script = '\n'.join((prelude, arrays, ledger_functions, placement, scheduler,
                                'trap stop_workers EXIT', stub))
            result = subprocess.run(['timeout', '--kill-after=2', '45', 'taskset', '-c', cpu,
                                     'bash', '-c', script], cwd=root, env=env,
                                    text=True, capture_output=True, timeout=50)
            self.assertEqual(result.returncode, 0, result.stdout[-1000:] + result.stderr[-2000:])
            timed_rows = [line.split('\t') for line in (directory / 'ledger').read_text().splitlines()]
            for row in timed_rows:
                self.assertEqual(len(row), 3)
                self.assertGreaterEqual(float(row[1]), 0)
            return dict(ledger=(''.join(f'{v}\t{label}\n' for v, duration, label in timed_rows)).encode(),
                        counts=tuple(map(int, (directory / 'counts').read_text().split())),
                        completion=(directory / 'completion-order').read_text().splitlines(),
                        families=[line.split('\t') for line in (directory / 'families.tsv').read_text().splitlines()
                                  if not line.startswith('production_units\t')],
                        helpers={path.parent.name for path in (directory / 'jobs').glob('*/done')
                                 if path.parent.name == 'production_units'},
                        cleaned={path.parent.name for path in (directory / 'jobs').glob('*/cleaned')
                                 if path.parent.name != 'production_units'})

    def test_opposite_completion_orders_have_byte_identical_canonical_ledgers(self):
        forward = self.run_scheduler()
        reverse = self.run_scheduler(reverse=True)
        self.assertEqual(forward['completion'], self.canonical)
        self.assertEqual(reverse['completion'], self.canonical[::-1])
        self.assertEqual(forward['ledger'], reverse['ledger'])
        self.assertEqual(forward['counts'], (len(self.canonical), 0))
        self.assertEqual({row[0] for row in reverse['families']}, set(self.canonical))
        self.assertEqual(reverse['cleaned'], set(self.canonical))

    def test_empty_fragment_and_explicit_failure_cannot_turn_green(self):
        for behavior in ('empty', 'red'):
            with self.subTest(behavior=behavior):
                result = self.run_scheduler(failure='flipctl', behavior=behavior)
                self.assertEqual(result['counts'], (len(self.canonical) - 1, 1))
                self.assertIn(b'FAIL\tflip controller: ramp gate, hold, surge + mix re-maneuvers\n', result['ledger'])

    def test_worker_exit_before_done_is_a_failure(self):
        # Last in completion order, so its deliberate exit cannot block another stub's barrier.
        result = self.run_scheduler(failure=self.canonical[-1], behavior='crash')
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1))
        self.assertIn(b'FAIL\tcorrectness family globcase\n', result['ledger'])

    def test_nonzero_worker_return_cannot_be_hidden_by_a_pass_fragment(self):
        result = self.run_scheduler(failure='flipctl', behavior='return')
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1))
        self.assertIn(b'FAIL\tflip controller: ramp gate, hold, surge + mix re-maneuvers\n', result['ledger'])

    def test_completion_is_not_visible_before_its_record_is_written(self):
        result = self.run_scheduler(delayed_completion=True)
        self.assertEqual(result['counts'], (len(self.canonical), 0))

    def test_limited_workers_reuse_slots_without_losing_or_repeating_jobs(self):
        result = self.run_scheduler(slots=3, ordered=False)
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertEqual(result['counts'], (len(self.canonical), 0))
        self.assertEqual(len(result['families']), len(self.canonical))
        self.assertLessEqual({row[1] for row in result['families']}, {'0', '1', '2'})

    def test_one_slot_uses_the_same_queue_without_losing_coverage(self):
        result = self.run_scheduler(slots=1)
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertEqual(result['counts'], (len(self.canonical), 0))
        self.assertEqual({row[1] for row in result['families']}, {'0'})
        self.assertEqual(result['cleaned'], set(self.canonical))

    def test_failed_prerequisite_cannot_leave_the_queue_waiting_forever(self):
        result = self.run_scheduler(slots=2, ordered=False, failure='release', behavior='crash')
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1))
        self.assertIn(b'FAIL\tcorrectness family release\n', result['ledger'])
        self.assertEqual(result['helpers'], {'production_units'})
        self.assertCountEqual(result['completion'], [name for name in self.canonical if name != 'release'])

    def test_release_boots_do_not_wait_for_independent_asan_build(self):
        result = self.run_scheduler(slots=2, ordered=False, dependency_probe=True)
        self.assertEqual(result['counts'], (len(self.canonical), 0))
        self.assertEqual(result['helpers'], {'production_units'})
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertLess(result['completion'].index('release_batteries'), result['completion'].index('asan'))
        families = {row[0]: row for row in result['families']}
        self.assertGreaterEqual(float(families['release_batteries'][2]), float(families['release'][3]))


class PerfCandidateDispatch(unittest.TestCase):
    def dispatch(self, build_candidate, build_rc=0, unquiet=False):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        start = gate.index('if [ "$TIER" = perf ]; then')
        branch = gate[start:gate.index('\nPASS=0; FAIL=0', start)]
        # Exercise the real branch with shell stubs: neither make, taskset, nor ABBA executes.
        stub = '''set -u
TIER=perf; BUILD_CORES=0-15; BUILD_JOBS=16
ABBA_ARGS=(--candidate-binary "$RUN_DIR/candidate")
taskset(){ printf 'BUILD %s\\n' "$*" >> "$EVENTS"; return "$BUILD_RC"; }
exec(){ printf 'ABBA %s\\n' "$*" >> "$EVENTS"; }
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            events = directory / 'events'
            env = dict(os.environ, RUN_DIR=temporary, EVENTS=str(events),
                       BUILD_CANDIDATE=str(build_candidate), BUILD_RC=str(build_rc),
                       GATE_QUIET_FILE=str(directory / 'missing-quiet') if unquiet else '')
            result = subprocess.run(['bash', '-c', stub + branch], cwd=root, env=env,
                                    text=True, capture_output=True, timeout=5)
            return result, events.read_text().splitlines() if events.exists() else []

    def test_omitted_candidate_builds_before_abba(self):
        result, events = self.dispatch(1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(events), 2)
        self.assertEqual(events[0], 'BUILD -c 0-15 make -j16')
        self.assertTrue(events[1].startswith('ABBA python3 tests/abbagate.py --candidate-binary '))

    def test_explicit_candidate_bypasses_build(self):
        result, events = self.dispatch(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(events), 1)
        self.assertTrue(events[0].startswith('ABBA '))

    def test_failed_build_cannot_measure_stale_candidate(self):
        result, events = self.dispatch(1, 17)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(events, ['BUILD -c 0-15 make -j16'])
        self.assertIn('candidate build failed', result.stderr)

    def test_unquiet_box_cannot_start_candidate_build(self):
        result, events = self.dispatch(1, unquiet=True)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(events, [])
        self.assertIn('candidate build not started', result.stderr)


class EarlyGateDispatch(unittest.TestCase):
    def test_perf_self_test_reaches_abba_and_common_options_reach_planner(self):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        branch = gate[gate.index('GATE_SELF_TEST=0'):gate.index('GATE_STARTED=$SECONDS')]
        stub = 'exec(){ printf "%s\\n" "$*"; exit 0; }\n'
        cases = [(['perf', '--self-test'], 'python3 tests/abbagate.py --self-test'),
                 (['quick', '--self-test'], 'python3 tests/gateplan.py quick --self-test'),
                 (['perf', '--help'], 'python3 tests/gateplan.py perf --help'),
                 (['perf', '--json'], 'python3 tests/gateplan.py perf --json')]
        for argv, expected in cases:
            with self.subTest(argv=argv):
                result = subprocess.run(['bash', '-c', stub + branch, 'gate.sh', *argv],
                                        cwd=root, text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)


if __name__ == '__main__':
    (Path(__file__).resolve().parents[1] / 'build').mkdir(exist_ok=True)
    unittest.main()
