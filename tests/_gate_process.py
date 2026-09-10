"""Process/telemetry plumbing for the feature and loopback performance gates.

All artifacts live in the requested output directory. Never discover or kill a server by name:
the Popen object is the ownership token, and INFO must identify that exact PID before any traffic.
"""
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

from _lib import Conn, encode


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def cpus(spec):
    result = set()
    for item in spec.split(','):
        ends = [int(x) for x in item.split('-')]
        require(len(ends) in (1, 2), f'invalid CPU list: {spec}')
        lo, hi = ends[0], ends[-1]
        require(0 <= lo <= hi, f'invalid CPU range: {item}')
        result.update(range(lo, hi + 1))
    require(result, 'empty CPU list')
    return sorted(result)


def cpu_spec(values):
    return ','.join(map(str, sorted(values)))


def pin_driver(server_cpus, load_cpus):
    require(not set(cpus(server_cpus)) & set(cpus(load_cpus)),
            'server and load CPUs overlap')
    os.sched_setaffinity(0, cpus(load_cpus))


def fields(raw):
    require(isinstance(raw, bytes), f'expected telemetry bulk string, got {raw!r}')
    return dict(line.split(':', 1) for line in raw.decode().splitlines()
                if ':' in line and not line.startswith('#'))


def info(conn, *sections):
    result = {}
    for section in sections or ('ALL',):
        result.update(fields(conn.must('INFO', section)))
    return result


def number(row, key):
    require(key in row, f'missing mandatory counter {key}')
    value = int(row[key])
    require(value >= 0, f'negative counter {key}: {value}')
    return value


def delta(before, after, key):
    value = number(after, key) - number(before, key)
    require(value >= 0, f'counter reset/wrapped: {key}')
    return value


def thread_readers(row):
    return {int(key.removeprefix('read_local_thread_')):
            dict(item.split('=', 1) for item in value.split(','))
            for key, value in row.items() if key.startswith('read_local_thread_')}


def lb_snapshot(conn):
    return parse_lb(conn.must('DEBUG', 'LBSIGNALS'))


def parse_lb(raw):
    require(isinstance(raw, bytes), f'expected LBSIGNALS bulk string: {raw!r}')
    threads, shards, stamp = {}, {}, None
    for line in raw.decode().splitlines():
        v = line.split()
        if v[0] == 'lbver':
            require(v[:3] == ['lbver', '1', 'stamp_ns'], 'unsupported LBSIGNALS schema')
            stamp = int(v[3])
        elif v[0] == 'thread':
            threads[int(v[1])] = dict(role=v[2], clients=int(v[4]), iterations=int(v[5]),
                                     ops=int(v[6]), busy_ns=int(v[7]), idle_ns=int(v[8]))
        elif v[0] == 'shard':
            shards[int(v[1])] = int(v[2])
    require(stamp and threads and shards, 'missing LBSIGNALS timestamp/threads/shards')
    return dict(stamp_ns=stamp, threads=threads, shards=shards)


def stop_process(process):
    forced = False
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            forced = True
            process.kill()
            process.wait(timeout=5)
    return forced


@contextlib.contextmanager
def server(binary, server_cpus, port, directory, args):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    data = directory / 'data'
    data.mkdir()
    # Successive cells reuse the gate's allocated port, including immediately after TLS and
    # io_uring teardown. A bind-only probe mistakes retained TCP state for a live server.
    # Reject every live listener (even one that cannot answer PING), then require INFO below
    # to identify the exact child we launched before exercising the cell.
    listeners = subprocess.check_output(
        ['ss', '-H', '-ltnp', f'sport = :{port}'], stderr=subprocess.STDOUT, text=True)
    (directory / 'port-guard.txt').write_text(listeners)
    require(not listeners.strip(), f'port {port} already has a live listener: {listeners.strip()}')
    argv = ['taskset', '-c', server_cpus, str(Path(binary).resolve()), '--port', str(port),
            '--bind', '127.0.0.1', '--dir', str(data), '--save', '', '--appendonly', 'no',
            '--enable-debug-command', 'yes', *map(str, args)]
    (directory / 'argv.json').write_text(json.dumps(argv, indent=2) + '\n')
    conn = None
    with (directory / 'server.log').open('w') as log:
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        (directory / 'pid').write_text(str(process.pid) + '\n')
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                require(process.poll() is None,
                        'server exited before boot: ' + (directory / 'server.log').read_text()[-2000:])
                try:
                    conn = Conn('127.0.0.1', port, timeout=5)
                    actual = info(conn, 'SERVER')
                    require(number(actual, 'process_id') == process.pid,
                            'port answered by a different PID; refusing traffic')
                    break
                except (ConnectionRefusedError, ConnectionResetError):
                    if conn:
                        conn.close()
                        conn = None
                    time.sleep(.025)
            require(conn is not None, f'boot timeout on port {port}')
            yield conn, process
        finally:
            if conn:
                conn.close()
            exited_before_cleanup = process.poll()
            forced = stop_process(process)
            (directory / 'exit.json').write_text(json.dumps(dict(
                pid=process.pid, exited_before_cleanup=exited_before_cleanup,
                returncode=process.returncode, forced_kill=forced)) + '\n')
        require(process.returncode == 0, f'server shutdown rc={process.returncode}; see {directory}')
        lines = (directory / 'server.log').read_text().splitlines()
        require(lines and lines[-1].startswith('shutdown_report '), 'missing final shutdown report')
        reports = [line.split('shutdown_report ', 1)[1] for line in lines if 'shutdown_report {' in line]
        require(len(reports) == 1, 'missing/duplicate final shutdown report')
        report = json.loads(reports[0])
        require(report.get('schema') == 1 and all(report.get('stuck', {}).get(k) == 0
                for k in ('live_conns', 'rob_not_quiesced', 'unsent_bytes_pending')),
                f'unclean shutdown report: {report}')


def install_signals():
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)


def populate(conn, keymax, value=b'x' * 64):
    require(conn.must('DBSIZE') == 0, 'population requires a fresh empty server')
    for first in range(1, keymax + 1, 512):
        keys = range(first, min(first + 512, keymax + 1))
        conn.raw(b''.join(encode('SET', f'gate-{key}', value) for key in keys))
        for _ in keys:
            require(conn.read() == b'OK', 'population SET failed')
    require(conn.must('DBSIZE') == keymax, 'DBSIZE != keymax after population')
