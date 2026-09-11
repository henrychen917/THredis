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
import shutil
import signal
import stat
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
    instrument_helpers = ('tests/abbagate.py', 'tests/gate_measurements.py',
                          'tests/background_environment_test.py', 'tests/gate_history.py',
                          'tests/gate_process_test.py', 'tests/gates_test.py')

    def run_block(self, kind, rc=0, abba_rc=0, abba_helper=''):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        if kind == 'feature':
            start = gate.index('# ---- A. mandatory feature')
            end = gate.index('if [ "$TIER" = quick ]; then', start)
        else:
            marker = gate.index('# ---- B. mandatory headline performance')
            start = gate.index('ABBA_WATCH_START=missing\n', marker)
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
ABBA_OUTPUT="$TMPDIR/abba"
# Dispatch uses an owned controller identity as the fixture's watchdog stand-in. The actual
# watcher command/parent validation is covered by gate_quiet's real-process controls.
ROW_WATCHDOG=$BASHPID
FEATURE_OUTPUT="$GATE_FEATURE_OUTPUT"
collect_job(){
  case "$1" in
    feature-cell-*) job_feature_cell "$1";;
    abba_selftest) job_abba_selftest;;
    *) return 90;;
  esac
}
py(){
  # Feature failures and the shared comparator controls are independent rows.
  # New control helpers must not accidentally inherit the feature-cell verdict.
  case "$1" in
    tests/feature_gate.py) return "$WIRE_RC";;
    tests/abbagate.py|tests/gate_measurements.py|tests/background_environment_test.py|tests/gate_history.py|tests/gate_process_test.py|tests/gates_test.py)
      printf '%s\\n' "$1" >> "$WIRE_CONTROLS"
      if [ -z "$WIRE_ABBA_HELPER" ] || [ "$1" = "$WIRE_ABBA_HELPER" ]; then
        return "$WIRE_ABBA_RC"
      fi
      return 0;;
    *) return 90;;
  esac
}
python3(){
  if [ "$WIRE_KIND" = performance ]; then
    printf '%s\\0' "$@" > "$WIRE_ARGV"
    printf '%s\\n' "$GATE_QUIET_WATCHDOG" > "$WIRE_WATCHDOG"
  fi
  return "$WIRE_ABBA_RC"
}
quiet_wait(){ :; }
row_begin(){ :; }
ok(){ printf 'ok\\t%s\\t\\n' "$1" >> "$WIRE_LEDGER"; }
bad(){ printf 'FAIL\\t%s\\t%s\\n' "$1" "${2:-}" >> "$WIRE_LEDGER"; }
say(){ :; }
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as directory:
            ledger = Path(directory) / 'rows.tsv'
            env = dict(os.environ, WIRE_RC=str(rc), WIRE_ABBA_RC=str(abba_rc),
                       WIRE_LEDGER=str(ledger), TMPDIR=directory, GATE_FEATURE_OUTPUT=directory,
                       WIRE_KIND=kind, WIRE_ARGV=str(Path(directory) / 'argv'),
                       WIRE_ABBA_HELPER=abba_helper, WIRE_CONTROLS=str(Path(directory) / 'controls'),
                       WIRE_WATCHDOG=str(Path(directory) / 'watchdog'))
            env.pop('GATE_QUIET_WATCHDOG', None)
            subprocess.run(['taskset', '-c', str(min(os.sched_getaffinity(0))), 'bash', '-uc',
                            prelude + definitions + gate[start:end]], cwd=root, env=env,
                           text=True, capture_output=True, check=True, timeout=10)
            if kind == 'feature':
                helpers = list(self.instrument_helpers)
                if abba_rc:
                    helpers = helpers[:helpers.index(abba_helper) + 1] if abba_helper else helpers[:1]
                self.assertEqual((Path(directory) / 'controls').read_text().splitlines(), helpers)
            if kind == 'performance':
                argv = (Path(directory) / 'argv').read_bytes().decode().rstrip('\0').split('\0')
                self.assertEqual(argv, ['tests/abbagate.py', '--output', str(Path(directory) / 'abba')])
                self.assertRegex((Path(directory) / 'watchdog').read_text().strip(), r'^[1-9][0-9]*:[1-9][0-9]*$')
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

    def test_each_instrument_helper_failure_reaches_the_same_gate_row(self):
        # Every helper must be dispatched and propagate both failure and skip status. A
        # newly included scheduler suite must not be hidden behind a permanently red stub.
        for helper in self.instrument_helpers:
            for rc in (1, 3):
                with self.subTest(helper=helper, rc=rc):
                    rows = self.run_block('feature', abba_rc=rc, abba_helper=helper)
                    self.assertEqual([row[0] for row in rows], ['ok'] * 35 + ['FAIL'])
                    self.assertEqual(rows[-1][1], 'ABBA comparison + saturation negative controls')

    def test_full_abba_counts_missing_refs_and_measurement_errors_as_failures(self):
        for rc in (0, 1, 3):
            with self.subTest(rc=rc):
                rows = self.run_block('performance', abba_rc=rc)
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0][:2], ['FAIL' if rc else 'ok', 'headline ABBA vs last pushed binary'])
                if rc == 3:
                    # Missing references and successful untrusted/partial measurements both
                    # return 3. Neither may become a green counted performance row.
                    self.assertIn('no trusted comparison PASS', rows[0][2])
                    self.assertIn('partial diagnostic or skipped', rows[0][2])


