#!/usr/bin/env python3
"""Serverless negative controls driving the real differential shell selection and loop."""
import copy
import contextlib
import io
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import differ_fanout as fanout
from _differ_history import write_json

ROOT = Path(__file__).resolve().parents[1]


class DifferentialTrackingReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Load the production connector/parser without opening any differential servers.
        spec = importlib.util.spec_from_file_location('differ_tracking_controls', ROOT / 'tests/differ.py')
        cls.differ = importlib.util.module_from_spec(spec)
        with patch.object(sys, 'argv', ['differ.py', '--list-generators']), contextlib.redirect_stdout(io.StringIO()):
            try:
                spec.loader.exec_module(cls.differ)
            except SystemExit as stopped:
                if stopped.code != 0:
                    raise

    def push_after_reply(self):
        self.differ.climon_reader_control()

    def test_push_after_reply_stays_visible_to_round_collector(self):
        self.push_after_reply()

    def test_buffered_reader_mutation_hides_push_and_is_rejected(self):
        original = self.differ.conn_mode
        def buffered(*args, **kwargs):
            return original(*args, **dict(kwargs, buffering=-1))
        with patch.object(self.differ, 'conn_mode', buffered):
            with self.assertRaisesRegex(AssertionError, 'push hidden in reply buffer'):
                self.push_after_reply()

    def test_real_climon_suite_requires_reader_control_before_server_commands(self):
        with patch.object(self.differ, 'climon_reader_control',
                          side_effect=AssertionError('reader control mutant')):
            with self.assertRaisesRegex(AssertionError, 'reader control mutant'):
                self.differ.run_climon_suite(None)

    def test_unbuffered_short_bulk_reads_are_assembled_and_truncation_fails(self):
        class Fragmented(io.BytesIO):
            def read(self, count=-1):
                return super().read(min(count, 2))
        wire = b'>2\r\n$10\r\ninvalidate\r\n*1\r\n$4\r\nck:2\r\n'
        self.assertEqual(self.differ.read_reply(Fragmented(wire)), wire)
        with self.assertRaises(EOFError):
            self.differ.read_reply(Fragmented(wire[:-1]))


def fixture_plan():
    return dict(schema=1, manifest=dict(run='fixture', permanent=[7, 19, 20], failed=[],
                rotating=21, seeds=[7, 19, 20, 21]), suites=['string', 'multi', 'sort'],
                repeats=4, equivalence_seeds=[21, 20])


