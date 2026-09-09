#!/usr/bin/env python3
"""Serial SET regression reproducer; results and owned builds live in build/setregress.

Build every supplied binary from clean before invoking this script. Example:
  taskset -c 96-127 python3 tools/setregress.py --output quiet-1 \
    --arm good=build/setregress/good/build/tomokv \
    --arm bad=build/setregress/bad/build/tomokv

Default: three alternating repetitions of fused/read-local=1/SET, with the task's
ports/cores, one million preloaded keys, 128 clients, depth 32 and 64-byte values.
--cell may be repeated. A fresh output directory is required for each run.
--contend-build is an explicitly contaminated diagnostic, never a performance verdict.
The process monitor detects known server/driver/compiler names; it is not a guarantee
against all possible machine interference. All cleanup uses owned Popen objects.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'build' / 'setregress'
sys.path.insert(0, str(ROOT / 'tests'))
import _lib

def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def save(path, obj):
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + '\n')

def busy():
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            name = (proc / 'comm').read_text().strip()
            if name.startswith('tomokv') or name in ('memtier_benchma', 'memtier_benchmar',
                                                    'memtier_benchmark', 'redis-benchmark',
                                                    'cc1plus', 'cc1', 'collect2', 'lto1'):
                status = (proc / 'status').read_text()
                if not re.search(r'^State:\s+Z', status, re.M):
                    found.append((int(proc.name), name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return found

def wait_quiet():
    start = time.monotonic()
    while busy():
        if time.monotonic() - start > 1800:
            raise RuntimeError('Other workload still active: ' + repr(busy()))
        print('Waiting for unrelated workload:', busy(), flush=True)
        time.sleep(15)

def descendant_of(pid, parent):
    visited = set()
    while pid > 1 and pid not in visited:
        if pid == parent:
            return True
        visited.add(pid)
        try:
            status = Path('/proc', str(pid), 'status').read_text()
            pid = int(re.search(r'^PPid:\s+(\d+)', status, re.M)[1])
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            return False
    return False

def monitor(seconds, result, child, load, compiler):
    """Invalidate the cell if an unrelated server/driver/compiler enters during measurement."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        allowed = {child.pid, load.pid}
        active = busy()
        if compiler:
            # Only the owned make's descendants belong to this deliberate control.
            assert compiler.poll() is None, 'Contention control lost its build'
            workers = [(pid, name) for pid, name in active
                       if name in ('cc1plus', 'cc1', 'collect2', 'lto1')
                       and descendant_of(pid, compiler.pid)]
            allowed.update(pid for pid, _ in workers)
            result['compiler_witness_samples'] = result.get('compiler_witness_samples', 0) + bool(workers)
        foreign = [(pid, name) for pid, name in active if pid not in allowed]
        if foreign:
            result['contamination'] = dict(utc=timestamp(), processes=foreign)
            raise RuntimeError('Invalid measurement: unrelated workload appeared: ' + repr(foreign))
        if child.poll() is not None:
            raise RuntimeError('Server exited during measurement')
        time.sleep(min(.2, max(0, end - time.monotonic())))

def capture(conn, out, stem):
    raw = conn.must('INFO', 'all').decode()
    (out / (stem + '.info')).write_text(raw)
    return dict(line.split(':', 1) for line in raw.splitlines()
                if line and not line.startswith('#') and ':' in line)