class ABBATermination(unittest.TestCase):
    def test_terminating_gate_reaps_driver_and_owned_child_but_not_foreign_process(self):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        cleanup = gate[gate.index('reap_children(){'):gate.index('\nport_listeners(){')]
        marker = gate.index('# ---- B. mandatory headline performance')
        start = gate.index('ABBA_WATCH_START=missing\n', marker)
        launch = gate[start:gate.index('\nesac', start) + len('\nesac')]
        with tempfile.TemporaryDirectory(dir=root / 'build') as tmp:
            directory = Path(tmp)
            driver = directory / 'mock-driver.py'
            # Only the measuring boundary is fake. The real ABBA Children class starts and
            # reaps a harmless sleeping child in its private session, exactly like a server.
            driver.write_text('''import json, os, signal, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from abbagate import Children, parse_args
sys.argv=['abbagate.py', *sys.argv[2:]]
args=parse_args(); out=args.output
assert out == Path(os.environ['WIRE_OUTPUT']), 'production output argument was lost'
token=os.environ['GATE_QUIET_WATCHDOG']
parent,start=map(int, token.split(':'))
assert parent == os.getppid(), 'fixture controller identity was not transmitted'
fields=Path(f'/proc/{parent}/stat').read_text().rsplit(')',1)[1].split()
assert int(fields[19]) == start, 'fixture controller start identity changed'
(out/'dispatch.json').write_text(json.dumps({'argv':sys.argv, 'watchdog':token}))
children=Children()
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
SRV=0; GLOBCASE_ORACLE=0; MMPID=0; PAUSABLE_PID=0; ABBA_PID=0; ABBA_ARGS=(); WORKER_PIDS=()
ABBA_OUTPUT="$WIRE_OUTPUT"; ROW_WATCHDOG=$BASHPID
stop_workers(){ :; }
row_unwatch(){ :; }
python3(){
  if [ "$1" = tests/abbagate.py ]; then shift; exec "$WIRE_PYTHON" "$WIRE_DRIVER" "$WIRE_TESTS" "$@"
  else command "$WIRE_PYTHON" "$@"; fi
}
ok(){ printf 'ok\\n' >> "$WIRE_OUTPUT/verdict"; }
bad(){ printf 'FAIL\\n' >> "$WIRE_OUTPUT/verdict"; }
'''
            env = dict(os.environ, WIRE_PYTHON=sys.executable, WIRE_DRIVER=str(driver),
                       WIRE_TESTS=str(root / 'tests'), WIRE_OUTPUT=str(directory))
            env.pop('GATE_QUIET_WATCHDOG', None)
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
                if not pids and process.poll() is not None:
                    stdout, stderr = process.communicate(timeout=1)
                    self.fail(f'mock ABBA never reached its measurement boundary: {stdout!r} {stderr!r}')
                self.assertTrue(pids, 'mock ABBA never reached its measurement boundary')
                dispatch = json.loads((directory / 'dispatch.json').read_text())
                self.assertEqual(dispatch['argv'], ['abbagate.py', '--output', str(directory)])
                self.assertRegex(dispatch['watchdog'], r'^[1-9][0-9]*:[1-9][0-9]*$')
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


def preserve_scheduler_failure(root, directory, script, stdout='', stderr=''):
    saved = Path(tempfile.mkdtemp(prefix='scheduler-failure-', dir=root / 'build'))
    # Write the command evidence first; a copy error must not erase the reason for preserving it.
    (saved / 'fixture.sh').write_text(script)
    (saved / 'command.stdout').write_text(stdout)
    (saved / 'command.stderr').write_text(stderr)

    def copy_artifact(source, target):
        mode = os.stat(source).st_mode
        if stat.S_ISFIFO(mode):
            # The scheduler's pause FIFO is a rendezvous object, with no file contents to read.
            os.mkfifo(target, stat.S_IMODE(mode))
            return target
        return shutil.copy2(source, target)

    try:
        shutil.copytree(directory, saved, dirs_exist_ok=True, copy_function=copy_artifact)
    except OSError as exc:
        raise RuntimeError(f'Scheduler failure artifacts partially preserved in {saved}: {exc}') from exc
    return saved


class WorkerCompletion(unittest.TestCase):
    def collect(self, completion, ledger='ok\t0.125000\tfixture\n'):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        collector = gate[gate.index('collect_job(){'):gate.index('\njoin_workers(){')]
        # Execute the real collector with already published artifacts. This isolates the
        # failure evidence boundary without starting workers or replacing verdict logic.
        stub = r'''
set -u
PASS=0; FAIL=0; WORKER_PIDS=()
LEDGER="$RUN_DIR/ledger"; TIMINGS="$RUN_DIR/timings"
: > "$LEDGER"; : > "$TIMINGS"
job_label(){ printf 'fixture\n'; }
bad(){ printf 'FAIL\t0\t%s\n' "$1" >> "$LEDGER"; FAIL=$((FAIL+1)); }
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as temporary:
            directory = Path(temporary)
            job = directory / 'jobs/fixture'
            job.mkdir(parents=True)
            (job / 'ledger').write_text(ledger)
            (job / 'timings').write_text(ledger)
            (job / 'output.log').write_text('fixture output\n')
            (job / 'done').write_text(completion)
            result = subprocess.run(['bash', '-c', stub + collector +
                                     '\ncollect_job fixture\nprintf "%s %s\\n" "$PASS" "$FAIL"\n'],
                                    cwd=root, env=dict(os.environ, RUN_DIR=temporary),
                                    text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return tuple(map(int, result.stdout.splitlines()[-1].split())), (directory / 'ledger').read_text()

    def test_matching_completion_preserves_both_verdicts(self):
        self.assertEqual(self.collect('0\t1\t0\n')[0], (1, 0))
        self.assertEqual(self.collect('1\t0\t1\n', 'FAIL\t0.125000\tfixture\n')[0], (0, 1))

    def test_collection_preserves_each_rows_own_duration(self):
        # Collection happens in canonical order after arbitrary worker completion. The
        # coordinator must preserve the worker's measured spans, including a failed row;
        # collector time or time since the previous public row is unrelated to either.
        fragment = 'ok\t0.125000\tfirst\nFAIL\t1.750000\tsecond\n'
        counts, ledger = self.collect('1\t1\t1\n', fragment)
        self.assertEqual(counts, (1, 1))
        self.assertEqual(ledger, fragment)

    def test_explicit_failure_or_wrong_counts_cannot_be_hidden_by_a_passing_fragment(self):
        for completion in ('0\t1\t1\n', '0\t0\t1\n', '0\t2\t0\n', '0\t0\t0\n',
                           '1\t1\t0\n'):
            with self.subTest(completion=completion):
                counts, ledger = self.collect(completion)
                self.assertEqual(counts, (0, 1))
                self.assertEqual(ledger, 'FAIL\t0\tfixture\n')

    def test_completion_must_be_one_complete_record(self):
        for completion in ('', '0\t1\n', '0\t1\t0\n0\t0\t1\n', '0\t1\t0\n\n',
                           '256\t1\t0\n', '999999999999999999999999\t1\t0\n'):
            with self.subTest(completion=completion):
                self.assertEqual(self.collect(completion)[0], (0, 1))

    def test_empty_or_malformed_fragment_cannot_supply_passing_rows(self):
        for ledger in ('', 'ok\tfixture\n', 'ok\tNaN\tfixture\n', 'ok\t-1\tfixture\n',
                       'ok\t0.125000\t\n', 'ok\t0.125000\tfixture\nBROKEN\t0\tother\n'):
            with self.subTest(ledger=ledger):
                self.assertEqual(self.collect('0\t1\t0\n', ledger)[0], (0, 1))


class SchedulerWiring(unittest.TestCase):
    helper_jobs = frozenset(('production_units', 'core_tsan_build', 'waits_tsan_build'))

    # Enumerate the real collector loops, not a hand-maintained approximation of their inventory.
    # This invokes only collect_job stubs: no compiler, server, battery or benchmark is started.
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        quick = gate[gate.index('\nstart_workers\n'):gate.index('\nif [ "$TIER" = quick ]; then\n  join_workers')]
        full = gate[gate.index('\ncollect_job asan_batteries\n'):gate.index('\n# Every worker has reaped')]
        stub = '''start_workers(){ :; }
collect_job(){
  case "$1" in
    differ-split) printf '%s\\n' differ-split-0 differ-split-1 differ-equivalence;;
    differ-armed) printf '%s\\n' differ-armed-0 differ-armed-1;;
    *) printf '%s\\n' "$1";;
  esac
}
'''
        result = subprocess.run(['bash', '-uc', stub + quick + full], cwd=root,
                                text=True, capture_output=True, check=True)
        cls.canonical = result.stdout.splitlines()
        if len(cls.canonical) != len(set(cls.canonical)):
            raise AssertionError('collector inventory repeats a job')

    def run_scheduler(self, *, reverse=False, slots=None, failure='', behavior='', ordered=True,
                      delayed_completion=False, dependency_probe=False,
                      remove_atomic_dependency=False, add_asan_dependency=False):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        ledger_functions = gate[gate.index('say(){'):gate.index('\nledger_labels(){')]
        placement = gate[gate.index('set_slot(){'):gate.index('\nset_slot 0')]
        scheduler = gate[gate.index('WORKER_PIDS=()'):gate.index('# ---- 0. preflight:')]
        order = self.canonical[::-1] if reverse else self.canonical
        order = [name for name in order if name != 'atomic_batteries'] + ['atomic_batteries']
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
export GATE_DIFFER_HISTORY="$RUN_DIR/differ-history"
python3 tests/gate_history.py prepare --history "$ROW_HISTORY" --output "$ROW_PLAN"
cleanup(){ row_unwatch; [ -z "${name:-}" ] || : > "$TMPDIR/cleaned"; }
reap_children(){ :; } # Real teardown is covered by gate_history's owned-process controls.
'''
        if delayed_completion:
            # Widen the real open-before-write window past the collector's polling interval.
            # Readers must wait for publication even when the writer is descheduled here.
            # Filter the completion record's format/three integer fields BEFORE readlink: a
            # wrapper that forks for every printf adds work to every label/ledger operation.
            # With 97 fixture workers on one CPU that instrumentation itself exhausted 45s,
            # invoking timeout teardown while otherwise healthy rows were still completing.
            prelude += '''
mkdir "$RUN_DIR/delayed-publication"
printf(){
  local writer_pid=$BASHPID target
  if [ "${1-}" = '%s\\t%s\\t%s\\n' ] && [ "$#" = 4 ] &&
      [[ "$2" =~ ^[0-9]+$ && "$3" =~ ^[0-9]+$ && "$4" =~ ^[0-9]+$ ]]; then
    target=$(readlink "/proc/$writer_pid/fd/1")
    case "$target" in */done|*/done.tmp)
      local family=${target%/*}; family=${family##*/}
      # Exactly one marker per actual publication. Append exposes a duplicate finalizer;
      # checking markers below makes a narrowed hook that never opens the window fail.
      builtin printf '%s\\t%s\\t%s\\n' "$writer_pid" "$CLEANUP_OWNER" "$EPOCHREALTIME" \\
          >> "$RUN_DIR/delayed-publication/$family"
      sleep .6;;
    esac
  fi
  builtin printf "$@"
}
'''
        # File barriers force the opposite completion order without relying on sleep durations
        # or machine scheduling. No stub opens a socket or invokes a server/build/benchmark.
        stub = '''
job_body(){
  local current=$1 dependency status_key status_value affinity=
  if [ "$current" = "$FAILED_JOB" ] && [ "$FAILURE_BEHAVIOR" = before-affinity ]; then
    echo 'deliberate fixture failure before affinity witness' >&2
    return 17
  fi
  while read -r status_key status_value; do
    if [ "$status_key" = Cpus_allowed_list: ]; then affinity=$status_value; break; fi
  done < "/proc/$BASHPID/status"
  [ "$affinity" = "$LOAD_CORES" ] || { echo "fixture slot affinity differs: $affinity/$LOAD_CORES" >&2; exit 22; }
  printf '%s\t%s\n' "$slot" "$affinity" > "$TMPDIR/fixture-affinity"
  : > "$RUN_DIR/started/$current"
  if ! job_ready "$current"; then echo "started $current before dependency completed" >&2; exit 18; fi
  if [ "$current" = atomic_batteries ]; then
    # This is the first operation of the unchanged production family: before its boot,
    # every other family must have published completion AFTER its owned-process cleanup.
    for dependency in "${JOB_NAMES[@]}"; do
      [ "$dependency" != "$current" ] || continue
      if [ ! -f "$RUN_DIR/jobs/$dependency/done" ] ||
          [ ! -f "$RUN_DIR/jobs/$dependency/cleaned" ]; then
        printf 'atomic boot preceded completion of %s\n' "$dependency" > "$RUN_DIR/exclusivity-failure"
        cat "$RUN_DIR/exclusivity-failure" >&2
        exit 19
      fi
    done
    : > "$RUN_DIR/atomic-boot-reached"
  fi
  case "$current" in
    production_units|core_tsan_build|waits_tsan_build)
      : > "$RUN_DIR/completed/$current"
      return 0;;
  esac
  if [ "$DEPENDENCY_PROBE" = 1 ]; then
    if [ "$current" = release ]; then
      while [ ! -f "$RUN_DIR/started/asan" ]; do pause; done
    elif [ "$current" = asan ]; then
      # The old fixture released ASAN at correctness START, then asserted correctness
      # COMPLETION preceded ASAN. Both legitimate completion orders were possible.
      # Hold ASAN until the post-verdict token, and prove the production readiness
      # predicate admits release correctness while this ASAN job is still unfinished.
      while [ ! -f "$RUN_DIR/jobs/release/done" ]; do pause; done
      : > "$RUN_DIR/dependency-probe-reached"
      if ! job_ready release_batteries; then
        printf 'release correctness blocked by unfinished ASAN\\n' > "$RUN_DIR/dependency-failure"
        cat "$RUN_DIR/dependency-failure" >&2
        exit 23
      fi
      while [ ! -f "$RUN_DIR/completed/release_batteries" ]; do pause; done
      [ ! -f "$TMPDIR/done" ] || exit 24
      : > "$RUN_DIR/dependency-handshake"
    fi
  fi
  if [ "$GATE_SLOTS" != 1 ] && [ "$FORCE_ORDER" = 1 ]; then
    for dependency in "${CANONICAL[@]}"; do
      [ "$dependency" != atomic_batteries ] || continue
      while [ ! -f "$RUN_DIR/started/$dependency" ]; do pause; done
    done
  fi
  if [ "$GATE_SLOTS" != 1 ] && [ "$FORCE_ORDER" = 1 ] && [ "$current" != atomic_batteries ]; then
    # A single predecessor token enforces the same total order transitively. Polling
    # every unfinished predecessor from 97 shells consumed the fixture's 45s budget
    # on one CPU (retained controls reached 92--94 families before timeout).
    if [ "$current" != "${COMPLETION_ORDER[0]}" ]; then
      read -r dependency < "$RUN_DIR/order/$current" || exit 20
      [ "$dependency" = completed ] || exit 21
    fi
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
  if [ "$GATE_SLOTS" != 1 ] && [ "$FORCE_ORDER" = 1 ] && [ "$current" != atomic_batteries ]; then
    local found=0
    for dependency in "${COMPLETION_ORDER[@]}"; do
      [ "$dependency" != atomic_batteries ] || break
      if [ "$found" = 1 ]; then
        printf 'completed\\n' > "$RUN_DIR/order/$dependency"
        break
      fi
      [ "$dependency" != "$current" ] || found=1
    done
  fi
  if [ "$current" = "$FAILED_JOB" ] && [ "$FAILURE_BEHAVIOR" = return ]; then return 17; fi
  return 0
}
# Completion-order probes remove ordinary dependency edges but retain real atomic exclusivity.
# The separate two-slot probe exercises the complete graph. A negative control removes ONLY
# the new edge, proving the pre-boot assertion catches a dispatch overlap on the actual queue.
if [ "$FORCE_ORDER" = 1 ] || [ "$REMOVE_ATOMIC_DEPENDENCY" = 1 ]; then
  original_dependencies=$(declare -f job_dependencies)
  eval "${original_dependencies/job_dependencies/real_job_dependencies}"
  job_dependencies(){
    if [ "$1" = atomic_batteries ]; then
      if [ "$REMOVE_ATOMIC_DEPENDENCY" = 1 ]; then echo release
      else real_job_dependencies "$1"; fi
    elif [ "$FORCE_ORDER" != 1 ]; then real_job_dependencies "$1"
    fi
  }
fi
# Poison only the release-correctness edge. The held ASAN job calls the real
# readiness predicate above, emits an explicit failed row, then finalizes so
# the queue can drain. The negative control never depends on a sleep or deadlock.
if [ "$ADD_ASAN_DEPENDENCY" = 1 ]; then
  original_dependencies=$(declare -f job_dependencies)
  eval "${original_dependencies/job_dependencies/unpoisoned_job_dependencies}"
  job_dependencies(){
    unpoisoned_job_dependencies "$1"
    [ "$1" != release_batteries ] || echo asan
  }
fi
mkdir "$RUN_DIR/order"
for requested in "${CANONICAL[@]}"; do mkfifo "$RUN_DIR/order/$requested"; done
start_workers
for requested in "${CANONICAL[@]}"; do collect_job "$requested"; done
join_workers
printf '%s %s\\n' "$PASS" "$FAIL" > "$RUN_DIR/counts"
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as tmp:
            directory = Path(tmp)
            env = dict(os.environ, RUN_DIR=tmp, FAILED_JOB=failure, FAILURE_BEHAVIOR=behavior,
                       FORCE_ORDER=str(int(ordered)), DEPENDENCY_PROBE=str(int(dependency_probe)),
                       REMOVE_ATOMIC_DEPENDENCY=str(int(remove_atomic_dependency)),
                       ADD_ASAN_DEPENDENCY=str(int(add_asan_dependency)),
                       GATE_FEATURE_OUTPUT=str(directory / 'features'))
            count = slots or len(self.canonical) + 1
            # The full-inventory order probes synchronize about 100 real shells. Pinning
            # all of them to one CPU serialized their watchdog/ledger work and exhausted
            # the unchanged 45s deadline under correctness contention. Use up to four
            # permitted CPUs so publication is exercised concurrently, while an explicit
            # caller affinity still limits the fixture. No jobs or assertions are removed.
            fixture_cpus = list(map(str, sorted(os.sched_getaffinity(0))[:min(4, count)]))
            cpu_list = ','.join(fixture_cpus)
            slot_cpus = [fixture_cpus[index % len(fixture_cpus)] for index in range(count)]
            arrays = '\n'.join([
                f'GATE_SLOTS={count}',
                'CANONICAL=(' + ' '.join(map(shlex.quote, self.canonical)) + ')',
                'COMPLETION_ORDER=(' + ' '.join(map(shlex.quote, order)) + ')',
                'SLOT_CORES=(' + ' '.join(slot_cpus) + ')',
                'SLOT_LOAD_CORES=(' + ' '.join(slot_cpus) + ')',
                'SLOT_PORTS=(' + ' '.join(str(19000 + 3 * index) for index in range(count)) + ')',
            ])
            script = '\n'.join((prelude, arrays, ledger_functions, placement, scheduler,
                                'trap stop_workers EXIT', stub))
            def preserve_failure(stdout='', stderr=''):
                saved = preserve_scheduler_failure(root, directory, script, stdout, stderr)
                return f'\nScheduler failure artifacts: {saved}\n'
            try:
                result = subprocess.run(['timeout', '--kill-after=2', '45', 'taskset', '-c', cpu_list,
                                         'bash', '-c', script], cwd=root, env=env,
                                        text=True, capture_output=True, timeout=50)
            except subprocess.TimeoutExpired as exc:
                def as_text(value):
                    return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
                evidence = preserve_failure(as_text(exc.stdout), as_text(exc.stderr))
                self.fail(f'Scheduler exceeded the outer fixture deadline.{evidence}')
            output = result.stdout + result.stderr
            if result.returncode:
                output += preserve_failure(result.stdout, result.stderr)
            self.assertEqual(result.returncode, 0, output)
            observed_cpus = set()
            try:
                # Collector rows enumerate scored jobs. The three build prerequisites do
                # not emit rows in this fixture, but must still run and finalize cleanly:
                # inspecting only directories that happen to exist can miss a deleted job.
                expected_jobs = set(self.canonical) | self.helper_jobs
                self.assertEqual({job.name for job in (directory / 'jobs').iterdir()}, expected_jobs)
                self.assertEqual({job.name for job in (directory / 'started').iterdir()}, expected_jobs)
                for helper in self.helper_jobs:
                    job = directory / 'jobs' / helper
                    self.assertEqual((job / 'done').read_text(), '0\t0\t0\n', helper)
                    self.assertTrue((job / 'cleaned').exists(), helper)
                for job in (directory / 'jobs').iterdir():
                    assigned_slot, observed_cpu = (job / 'fixture-affinity').read_text().split()
                    self.assertEqual(observed_cpu, slot_cpus[int(assigned_slot)], job.name)
                    observed_cpus.add(observed_cpu)
            except (AssertionError, IndexError, ValueError, OSError) as exc:
                # A worker can fail before job_body records its affinity and still publish
                # a recovered completion. Retain its output/ledger before TemporaryDirectory
                # removes the only explanation; a missing witness remains a failed control.
                self.fail(str(exc) + preserve_failure(result.stdout, result.stderr))
            if delayed_completion:
                try:
                    fired = directory / 'delayed-publication'
                    expected_families = {path.name for path in (directory / 'jobs').iterdir()}
                    self.assertEqual({path.name for path in fired.iterdir()}, expected_families)
                    for path in fired.iterdir():
                        events = path.read_text().splitlines()
                        self.assertEqual(len(events), 1, f'{path.name}: completion injection repeated')
                        writer, owner, timestamp = events[0].split('\t')
                        self.assertEqual(writer, owner, f'{path.name}: non-owner published completion')
                        self.assertGreater(float(timestamp), 0)
                except (AssertionError, ValueError, OSError) as exc:
                    self.fail(str(exc) + preserve_failure(result.stdout, result.stderr))
            timed_rows = [line.split('\t') for line in (directory / 'ledger').read_text().splitlines()]
            for row in timed_rows:
                self.assertEqual(len(row), 3)
                self.assertGreaterEqual(float(row[1]), 0)
            counts = tuple(map(int, (directory / 'counts').read_text().split()))
            expected = ((len(self.canonical), 1) if behavior == 'return' else
                        (len(self.canonical) - 1, 1) if behavior in ('empty', 'red', 'crash') or remove_atomic_dependency or add_asan_dependency else
                        (len(self.canonical), 0))
            dependency_reached = (directory / 'dependency-probe-reached').exists()
            dependency_handshake = (directory / 'dependency-handshake').exists()
            dependency_failure = ((directory / 'dependency-failure').read_text()
                                  if (directory / 'dependency-failure').exists() else '')
            # Return the saved path with the verdict diagnostics; callers include this in
            # count assertions so a broken scheduler retains an actionable failure report.
            if counts != expected or (dependency_probe and (not dependency_reached or
                    (bool(dependency_failure) != add_asan_dependency) or
                    (dependency_handshake == add_asan_dependency))):
                output += preserve_failure(result.stdout, result.stderr)
            return dict(ledger=(''.join(f'{v}\t{label}\n' for v, duration, label in timed_rows)).encode(),
                        output=output, counts=counts,
                        fixture_cpus=set(fixture_cpus), observed_cpus=observed_cpus,
                        dependency_reached=dependency_reached,
                        dependency_handshake=dependency_handshake,
                        dependency_failure=dependency_failure,
                        atomic_boot_reached=(directory / 'atomic-boot-reached').exists(),
                        exclusivity_failure=((directory / 'exclusivity-failure').read_text()
                                             if (directory / 'exclusivity-failure').exists() else ''),
                        completion=(directory / 'completion-order').read_text().splitlines(),
                        families=[line.split('\t') for line in (directory / 'families.tsv').read_text().splitlines()
                                  if line.split('\t')[0] not in self.helper_jobs],
                        helpers={path.parent.name for path in (directory / 'jobs').glob('*/done')
                                 if path.parent.name in self.helper_jobs},
                        cleaned={path.parent.name for path in (directory / 'jobs').glob('*/cleaned')
                                 if path.parent.name not in self.helper_jobs})

    def test_failure_evidence_survives_a_named_pipe_in_the_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'build').mkdir()
            directory = root / 'fixture'
            directory.mkdir()
            os.mkfifo(directory / 'pause')
            (directory / 'ledger').write_text('FAIL\t0.001\tfixture\n')
            saved = preserve_scheduler_failure(root, directory, 'actual shell', 'actual stdout', 'actual stderr')
            self.assertTrue(stat.S_ISFIFO((saved / 'pause').stat().st_mode))
            self.assertEqual((saved / 'ledger').read_bytes(), (directory / 'ledger').read_bytes())
            self.assertEqual((saved / 'fixture.sh').read_text(), 'actual shell')
            self.assertEqual((saved / 'command.stdout').read_text(), 'actual stdout')
            self.assertEqual((saved / 'command.stderr').read_text(), 'actual stderr')

    def test_missing_helper_witness_stays_red_and_keeps_its_explanation(self):
        # Reproduce a helper failing before its first observation. Its recovered done
        # marker still releases dependencies, so the ordinary scored rows can all pass;
        # the helper inventory check must fail and preserve the otherwise lost cause.
        with self.assertRaises(AssertionError) as failure:
            self.run_scheduler(slots=3, ordered=False, failure='core_tsan_build',
                               behavior='before-affinity')
        message = str(failure.exception)
        self.assertIn('core_tsan_build', message)
        self.assertIn('Scheduler failure artifacts: ', message)
        saved = Path(message.split('Scheduler failure artifacts: ', 1)[1].strip())
        try:
            helper = saved / 'jobs/core_tsan_build'
            self.assertFalse((helper / 'fixture-affinity').exists())
            self.assertIn('deliberate fixture failure before affinity witness',
                          (helper / 'output.log').read_text())
            self.assertEqual((helper / 'done').read_text(), '17\t0\t1\n')
        finally:
            shutil.rmtree(saved)

    def test_opposite_completion_orders_have_byte_identical_canonical_ledgers(self):
        forward = self.run_scheduler()
        reverse = self.run_scheduler(reverse=True)
        self.assertEqual(forward['completion'],
                         [name for name in self.canonical if name != 'atomic_batteries'] + ['atomic_batteries'])
        self.assertEqual(reverse['completion'],
                         [name for name in self.canonical[::-1] if name != 'atomic_batteries'] + ['atomic_batteries'])
        self.assertEqual(forward['ledger'], reverse['ledger'], forward['output'] + reverse['output'])
        self.assertEqual(forward['observed_cpus'], forward['fixture_cpus'])
        self.assertEqual(reverse['observed_cpus'], reverse['fixture_cpus'])
        self.assertEqual(forward['counts'], (len(self.canonical), 0), forward['output'])
        self.assertEqual({row[0] for row in reverse['families']}, set(self.canonical))
        self.assertEqual(reverse['cleaned'], set(self.canonical))

    def test_empty_fragment_and_explicit_failure_cannot_turn_green(self):
        for behavior in ('empty', 'red'):
            with self.subTest(behavior=behavior):
                result = self.run_scheduler(failure='flipctl', behavior=behavior)
                self.assertEqual(result['counts'], (len(self.canonical) - 1, 1), result['output'])
                self.assertIn(b'FAIL\tflip controller: ramp gate, hold, surge + mix re-maneuvers\n', result['ledger'])

    def test_worker_exit_before_done_is_a_failure(self):
        # Last in completion order, so its deliberate exit cannot block another stub's barrier.
        result = self.run_scheduler(failure=self.canonical[-1], behavior='crash')
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1), result['output'])
        self.assertIn(b'FAIL\tcorrectness family globcase\n', result['ledger'])

    def test_nonzero_worker_return_cannot_be_hidden_by_a_pass_fragment(self):
        result = self.run_scheduler(failure='flipctl', behavior='return')
        # The observed green row remains evidence; the abnormal return adds its own
        # infrastructure failure instead of silently deleting the partial fragment.
        self.assertEqual(result['counts'], (len(self.canonical), 1), result['output'])
        self.assertIn(b'ok\tflip controller: ramp gate, hold, surge + mix re-maneuvers\n', result['ledger'])
        self.assertIn(b'FAIL\tflip controller: ramp gate, hold, surge + mix re-maneuvers\n', result['ledger'])

    def test_completion_is_not_visible_before_its_record_is_written(self):
        # Publication is independent of completion order, which has its own full-inventory
        # forward/reverse controls above. Three workers still open every family's publication
        # window while the collector runs; 97 synchronized shells on one CPU only add contention.
        result = self.run_scheduler(slots=3, ordered=False, delayed_completion=True)
        self.assertEqual(result['counts'], (len(self.canonical), 0), result['output'])

    def test_limited_workers_reuse_slots_without_losing_or_repeating_jobs(self):
        result = self.run_scheduler(slots=3, ordered=False)
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertEqual(result['counts'], (len(self.canonical), 0), result['output'])
        self.assertEqual(len(result['families']), len(self.canonical))
        self.assertLessEqual({row[1] for row in result['families']}, {'0', '1', '2'})

    def test_atomic_boot_waits_for_all_other_families_and_their_cleanup(self):
        result = self.run_scheduler(slots=3, ordered=False)
        self.assertEqual(result['counts'], (len(self.canonical), 0), result['output'])
        self.assertTrue(result['atomic_boot_reached'])
        self.assertEqual(result['exclusivity_failure'], '')
        self.assertEqual(result['completion'][-1], 'atomic_batteries')
        families = {row[0]: row for row in result['families']}
        atomic_start = float(families['atomic_batteries'][2])
        self.assertTrue(all(float(row[3]) <= atomic_start for name, row in families.items()
                            if name != 'atomic_batteries'))

    def test_removing_atomic_exclusivity_fails_before_its_boot(self):
        result = self.run_scheduler(slots=2, ordered=False, remove_atomic_dependency=True)
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1), result['output'])
        self.assertFalse(result['atomic_boot_reached'])
        self.assertIn('atomic boot preceded completion of ', result['exclusivity_failure'])
        self.assertIn(b'FAIL\tcorrectness family atomic_batteries\n', result['ledger'])

    def test_one_slot_uses_the_same_queue_without_losing_coverage(self):
        result = self.run_scheduler(slots=1)
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertEqual(result['counts'], (len(self.canonical), 0), result['output'])
        self.assertEqual({row[1] for row in result['families']}, {'0'})
        self.assertEqual(result['cleaned'], set(self.canonical))

    def test_failed_prerequisite_cannot_leave_the_queue_waiting_forever(self):
        result = self.run_scheduler(slots=2, ordered=False, failure='release', behavior='crash')
        self.assertEqual(result['counts'], (len(self.canonical) - 1, 1), result['output'])
        self.assertIn(b'FAIL\tcorrectness family release\n', result['ledger'])
        self.assertEqual(result['helpers'], {'production_units', 'core_tsan_build', 'waits_tsan_build'})
        self.assertCountEqual(result['completion'], [name for name in self.canonical if name != 'release'])

    def test_release_boots_do_not_wait_for_independent_asan_build(self):
        result = self.run_scheduler(slots=2, ordered=False, dependency_probe=True)
        self.assertEqual(result['counts'], (len(self.canonical), 0), result['output'])
        self.assertEqual(result['helpers'], {'production_units', 'core_tsan_build', 'waits_tsan_build'})
        self.assertCountEqual(result['completion'], self.canonical)
        self.assertTrue(result['dependency_reached'], result['output'])
        self.assertTrue(result['dependency_handshake'], result['output'])
        self.assertEqual(result['dependency_failure'], '', result['output'])
        self.assertLess(result['completion'].index('release_batteries'), result['completion'].index('asan'))
        families = {row[0]: row for row in result['families']}
        self.assertGreaterEqual(float(families['release_batteries'][2]), float(families['release'][3]))

        broken = self.run_scheduler(slots=2, ordered=False, dependency_probe=True,
                                    add_asan_dependency=True)
        self.assertEqual(broken['counts'], (len(self.canonical) - 1, 1), broken['output'])
        self.assertTrue(broken['dependency_reached'], broken['output'])
        self.assertFalse(broken['dependency_handshake'], broken['output'])
        self.assertEqual(broken['dependency_failure'],
                         'release correctness blocked by unfinished ASAN\n', broken['output'])
        self.assertIn(b'FAIL\tcorrectness family asan\n', broken['ledger'])
        self.assertCountEqual(broken['completion'], [name for name in self.canonical if name != 'asan'])


class TSANWiring(unittest.TestCase):
    def run_rows(self, kind='core', failure='', ready=True):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        helpers = gate[gate.index('tsan_unit(){'):gate.index('\njob_production_units(){')]
        first, after = ('job_core_units(){', 'job_reorder_unit(){') if kind == 'core' else ('job_wait_units(){', 'job_readonly(){')
        body = gate[gate.index(first):gate.index(after)]
        stub = r'''set -u
