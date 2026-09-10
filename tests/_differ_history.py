#!/usr/bin/env python3
"""Durable rotating seeds and executed-comparison coverage for the differential matrix.

State is outside build/: make clean must not erase a newly discovered counterexample. Each
worktree owns its state directory. Failures are automatically a permanent replay corpus;
copying a seed into tests/differ_seeds.txt also carries it to other machines. Nothing removes a
failed seed, and the running gate never edits tracked source. flock serializes parallel matrices.
"""
import argparse
import atexit
from collections import Counter
import contextlib
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def utc():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as file:
            json.dump(value, file, indent=2)
            file.write('\n')
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def default_history():
    return Path(os.environ.get('GATE_DIFFER_HISTORY', str(ROOT / '.gate-history/differ')))


@contextlib.contextmanager
def locked(history):
    history.mkdir(parents=True, exist_ok=True)
    with (history / 'lock').open('a') as file:
        fcntl.flock(file, fcntl.LOCK_EX)
        yield


def permanent_seeds():
    seeds = []
    for line in (ROOT / 'tests/differ_seeds.txt').read_text().splitlines():
        token = line.partition('#')[0].strip()
        if token:
            seed = int(token)
            if seed < 0 or seed in seeds:
                raise ValueError('invalid/duplicate permanent differential seed')
            seeds.append(seed)
    if not {7, 19} <= set(seeds):
        raise ValueError('historical seeds 7 and 19 must remain permanent')
    return seeds


def allocate(history, run):
    with locked(history):
        path = history / 'seeds.json'
        state = json.loads(path.read_text()) if path.exists() else dict(next=20, failed=[], runs={})
        fixed = permanent_seeds()
        if run not in state['runs']:
            seed = state['next']
            while seed in fixed or seed in state['failed']:
                seed += 1
            state['next'] = seed + 1
            state['runs'][run] = dict(seed=seed, started=utc())
        rotating = state['runs'][run]['seed']
        seeds = list(dict.fromkeys([*fixed, *state['failed'], rotating]))
        state['runs'][run]['seeds'] = seeds
        write_json(path, state)
        return dict(run=run, permanent=fixed, failed=state['failed'], rotating=rotating,
                    seeds=seeds, history=str(history))


def record_leg(history, args):
    with locked(history):
        path = history / 'seeds.json'
        state = json.loads(path.read_text())
        if args.verdict == 'FAIL' and args.seed not in state['failed']:
            state['failed'].append(args.seed)
            state['failed'].sort()
            write_json(path, state)
        entry = dict(timestamp=utc(), run=args.run, seed=args.seed, suite=args.suite,
                     geometry=args.geometry, atomic=args.atomic, verdict=args.verdict,
                     log=str(Path(args.log).resolve()))
        with (history / 'legs.jsonl').open('a') as file:
            file.write(json.dumps(entry) + '\n')
            file.flush()
            os.fsync(file.fileno())


class ComparisonCoverage:
    def __init__(self):
        self.path = os.environ.get('GATE_DIFFER_COVERAGE')
        self.counts = Counter()
        self.kinds = {}
        if self.path:
            atexit.register(self.save)

    def note(self, argv, kind='reply'):
        # Only callers that HAVE read both replies and are comparing them call this. Encoding or
        # merely sending a command is not coverage: setup replies are often drained unexamined.
        if not self.path:
            return
        name = argv[0] if isinstance(argv, (tuple, list)) else argv
        if isinstance(name, bytes):
            name = name.decode('ascii')
        name = name.upper()
        self.counts[name] += 1
        self.kinds.setdefault(name, set()).add(kind)

    def save(self):
        write_json(Path(self.path), dict(schema=1, commands=dict(sorted(self.counts.items())),
                   kinds={name: sorted(kinds) for name, kinds in sorted(self.kinds.items())}))


coverage = ComparisonCoverage()


def summarize(directory):
    from cmdmeta_coverage import registered_commands
    registered = set(registered_commands())
    counts = Counter()
    files = sorted(directory.glob('*.coverage.json'))
    if not files:
        raise ValueError('no executed differential comparison coverage artifacts')
    for path in files:
        artifact = json.loads(path.read_text())
        if artifact.get('schema') != 1:
            raise ValueError(f'unknown coverage schema: {path}')
        if not artifact['commands']:
            raise ValueError(f'no comparison witness recorded by differential leg: {path}')
        counts.update(artifact['commands'])
    covered = registered & counts.keys()
    report = dict(registered=len(registered), compared=len(covered),
                  commands={name: counts[name] for name in sorted(covered)},
                  missing=sorted(registered - covered),
                  unregistered=sorted(counts.keys() - registered), artifacts=len(files))
    write_json(directory / 'command-coverage.json', report)
    print(f'DIFFER COMMAND COVERAGE: diffed {len(covered)} of {len(registered)} registered commands '
          f'({sum(counts.values())} recorded comparison witnesses, {len(files)} legs)')
    print('  not observed in compared replies/properties: ' + ', '.join(report['missing']))
    return report