class DifferentialFanout(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.plan = fixture_plan()

    def run_part(self, part, poison=''):
        directory = self.directory / part
        directory.mkdir()
        out = directory / 'output'
        plan = directory / 'plan.json'; write_json(plan, self.plan)
        driver = directory / 'driver.py'
        driver.write_text('''import json, os, pathlib, sys
# The fake command writes its explicit comparison artifact. Importing the real helpers with
# this variable armed would register their empty atexit reporter and overwrite that evidence.
coverage_path=os.environ.pop('GATE_DIFFER_COVERAGE',None)
sys.path.insert(0, os.environ['REAL_TESTS'])
from mode_equivalence import CELLS, command_stream
args=sys.argv[1:]; plan=json.load(open(os.environ['FIXTURE_PLAN']))
with open(os.environ['CALLS'],'a') as f: f.write(json.dumps(args)+'\\n')
if args[0]=='tests/differ.py':
    if args[1:]==['--list-generators']:
        print('\\n'.join(plan['suites']))
    else:
        suite=args[5]; seed=int(args[6])
        pathlib.Path(coverage_path).write_text(json.dumps({'schema':1,'commands':{'GET':1}}))
        print('DIFFER '+suite+': compared commands')
        sys.exit(7 if suite==os.environ.get('POISON') else 0)
elif args[0]=='tests/differ_fanout.py':
    if args[1]=='select':
        print(' '.join(map(str,plan['manifest']['seeds'])))
        print(' '.join(map(str,plan['equivalence_seeds'])))
        print(plan['repeats'])
    elif args[1]=='finish':
        pathlib.Path(os.environ['FINISH']).write_text(json.dumps({'failures':int(args[args.index('--failures')+1]), 'passed':int(args[args.index('--passed')+1])}))
elif args[0]=='tests/_differ_history.py':
    if args[1]=='allocate':
        pathlib.Path(args[args.index('--output')+1]).write_text(json.dumps(plan['manifest']))
        print(' '.join(map(str,plan['manifest']['seeds'])))
    elif args[1] not in ('record','summary'): raise AssertionError(args)
elif args[0]=='tests/mode_equivalence.py':
    seed=int(args[args.index('--seed')+1]); folder=pathlib.Path(args[args.index('--output')+1]); folder.mkdir()
    count=len(command_stream(seed))
    folder.joinpath('stream.json').write_text(json.dumps({'seed':seed,'cells':CELLS,'operation_count':count}))
    folder.joinpath('results.json').write_text(json.dumps([{'cell':cell,'verdict':'ok','reached':True,'compared_replies':count,'witnesses':{'observed':1},'reply_sha256':'fixture'} for cell in CELLS]))
else: raise AssertionError(args)
''')
        cli = directory / 'oracle-cli'
        cli.write_text('#!/bin/sh\nprintf "redis_version:7.4.2\\nread_local_hits:1\\nread_local_fallbacks:0\\n"\n')
        cli.chmod(0o755)
        source = (ROOT / 'tests/differ_gate.sh').read_text()
        marker = source.index('DISCOVERED_SUITES=')
        # Only workload boundaries are replaced. The production part selection, geometry check,
        # atomic/seed/suite loops, repeats, exit handling and completion invocation all run.
        stubs = '''
python3(){ command "$REAL_PYTHON" "$FIXTURE_DRIVER" "$@"; }
taskset(){ shift 2; if [ "$1" = timeout ]; then shift 2; fi; "$@"; }
boot_owned(){ BOOT_PID=100; printf '%s\\n' "$*" >> "$BOOTS"; }
stop_owned(){ printf '%s\\n' "$*" >> "$STOPS"; }
cleanup(){ :; }
quiet_stop(){ :; }
'''
        env = dict(os.environ, GATE_DIFFER_PART=part, GATE_DIFFER_PLAN=str(plan),
                   GATE_DIFFER_OUT=str(out), GATE_DIFFER_GEOMETRY='armed-fused' if part.startswith('armed') else 'split',
                   GATE_DIFFER_ORACLE_BIN='/bin/true', GATE_DIFFER_REDIS_CLI=str(cli),
                   REAL_PYTHON=sys.executable, FIXTURE_DRIVER=str(driver), FIXTURE_PLAN=str(plan),
                   REAL_TESTS=str(ROOT / 'tests'), CALLS=str(directory / 'calls.jsonl'),
                   FINISH=str(directory / 'finish.json'), BOOTS=str(directory / 'boots'),
                   STOPS=str(directory / 'stops'), POISON=poison)
        env.pop('GATE_LOAD_CORES', None)
        script = source[:marker] + stubs + source[marker:]
        # The real script normally derives ROOT from its own filename. A -c fixture supplies the
        # same source path as $0, so its initial cd still resolves the repository correctly.
        result = subprocess.run(['bash', '-c', script, str(ROOT / 'tests/differ_gate.sh'), '/bin/true'],
                                cwd=ROOT, env=env, text=True, capture_output=True, timeout=30)
        if result.returncode != int(bool(poison)):
            saved = Path(tempfile.mkdtemp(prefix='differ-fanout-failure-', dir=ROOT / 'build'))
            shutil.copytree(directory, saved, dirs_exist_ok=True)
            (saved / 'fixture.sh').write_text(script)
            (saved / 'stdout').write_text(result.stdout)
            (saved / 'stderr').write_text(result.stderr)
            self.fail(f'Unexpected fixture exit {result.returncode}; retained at {saved}\n' +
                      result.stdout + result.stderr)
        completed = json.loads((directory / 'finish.json').read_text())
        return directory, out, completed

    def test_actual_atomic_lifetimes_keep_every_seed_suite_and_same_target_repeat(self):
        for part in fanout.PARTS[:4]:
            with self.subTest(part=part):
                directory, out, completed = self.run_part(part)
                fanout.finish(self.plan, part, out, **completed)
                rows = fanout.read_journal(out)
                self.assertEqual([row[:4] for row in rows], fanout.expected_legs(self.plan, part))
                boots = (directory / 'boots').read_text().splitlines()
                stops = (directory / 'stops').read_text().splitlines()
                self.assertEqual(len(boots), 2)  # exactly one oracle and one target, across all seeds
                self.assertEqual(len(stops), 2)
                self.assertEqual('--set-max-intset-entries 128' in boots[0], part.endswith('-1'))

    def test_actual_equivalence_loop_runs_every_frozen_failure_stream(self):
        directory, out, completed = self.run_part('equivalence')
        fanout.finish(self.plan, 'equivalence', out, **completed)
        self.assertEqual(fanout.check_equivalence(self.plan, out), 64)
        self.assertFalse((directory / 'boots').exists(), 'equivalence part booted an unused oracle')
        rows = json.loads((out / 'mode-equivalence-20/results.json').read_text())
        (out / 'mode-equivalence-20/results.json').write_text(json.dumps(rows[:-1]))
        with self.assertRaisesRegex(ValueError, 'missing or reordered'):
            fanout.check_equivalence(self.plan, out)

    def test_actual_failed_leg_stays_red_and_later_legs_still_run(self):
        _directory, out, completed = self.run_part('split-1', 'multi')
        self.assertEqual(len(fanout.read_journal(out)), len(fanout.expected_legs(self.plan, 'split-1')))
        self.assertGreater(completed['failures'], 0)
        with self.assertRaisesRegex(ValueError, 'failed leg'):
            fanout.finish(self.plan, 'split-1', out, **completed)
        self.assertFalse((out / 'complete.json').exists())

    def test_deleted_repeated_reordered_unreached_and_unwitnessed_legs_fail(self):
        _directory, out, completed = self.run_part('split-1')
        path = out / 'legs.tsv'; original = path.read_text(); rows = original.splitlines()
        for poison in (rows[:-1], rows + rows[-1:], [*rows[1:2], rows[0], *rows[2:]]):
            path.write_text('\n'.join(poison) + '\n')
            with self.assertRaisesRegex(ValueError, 'executed legs'):
                fanout.check_matrix(self.plan, 'split-1', out)
        path.write_text(original)
        witness = out / 'string-a1-s7.txt.coverage.json'
        witness.write_text(json.dumps({'schema':1, 'commands':{}}))
        with self.assertRaisesRegex(ValueError, 'comparison witness'):
            fanout.check_matrix(self.plan, 'split-1', out)
        witness.write_text(json.dumps({'schema':1, 'commands':{'GET':1}}))
        with self.assertRaisesRegex(ValueError, 'missing comparison or armed-lane'):
            fanout.finish(self.plan, 'split-1', out, failures=0, passed=completed['passed']-1)

    def test_outer_fold_requires_every_successful_clean_child_and_uses_wall_span(self):
        jobs = self.directory / 'jobs'; jobs.mkdir()
        for index, part in enumerate(fanout.PARTS):
            _directory, out, completed = self.run_part(part)
            fanout.finish(self.plan, part, out, **completed)
            job = jobs / ('differ-' + part); job.mkdir()
            shutil.copytree(out, job / 'differ')
            (job / 'done').write_text('0\t1\t0\n')
            (job / 'ledger').write_text(f'ok\t10\tprivate {part}\n')
            (job / 'family.tsv').write_text(f'differ-{part}\t{index}\t{100+index}\t{110+index}\n')
        # split0/1 and equivalence overlap: use [100,114], not the sum of their30s durations.
        self.assertEqual(fanout.fold(self.plan, 'split', self.directory)['seconds'], 14)
        self.assertEqual(fanout.fold(self.plan, 'armed', self.directory)['seconds'], 11)
        for part in fanout.PARTS:
            group = 'armed' if part.startswith('armed') else 'split'
            job = jobs / ('differ-' + part)
            done = job / 'done'; original = done.read_text()
            for poison in ('', '17\t1\t0\n', '0\t0\t0\n', '0\t1\t1\n'):
                done.write_text(poison)
                with self.subTest(part=part, poison=poison), self.assertRaisesRegex(ValueError, 'worker completion'):
                    fanout.fold(self.plan, group, self.directory)
            done.write_text(original)
            result_path = job / 'differ/complete.json'
            result = json.loads(result_path.read_text()); stale = dict(result, run='earlier-run')
            write_json(result_path, stale)
            with self.assertRaisesRegex(ValueError, 'stale/incomplete'):
                fanout.fold(self.plan, group, self.directory)
            write_json(result_path, result)
        (jobs / 'differ-armed-1/done').unlink()
        with self.assertRaises(FileNotFoundError):
            fanout.fold(self.plan, 'armed', self.directory)


class FailedFoldTiming(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.plan = fixture_plan()
        write_json(self.directory / 'plan.json', self.plan)
        write_json(self.directory / 'timeouts.json',
                   dict(schema=1, rows={}, defaults=dict(fallback_seconds=900)))
        for index, part in enumerate(fanout.PARTS):
            job = self.directory / 'jobs' / ('differ-' + part)
            job.mkdir(parents=True)
            (job / 'done').write_text('0\t0\t1\n')
            (job / 'ledger').write_text(f'FAIL\t10\tprivate {part}\n')
            (job / 'family.tsv').write_text(f'differ-{part}\t{index}\t{100+index}\t{110+index}\n')

    def publish(self, group='split', **overrides):
        argv = ['differ_fanout.py', 'fold', '--plan', str(self.directory / 'plan.json'),
                '--group', group, '--run-directory', str(self.directory),
                '--row-plan', str(self.directory / 'timeouts.json'),
                '--row-history', str(self.directory / 'history'), '--row-run', 'fixture']
        output, errors = io.StringIO(), io.StringIO()
        loader_options = overrides or {'return_value': self.plan}
        with patch.object(sys, 'argv', argv), patch.object(fanout, 'load_plan', **loader_options), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            rc = fanout.main()
        return rc, output.getvalue(), errors.getvalue()

    def observations(self):
        path = self.directory / 'history/row-observations.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_completed_failure_keeps_each_public_duration_and_history(self):
        for group, seconds in (('split', 14), ('armed', 11)):
            with self.subTest(group=group):
                with self.assertRaisesRegex(ValueError, 'worker completion'):
                    fanout.fold(self.plan, group, self.directory)
                rc, output, errors = self.publish(group)
                self.assertEqual(rc, 1)
                self.assertEqual(output, f'FAIL\t{seconds:.6f}\t{fanout.LABELS[group]}\n')
                self.assertIn('worker completion', errors)
                record = self.observations()[-1]
                self.assertEqual((record['verdict'], record['seconds'], record['ledger_label']),
                                 ('FAIL', seconds, fanout.LABELS[group]))
                self.assertTrue(record['scored'])
                self.assertFalse(record['timed_out'])

    def test_inventory_failure_retains_original_run_timing_without_authorizing_success(self):
        rc, output, errors = self.publish(side_effect=ValueError('permanent seed source changed'))
        self.assertEqual(rc, 1)
        self.assertTrue(output.startswith('FAIL\t14.000000\t'))
        self.assertIn('permanent seed source changed', errors)
        self.assertEqual(self.observations()[0]['verdict'], 'FAIL')

    def test_missing_or_malformed_completion_timing_cannot_manufacture_a_row(self):
        job = self.directory / 'jobs/differ-split-0'
        for filename, poison in (('done', ''), ('done', '0\t0\t1\n0\t0\t1\n'),
                                 ('family.tsv', ''), ('family.tsv', 'differ-armed-0\t0\t100\t110\n'),
                                 ('family.tsv', 'differ-split-0\t0\tNaN\t110\n'),
                                 ('family.tsv', 'differ-split-0\t0\t110\t100\n')):
            path = job / filename; original = path.read_text()
            with self.subTest(filename=filename, poison=poison):
                path.write_text(poison)
                rc, output, _errors = self.publish()
                self.assertEqual((rc, output), (1, ''))
                self.assertEqual(self.observations(), [])
                path.write_text(original)
        (job / 'family.tsv').unlink()
        self.assertEqual(self.publish()[:2], (1, ''))
        self.assertEqual(self.observations(), [])

    def test_another_run_cannot_donate_timing_or_history(self):
        changed = copy.deepcopy(self.plan)
        changed['manifest']['run'] = 'another-run'
        write_json(self.directory / 'plan.json', changed)
        self.assertEqual(self.publish()[:2], (1, ''))
        self.assertEqual(self.observations(), [])

    def test_success_and_timeout_history_match_emitted_six_decimal_duration(self):
        for seconds, expected in ((14.123456789, 'ok'), (901.123456789, 'FAIL')):
            with self.subTest(seconds=seconds), patch.object(fanout, 'fold',
                    return_value=dict(schema=1, seconds=seconds, complete=True)):
                rc, output, _errors = self.publish()
                verdict, duration, label = output.strip().split('\t')
                self.assertEqual((rc, verdict, label), (int(expected == 'FAIL'), expected, fanout.LABELS['split']))
                self.assertEqual(float(duration), self.observations()[-1]['seconds'])
                self.assertEqual(self.observations()[-1]['timed_out'], expected == 'FAIL')

    def test_real_collector_accepts_only_exact_failed_rows_after_helper_error(self):
        gate = (ROOT / 'tests/gate.sh').read_text()
        collector = gate[gate.index('collect_differ_group(){'):gate.index('\ncollect_job(){')]
        label = fanout.LABELS['split']
        cases = [(1, f'FAIL\t14.000000\t{label}\n', True),
                 (0, f'ok\t14.000000\t{label}\n', True),
                 (1, f'ok\t14.000000\t{label}\n', False), (1, '', False),
                 (1, f'FAIL\tNaN\t{label}\n', False), (1, 'FAIL\t14\twrong label\n', False),
                 (1, f'FAIL\t14\t{label}\nFAIL\t14\t{label}\n', False),
                 (1, f'FAIL\t14\t{label}\textra\n', False)]
        stub = r'''
set -u
PASS=0; FAIL=0; WORKER_PIDS=(); ROW_PLAN=unused; ROW_HISTORY=unused; ROW_RUN_ID=fixture
LEDGER="$RUN_DIR/ledger"; TIMINGS="$RUN_DIR/timings"; : > "$LEDGER"; : > "$TIMINGS"
job_label(){ printf '%s\n' "$EXPECTED_LABEL"; }
python3(){ printf '%s' "$FOLD_OUTPUT"; return "$FOLD_RC"; }
bad(){ printf 'FAIL\t0\t%s\n' "$1" >> "$LEDGER"; FAIL=$((FAIL+1)); }
say(){ :; }
'''
        for rc, output, accepted in cases:
            with self.subTest(rc=rc, output=output):
                result = subprocess.run(['bash', '-c', stub + collector + '\ncollect_differ_group split\n'],
                    cwd=ROOT, env=dict(os.environ, RUN_DIR=str(self.directory), EXPECTED_LABEL=label,
                                      FOLD_RC=str(rc), FOLD_OUTPUT=output), capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.directory / 'ledger').read_text(), output if accepted else f'FAIL\t0\t{label}\n')


if __name__ == '__main__':
    unittest.main()