CORE_TSAN=/unused-core-tsan; WAITS_TSAN=/unused-waits-tsan; CORES=0-7
quiet_wait(){ :; }
row_begin(){ :; }
taskset(){ timeout "$@"; }
unit_ready(){ return 0; }
ok(){ printf 'ok\t%s\n' "$1" >> "$RUN_DIR/rows"; }
bad(){ printf 'FAIL\t%s\n' "$1" >> "$RUN_DIR/rows"; }
timeout(){
  local argv="$*" selected=${@: -1}
  case "$argv" in
    *"$CORE_TSAN"*|*"$WAITS_TSAN"*)
      printf 'tsan\t%s\n' "$selected" >> "$RUN_DIR/calls"
      [ "$TSAN_OPTIONS" = halt_on_error=1:exitcode=66 ] || return 89
      [ "$FAILURE" != unavailable ] || return 127
      [ "$FAILURE" != runtime ] || return 66
      [ "$FAILURE" != report ] || echo 'WARNING: ThreadSanitizer: data race'
      [ "$FAILURE" != witness ] || return 0
      if [[ "$argv" == *"$CORE_TSAN"* ]]; then
        printf 'PASS core concurrency %s (state assertions fired)\n' "$selected"
      else printf 'waits unit: PASS\n'; fi;;
    *)
      printf 'control\t%s\n' "$selected" >> "$RUN_DIR/calls"
      [ "$FAILURE" != control ] || return 1;;
  esac
}
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as temporary:
            directory = Path(temporary)
            (directory / 'build').mkdir()
            (directory / 'unit-ready').mkdir()
            if ready:
                for name in ('core-concurrency-tsan', 'waits-unit-tsan'):
                    (directory / 'unit-ready' / name).touch()
            result = subprocess.run(['bash', '-c', stub + helpers + body + '\n' + first.split('(')[0]],
                cwd=directory, env=dict(os.environ, RUN_DIR=temporary, TMPDIR=temporary, FAILURE=failure),
                capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            return [line.split('\t') for line in (directory / 'rows').read_text().splitlines()], \
                   [line.split('\t') for line in (directory / 'calls').read_text().splitlines()]

    def test_core_rows_execute_both_matching_controls(self):
        rows, calls = self.run_rows()
        selections = 'watch scheduler lifetime drain route snapshot config notify'.split()
        self.assertEqual(rows, [['ok', 'core concurrency ' + case] for case in selections])
        self.assertEqual([case for mode, case in calls if mode == 'tsan'], selections)
        self.assertEqual(len([1 for mode, _ in calls if mode == 'control']), 8)

    def test_core_runtime_report_unavailability_and_missing_witness_all_fail(self):
        for failure in ('runtime', 'report', 'unavailable', 'witness', 'control'):
            with self.subTest(failure=failure):
                rows, calls = self.run_rows(failure=failure)
                self.assertEqual([row[0] for row in rows], ['FAIL'] * 8)
                if failure == 'control':
                    self.assertTrue(all(mode == 'control' for mode, _ in calls))
        rows, calls = self.run_rows(ready=False)
        self.assertEqual([row[0] for row in rows], ['FAIL'] * 8)
        self.assertTrue(all(mode == 'control' for mode, _ in calls))

    def test_waits_keeps_existing_rows_and_adds_one_tsan_execution(self):
        rows, calls = self.run_rows(kind='waits')
        self.assertEqual([row[0] for row in rows], ['ok', 'ok'])
        self.assertEqual(len([1 for mode, _ in calls if mode == 'tsan']), 1)
        rows, _ = self.run_rows(kind='waits', failure='unavailable')
        self.assertEqual([row[0] for row in rows], ['FAIL', 'ok'])

    def test_core_build_instruments_every_dependency_and_failed_build_publishes_no_ready_marker(self):
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        bodies = gate[gate.index('job_core_tsan_build(){'):gate.index('tsan_unit(){')]
        makefile = (root / 'Makefile').read_text().replace('\\\n', ' ')
        production = set()
        for line in makefile.splitlines():
            if line.startswith('SRC '):
                production.update(line.split('=', 1)[1].split())
        expected = production - {'src/main.cc', 'src/core/genthread.cc'} | {'tests/core_concurrency_unit.cc'}
        with tempfile.TemporaryDirectory(dir=root / 'build') as temporary:
            directory = Path(temporary)
            stub = r'''set -u
CORE_TSAN="$RUN_DIR/core"; WAITS_TSAN="$RUN_DIR/waits"; BUILD_CORES=0-7
pausable(){ printf '%s\0' "$@" > "$RUN_DIR/argv"; return "$BUILD_RC"; }
'''
            for rc in (0, 1):
                ready = directory / 'unit-ready' / 'core-concurrency-tsan'
                ready.unlink(missing_ok=True)
                result = subprocess.run(['bash', '-c', stub + bodies + '\njob_core_tsan_build'], cwd=root,
                    env=dict(os.environ, RUN_DIR=temporary, TMPDIR=temporary, BUILD_RC=str(rc)),
                    text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, rc)
                self.assertEqual(ready.exists(), rc == 0)
                args = (directory / 'argv').read_bytes().decode().rstrip('\0').split('\0')
                index = args.index('tests/parbuild.sh')
                flags = args[index + 3]
                self.assertIn('-fsanitize=thread', flags)
                self.assertIn('-DTOMO_CORE_CONCURRENCY_TEST', flags)
                self.assertNotIn('-fsanitize=address', flags)
                self.assertEqual(set(args[index + 5:]), expected)
                self.assertFalse(any(arg.endswith('.o') for arg in args))


class CompleteTierDispatch(unittest.TestCase):
    def test_real_coordinator_keeps_full_jobs_and_measures_only_after_join(self):
        import gateplan
        root = Path(__file__).resolve().parent.parent
        gate = (root / 'tests/gate.sh').read_text()
        start = gate[gate.index('start_workers(){'):gate.index('\ncollect_job(){')]
        # Keep the real quick exit, collectors and ABBA background/wait dispatch. Replace only
        # the workload boundaries; a premature measurement, lost full job or wrong argv fails.
        coordinator = gate[gate.index('\nstart_workers\n'):
                           gate.index('\ncase "$ABBA_RC" in')]
        ledger = next(line for line in gate.splitlines() if line.startswith('LEDGER=${GATE_LEDGER:'))
        # Exercise the reviewed 32-thread ABBA geometry. The planner correctly refuses an
        # unreviewed 8-thread measurement before reaching this coordinator boundary.
        topology = {cpu: frozenset((cpu, cpu + 1000)) for cpu in range(64)}
        topology.update({cpu + 1000: group for cpu, group in list(topology.items())})
        full_only = {'asan_batteries', 'replyoff', 'zc', 'rldbg', 'rlcache',
                     'differ-split', 'differ-armed', 'globcase'}
        stub = r'''
GATE_SLOTS=0; PASS=0; FAIL=0; EXPECT_QUICK=419; GATE_STARTED=$SECONDS; JOINED=0
LEDGER="$RUN_DIR/ledger"; TIMINGS="$RUN_DIR/timings"; : > "$LEDGER"; : > "$TIMINGS"
phase(){ printf 'PHASE %s\n' "$1" >> "$EVENTS"; }
program_state(){ :; }
quiet_wait(){ :; }
row_begin(){ ROW_WATCHDOG=$BASHPID; }
collect_job(){
  local required=("$1") child
  case "$1" in
    differ-split) required=(differ-split-0 differ-split-1 differ-equivalence);;
    differ-armed) required=(differ-armed-0 differ-armed-1);;
  esac
  for child in "${required[@]}"; do
    case " ${JOB_NAMES[*]} " in *" $child "*) ;; *) echo "unreached job $child" >&2; exit 71;; esac
  done
  printf 'COLLECT %s\n' "$1" >> "$EVENTS"
}
join_workers(){ JOINED=1; printf 'JOIN\n' >> "$EVENTS"; }
python3(){
  # This new serverless output resolver is not an ABBA measurement. Let the actual parser
  # resolve its destination before recording the one real background dispatch below.
  if [ "$1" = - ]; then command "$WIRE_PYTHON" "$@"; return; fi
  if [ "$1" = tests/gate_history.py ]; then printf "fixture-context\n"; return 0; fi
  if [ "$1" = tests/differ_fanout.py ]; then return 0; fi
  [ "$1" = tests/abbagate.py ] || return 74
  [ "$JOINED" = 1 ] || { echo 'measurement before worker join' >&2; return 72; }
  case " ${JOB_NAMES[*]} " in *' abba '*|*' perf '*) return 73;; esac
  printf 'ABBA\n' >> "$EVENTS"
  printf '%s\0' "$@" > "$RUN_DIR/argv"
  printf '%s\n' "$GATE_QUIET_WATCHDOG" > "$RUN_DIR/watchdog"
}
'''
        with tempfile.TemporaryDirectory(dir=root / 'build') as temporary:
            for purpose in ('iteration', 'push', 'release', 'full', 'quick'):
                with self.subTest(purpose=purpose):
                    with patch.dict(os.environ, {}, clear=True):
                        args = gateplan.parser().parse_args([purpose, '--server-cores', '0-31',
                                                            '--load-cores', '32-63'])
                    plan = gateplan.make_plan(args, topology=topology, available=set(topology),
                                              check_available=False)
                    directory = Path(temporary) / purpose
                    directory.mkdir()
                    env = dict(os.environ, RUN_DIR=str(directory), EVENTS=str(directory / 'events'),
                               WIRE_PYTHON=sys.executable)
                    env.pop('GATE_LEDGER', None)
                    env.pop('GATE_QUIET_WATCHDOG', None)
                    script = gateplan.shell_plan(plan) + ledger + '\nprintf "%s\\n" "$LEDGER"\n'
                    result = subprocess.run(['bash', '-uc', script + stub + start + coordinator],
                        cwd=root, env=env, text=True, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout.splitlines()[0], str(root / f'build/gate-ledger-{purpose}.txt'))
                    events = (directory / 'events').read_text().splitlines()
                    collected = {event.removeprefix('COLLECT ') for event in events if event.startswith('COLLECT ')}
                    if purpose == 'quick':
                        self.assertFalse(collected & full_only)
                        self.assertNotIn('ABBA', events)
                    else:
                        self.assertTrue(full_only <= collected)
                        self.assertEqual(events.count('ABBA'), 1)
                        self.assertLess(events.index('JOIN'), events.index('ABBA'))
                        self.assertTrue(all(index < events.index('JOIN') for index, event in enumerate(events)
                                            if event.startswith('COLLECT ')))
                        argv = (directory / 'argv').read_bytes().decode().rstrip('\0').split('\0')
                        self.assertEqual(argv[0], 'tests/abbagate.py')
                        self.assertEqual(argv[argv.index('--subset') + 1],
                                         'smoke' if purpose == 'iteration' else 'full')
                        self.assertEqual(argv[-2:], ['--output', str(directory / 'abba')])
                        self.assertRegex((directory / 'watchdog').read_text().strip(), r'^[1-9][0-9]*:[1-9][0-9]*$')
                        if purpose == 'iteration':
                            # Delete only the production measurement barrier. The same
                            # workload-boundary assertion must refuse ABBA before it emits
                            # any measurement evidence; a control that never reaches this
                            # assertion would otherwise pass on the broken coordinator.
                            barrier = '\njoin_workers\nphase abba-begin'
                            self.assertEqual(coordinator.count(barrier), 1)
                            poisoned = coordinator.replace(barrier, '\nphase abba-begin', 1)
                            (directory / 'events').unlink()
                            (directory / 'argv').unlink()
                            result = subprocess.run(
                                ['bash', '-uc', script + stub + start + poisoned + '\nexit "$ABBA_RC"\n'],
                                cwd=root, env=env, text=True, capture_output=True, timeout=5)
                            self.assertEqual(result.returncode, 72, result.stdout + result.stderr)
                            self.assertIn('measurement before worker join', result.stderr)
                            self.assertNotIn('ABBA', (directory / 'events').read_text().splitlines())
                            self.assertFalse((directory / 'argv').exists())


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