def one(args, arm, binary, mode, lane, op, rep):
    wait_quiet()
    out = HERE / args.output / f'{arm}-{mode}-rl{lane}-{op}-{rep}'
    out.mkdir(parents=True)
    (out / 'data').mkdir()
    port = args.port
    assert 8460 <= port <= 8489
    with socket.socket() as guard:
        guard.bind(('127.0.0.1', port))
    command = ['taskset', '-c', args.cores, str(binary), '--port', str(port),
               '--bind', '127.0.0.1', '--thread-mode', mode, '--atomic', '1',
               '--read-local', str(lane), '--save', '', '--dir', str(out / 'data')]
    if mode == '2s':
        command += ['--ratio', args.ratio, '--flip-auto', str(args.flip)]
    if args.shards:
        command += ['--shards', str(args.shards)]
    command += args.server_arg
    common = ['taskset', '-c', '96-127', '/usr/bin/memtier_benchmark', '-s',
              '127.0.0.1', '-p', str(port), '--protocol=redis', '--pipeline=32',
              '--key-pattern=P:P', '--key-minimum=1', '--key-maximum=1000000',
              '-d', '64', '--hide-histogram']
    populate = common + ['-t', '8', '-c', '8', '--ratio=1:0', '-n', 'allkeys']
    loadcmd = common + ['-t', '16', '-c', '8', '--ratio=' + ('1:0' if op == 'SET' else '0:1'),
                        '--test-time=' + str(args.seconds)]
    result = dict(started_utc=timestamp(), arm=arm, mode=mode, lane=lane, op=op, rep=rep,
                  server_command=command, populate_command=populate, load_command=loadcmd,
                  sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    child = load = ctl = profiler = compiler = None
    try:
        with (out / 'server.log').open('w') as log:
            child = subprocess.Popen(command, cwd=out / 'data', stdout=log, stderr=subprocess.STDOUT)
        result['pid'] = child.pid
        save(out / 'result.json', result)
        deadline = time.monotonic() + 30
        while True:
            if child.poll() is not None:
                raise RuntimeError('Server boot failed: ' + str(child.returncode))
            try:
                ctl = _lib.Conn('127.0.0.1', port, timeout=3)
                assert ctl.must('PING') == b'PONG'
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.05)
        result['boot'] = capture(ctl, out, 'boot')
        with (out / 'populate.log').open('w') as log:
            subprocess.run(populate, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        result['dbsize'] = ctl.must('DBSIZE')
        assert result['dbsize'] == 1000000, result['dbsize']
        result['before'] = capture(ctl, out, 'before')
        if args.contend_build:
            buildtree = (HERE / args.contend_build).resolve()
            assert buildtree.parent == HERE and (buildtree / 'Makefile').is_file()
            clean = ['taskset', '-c', '8-31', 'make', '-C', str(buildtree), 'clean']
            compilecmd = ['taskset', '-c', '8-31', 'make', '-C', str(buildtree), '-j20']
            with (out / 'contending-build.log').open('w') as log:
                subprocess.run(clean, stdout=log, stderr=subprocess.STDOUT, check=True)
                compiler = subprocess.Popen(compilecmd, stdout=log, stderr=subprocess.STDOUT)
            result['contending_build'] = dict(clean_command=clean, command=compilecmd,
                                               pid=compiler.pid)
        with (out / 'load.log').open('w') as log:
            load = subprocess.Popen(loadcmd, stdout=log, stderr=subprocess.STDOUT)
        result['load_pid'] = load.pid
        result['load_started_utc'] = timestamp()
        save(out / 'result.json', result)
        monitor(2, result, child, load, compiler)
        result['during_early'] = capture(ctl, out, 'during-early')
        if args.profile:
            if args.profile == 'stat':
                pcmd = ['taskset', '-c', '96-127', 'perf', 'stat', '-p', str(child.pid),
                        '-e', 'cycles,instructions,cache-misses,branches,branch-misses',
                        '-o', str(out / 'perf-stat.txt'), '--', 'sleep', str(args.seconds - 4)]
            else:
                pcmd = ['taskset', '-c', '96-127', 'perf', 'record', '-F', '499',
                        '-g', '-p', str(child.pid), '-o', str(out / 'perf.data'),
                        '--', 'sleep', str(args.seconds - 4)]
            with (out / 'perf.log').open('w') as log:
                profiler = subprocess.Popen(pcmd, stdout=log, stderr=subprocess.STDOUT)
            result['profile_command'] = pcmd
        monitor(max(1, args.seconds - 4), result, child, load, compiler)
        result['during_late'] = capture(ctl, out, 'during-late')
        deadline = time.monotonic() + 60
        while load.poll() is None:
            if time.monotonic() > deadline:
                raise RuntimeError('Load generator did not finish')
            monitor(.2, result, child, load, compiler)
        assert load.returncode == 0, load.returncode
        if profiler:
            result['perf_exit'] = profiler.wait(timeout=15)
            assert result['perf_exit'] == 0, 'Requested profile failed'
        result['load_ended_utc'] = timestamp()
        result['after'] = capture(ctl, out, 'after')
        if compiler:
            result['contending_build']['active_at_load_end'] = compiler.poll() is None
            assert compiler.poll() is None, 'The contention window ended before the load'
            assert result.get('compiler_witness_samples', 0) > 0, 'No compiler witnessed during load'
        matches = re.findall(r'^Totals\s+([0-9.]+)', (out / 'load.log').read_text(), re.M)
        assert len(matches) == 1, matches
        result['rate'] = float(matches[0])
        result['ok'] = True
    except Exception as exc:
        result['error'] = repr(exc)
        raise
    finally:
        if ctl:
            ctl.close()
        for proc in (profiler, load, child):
            if proc is None:
                continue
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
                result['forced_stop'] = True
        if child:
            result['server_exit'] = child.returncode
        if compiler:
            print('Measurement complete; waiting for owned clean build', compiler.pid, flush=True)
            result['contending_build']['exit'] = compiler.wait(timeout=600)
        result['finished_utc'] = timestamp()
        save(out / 'result.json', result)
    assert result['server_exit'] == 0, result
    print(f"{arm:16} {mode} rl{lane} {op} rep={rep} {result['rate']/1e6:.6f} Mops/s "
          f"arms={result['during_late'].get('read_local_arms')} "
          f"records={result['during_late'].get('read_local_write_ring_records')} "
          f"hits={result['during_late'].get('read_local_hits')}", flush=True)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--arm', action='append', required=True, help='label=/absolute/binary')
    parser.add_argument('--output', required=True)
    parser.add_argument('--cell', action='append', help='1s:1:SET etc.; default armed fused SET')
    parser.add_argument('--reps', type=int, default=3)
    parser.add_argument('--seconds', type=int, default=8)
    parser.add_argument('--cores', default='8-31')
    parser.add_argument('--ratio', default='12:12')
    parser.add_argument('--flip', type=int, default=1)
    parser.add_argument('--shards', type=int)
    parser.add_argument('--port', type=int, default=8460)
    parser.add_argument('--server-arg', action='append', default=[])
    parser.add_argument('--profile', choices=['stat', 'record'])
    parser.add_argument('--contend-build', help='EXPLICIT contention control only: clean/build this owned child tree during load')
    args = parser.parse_args()
    assert os.sched_getaffinity(0) <= set(range(96, 128))
    arms = [(label, Path(binary).resolve()) for label, binary in (a.split('=', 1) for a in args.arm)]
    cells = [(mode, int(lane), op) for mode, lane, op in
             (c.split(':') for c in (args.cell or ['1s:1:SET']))]
    assert all(mode in ('1s', '2s') and lane in (0, 1) and op in ('SET', 'GET')
               for mode, lane, op in cells), cells
    HERE.mkdir(parents=True, exist_ok=True)
    lock = (HERE / 'measure.lock').open('a')
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
    assert args.seconds >= 5
    assert args.reps > 0
    results = []
    for rep in range(1, args.reps + 1):
        for mode, lane, op in cells:
            for arm, binary in (arms if rep % 2 else list(reversed(arms))):
                results.append(one(args, arm, binary, mode, lane, op, rep))
                save(HERE / args.output / 'results.json', results)

if __name__ == '__main__':
    main()
