#!/usr/bin/env python3
"""Frozen differential inventory and strict folding of independently scheduled owner lifetimes.

The unit of scheduling is a target's entire atomic-mode lifetime. Its ordered seeds, suites and
seed-19 MULTI repeats stay together; splitting seeds would erase the accumulated allocator and
concurrency state that those repeats deliberately revisit. Only the two historical outer rows
enter the public ledger. Private child rows retain their own deadlines and unscored history.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys

from _differ_history import allocate, default_history, failing_seeds, permanent_seeds, write_json

ROOT = Path(__file__).resolve().parents[1]
PARTS = ('split-0', 'split-1', 'armed-0', 'armed-1', 'equivalence')
LABELS = {'split': 'Redis 7.4 differential matrix',
          'armed': 'Redis 7.4 differential matrix (armed fused + read-local)'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def suites():
    names = subprocess.check_output([sys.executable, str(ROOT / 'tests/differ.py'),
                                     '--list-generators'], text=True).splitlines()
    require(names and len(names) == len(set(names)), 'empty or duplicate suite inventory')
    return names


def load_plan(path):
    plan = json.loads(Path(path).read_text())
    require(plan.get('schema') == 1, 'unknown differential plan schema')
    seed = plan['manifest']
    require(seed['permanent'] == permanent_seeds(), 'permanent seed source changed during run')
    require(seed['seeds'] == list(dict.fromkeys([*seed['permanent'], *seed['failed'], seed['rotating']])),
            'frozen seed inventory is incomplete or reordered')
    require(all(type(x) is int and x >= 0 for x in seed['seeds']), 'invalid frozen seed')
    require(plan['suites'] == suites(), 'suite source changed during run')
    require(type(plan['repeats']) is int and plan['repeats'] >= 4, 'lost historical MULTI repeats')
    require(plan['equivalence_seeds'] and len(set(plan['equivalence_seeds'])) == len(plan['equivalence_seeds']),
            'invalid equivalence seed inventory')
    return plan


def expected_legs(plan, part):
    require(part in PARTS, 'unknown differential part')
    if part == 'equivalence':
        return []
    mode, atomic = part.split('-')
    names = [name for name in plan['suites'] if mode != 'armed' or name != 'sort']
    rows = [(suite, int(atomic), seed, 0) for seed in plan['manifest']['seeds'] for suite in names]
    if atomic == '1':
        rows += [('multi', 1, 19, rep) for rep in range(1, plan['repeats'] + 1)]
    return rows


def make_plan(path, run, repeats):
    require(not Path(path).exists(), 'refusing to overwrite frozen differential plan')
    require(repeats >= 4, 'the four historical MULTI repeats are mandatory')
    history = default_history()
    manifest = allocate(history, run)
    plan = dict(schema=1, manifest=manifest, suites=suites(), repeats=repeats,
                equivalence_seeds=list(dict.fromkeys([manifest['rotating'],
                    *failing_seeds(history, 'mode-equivalence')])))
    write_json(Path(path), plan)
    return plan


def read_journal(directory):
    path = directory / 'legs.tsv'
    require(path.exists(), 'missing executed-leg journal')
    rows = []
    for line in path.read_text().splitlines():
        fields = line.split('\t')
        require(len(fields) == 6, 'malformed executed-leg journal')
        suite, atomic, seed, repeat, rc, filename = fields
        rows.append((suite, int(atomic), int(seed), int(repeat), int(rc), filename))
    return rows


def check_matrix(plan, part, directory):
    rows = read_journal(directory)
    expected = expected_legs(plan, part)
    require([row[:4] for row in rows] == expected,
            f'{part}: missing, duplicated, reordered or unexpected executed legs')
    for suite, atomic, seed, repeat, rc, filename in rows:
        stem = f'{suite}-a{atomic}-s{seed}' + (f'-rep{repeat}' if repeat else '') + '.txt'
        require(filename == stem and rc == 0, f'{part}: failed leg {stem} (exit {rc})')
        log = directory / stem
        require(log.is_file() and log.stat().st_size, f'{part}: missing leg log {stem}')
        artifact = json.loads((directory / (stem + '.coverage.json')).read_text())
        counts = artifact.get('commands', {})
        require(artifact.get('schema') == 1 and counts and
                all(type(count) is int and count > 0 for count in counts.values()),
                f'{part}: missing executed comparison witness for {stem}')
    return len(rows)


def check_equivalence(plan, directory):
    # The actual runner must return every named cell, reached and compared. An exit0 summary is
    # insufficient: a deleted loop iteration or stale successful prefix cannot satisfy this fold.
    from mode_equivalence import CELLS, command_stream
    count = 0
    for seed in plan['equivalence_seeds']:
        folder = directory / f'mode-equivalence-{seed}'
        manifest = json.loads((folder / 'stream.json').read_text())
        rows = json.loads((folder / 'results.json').read_text())
        expected_replies = len(command_stream(seed))
        require(manifest['seed'] == seed and manifest['cells'] == list(CELLS) and
                manifest['operation_count'] == expected_replies, 'equivalence stream inventory mismatch')
        require([row['cell'] for row in rows] == list(CELLS), 'missing or reordered equivalence cell')
        for row in rows:
            require(row.get('verdict') == 'ok' and row.get('reached') is True and
                    row.get('compared_replies') == expected_replies and row.get('witnesses') and
                    row.get('reply_sha256'), f'equivalence cell did not pass: {row.get("cell")}')
        count += len(rows)
    return count


def finish(plan, part, directory, failures, passed):
    require(not (directory / 'complete.json').exists(), 'duplicate child completion publication')
    count = check_equivalence(plan, directory) if part == 'equivalence' else check_matrix(plan, part, directory)
    require(failures == 0, f'{part}: boot, identity, lane, comparison or shutdown failure')
    expected_passes = (len(plan['equivalence_seeds']) if part == 'equivalence' else
                       count + int(part.startswith('armed-')))
    require(passed == expected_passes, f'{part}: missing comparison or armed-lane verdict')
    # Written only after the shell reaped its listeners and completed their unchanged assertions.
    write_json(directory / 'complete.json', dict(schema=1, part=part, run=plan['manifest']['run'],
               seeds=plan['manifest']['seeds'], comparisons=count, complete=True))


def fold(plan, group, run_directory):
    selected = [f'{group}-0', f'{group}-1'] + (['equivalence'] if group == 'split' else [])
    starts, ends, total = [], [], 0
    for part in selected:
        directory = run_directory / 'jobs' / ('differ-' + part)
        done = directory.joinpath('done').read_text().split()
        require(done == ['0', '1', '0'], f'{part}: missing or unsuccessful worker completion {done}')
        rows = directory.joinpath('ledger').read_text().splitlines()
        require(len(rows) == 1 and rows[0].split('\t')[0] == 'ok', f'{part}: private row did not pass')
        family = directory.joinpath('family.tsv').read_text().strip().split('\t')
        require(len(family) == 4 and family[0] == 'differ-' + part, f'{part}: wrong family identity')
        start, end = map(float, family[2:])
        require(math.isfinite(start) and math.isfinite(end) and 0 < start <= end,
                f'{part}: invalid start/finish duration')
        starts.append(start); ends.append(end)
        result = json.loads((directory / 'differ/complete.json').read_text())
        require(result.get('schema') == 1 and result.get('complete') is True and
                result.get('part') == part and result.get('run') == plan['manifest']['run'] and
                result.get('seeds') == plan['manifest']['seeds'], f'{part}: stale/incomplete result')
        count = (check_equivalence(plan, directory / 'differ') if part == 'equivalence' else
                 check_matrix(plan, part, directory / 'differ'))
        require(result['comparisons'] == count, f'{part}: completion count mismatch')
        total += count
    return dict(schema=1, group=group, parts=selected, start=min(starts), end=max(ends),
                seconds=max(ends)-min(starts), comparisons=total, complete=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    q = sub.add_parser('plan'); q.add_argument('--output', type=Path, required=True)
    q.add_argument('--run', required=True); q.add_argument('--repeats', type=int, default=4)
    q = sub.add_parser('select'); q.add_argument('--plan', type=Path, required=True)
    q.add_argument('--part', choices=PARTS, required=True)
    q = sub.add_parser('finish'); q.add_argument('--plan', type=Path, required=True)
    q.add_argument('--part', choices=PARTS, required=True); q.add_argument('--directory', type=Path, required=True)
    q.add_argument('--failures', type=int, required=True)
    q.add_argument('--passed', type=int, required=True)
    q = sub.add_parser('fold'); q.add_argument('--plan', type=Path, required=True)
    q.add_argument('--group', choices=LABELS, required=True); q.add_argument('--run-directory', type=Path, required=True)
    q.add_argument('--row-plan', type=Path, required=True)
    q.add_argument('--row-history', type=Path, required=True); q.add_argument('--row-run', required=True)
    args = p.parse_args()
    try:
        if args.action == 'plan':
            make_plan(args.output, args.run, args.repeats)
        else:
            plan = load_plan(args.plan)
            if args.action == 'select':
                print(' '.join(map(str, plan['manifest']['seeds'])))
                print(' '.join(map(str, plan['equivalence_seeds'])))
                print(plan['repeats'])
            elif args.action == 'finish':
                finish(plan, args.part, args.directory, args.failures, args.passed)
            else:
                require(args.row_run == plan['manifest']['run'], 'fold run identity differs from frozen plan')
                result = fold(plan, args.group, args.run_directory)
                write_json(args.run_directory / ('differ-' + args.group + '-fold.json'), result)
                from gate_history import budget, record
                label = LABELS[args.group]
                policy = budget(json.loads(args.row_plan.read_text()), label)
                expired = result['seconds'] >= policy['timeout_seconds']
                verdict = 'FAIL' if expired else 'ok'
                # Child watchdogs bound the work while it runs. This already-finished public row
                # also retains its historical whole-row budget, without charging collection delay
                # or pretending that durations of simultaneous children should be added together.
                if expired:
                    print(f'DIFFER TIMEOUT: {label}: {policy["timeout_seconds"]}s; '
                          f'median={policy["median_seconds"]}; {policy["basis"]}', file=sys.stderr)
                record(args.row_history, run_id=args.row_run, label=label, seconds=result['seconds'],
                       verdict=verdict, timed_out=expired, ledger_label=label)
                print(f'{verdict}\t{result["seconds"]:.6f}\t{label}')
        return 0
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f'DIFFER FANOUT FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