def self_test():
    import io
    import runpy
    import socket
    with tempfile.TemporaryDirectory(prefix='differ-history-self-test-', dir=ROOT / 'build') as temporary:
        history = Path(temporary)
        first = allocate(history, 'run-a')
        repeated = allocate(history, 'run-a')
        assert first['seeds'] == repeated['seeds'] == [7, 19, 20]
        record_leg(history, argparse.Namespace(run='run-a', seed=20, suite='string',
                   geometry='split', atomic=1, verdict='FAIL', log=history / 'failed.log'))
        second = allocate(history, 'run-b')
        assert second['seeds'] == [7, 19, 20, 21] and second['failed'] == [20]
        # Drive the REAL differ generator, sends, reads and comparison loop without a server.
        # Identical fake reply streams are enough to test coverage plumbing. In particular the
        # FLUSHALL setup reply is drained but never diffed, so it MUST NOT become command coverage.
        class FakeSocket:
            def setsockopt(self, *args): pass
            def sendall(self, _payload): pass
            def close(self): pass
            def makefile(self, _mode): return self
            def readline(self): return b'+OK\r\n'
        old_connection, old_argv = socket.create_connection, sys.argv
        old_module = sys.modules.get('_differ_history')
        old_path, old_counts, old_kinds = coverage.path, coverage.counts, coverage.kinds
        try:
            socket.create_connection = lambda *args, **kwargs: FakeSocket()
            sys.argv = ['differ.py', 'fake-target', '1', 'fake-oracle', '2', 'string', '7']
            sys.modules['_differ_history'] = sys.modules[__name__]
            coverage.path, coverage.counts, coverage.kinds = str(history / 'leg.coverage.json'), Counter(), {}
            with contextlib.redirect_stdout(io.StringIO()):
                try:
                    runpy.run_path(str(ROOT / 'tests/differ.py'), run_name='__main__')
                except SystemExit as exc:
                    assert exc.code == 0
            assert coverage.counts['SET'] > 0 and coverage.counts['GET'] > 0
            assert 'FLUSHALL' not in coverage.counts
            assert sum(coverage.counts.values()) >= 4000
            coverage.save()
            assert json.loads((history / 'leg.coverage.json').read_text())['commands']['GET'] > 0
        finally:
            socket.create_connection, sys.argv = old_connection, old_argv
            coverage.path, coverage.counts, coverage.kinds = old_path, old_counts, old_kinds
            if old_module is None:
                sys.modules.pop('_differ_history', None)
            else:
                sys.modules['_differ_history'] = old_module
    print('DIFFER history self-test: durable failing-seed replay and real comparison-loop coverage passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history', type=Path)
    sub = parser.add_subparsers(dest='action', required=True)
    allocate_parser = sub.add_parser('allocate')
    allocate_parser.add_argument('--run', required=True)
    allocate_parser.add_argument('--output', type=Path, required=True)
    record_parser = sub.add_parser('record')
    for name in ('run', 'suite', 'geometry', 'log'):
        record_parser.add_argument('--' + name, required=True)
    record_parser.add_argument('--seed', type=int, required=True)
    record_parser.add_argument('--atomic', type=int, choices=(0, 1), required=True)
    record_parser.add_argument('--verdict', choices=('ok', 'FAIL'), required=True)
    summary_parser = sub.add_parser('summary')
    summary_parser.add_argument('directory', type=Path)
    sub.add_parser('self-test')
    args = parser.parse_args()
    if args.action == 'self-test':
        (ROOT / 'build').mkdir(exist_ok=True)
        self_test()
    elif args.action == 'summary':
        summarize(args.directory)
    else:
        history = args.history or default_history()
        if args.action == 'allocate':
            result = allocate(history, args.run)
            write_json(args.output, result)
            print(' '.join(map(str, result['seeds'])))
        else:
            record_leg(history, args)


if __name__ == '__main__':
    main()
