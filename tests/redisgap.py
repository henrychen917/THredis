#!/usr/bin/env python3
"""Bounded configuration compatibility checks; no gate or benchmark.

taskset -c 68-79 python3 tests/redisgap.py --binary build/tomokv
Only starts taskset children on ports 8120..8139 and stops its own Popen objects.
Fixtures/logs stay under build/redisgap-tests. A baseline binary must fail this test.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import time


class Resp:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.reader = self.sock.makefile('rb')

    def close(self):
        self.reader.close()
        self.sock.close()

    def read(self):
        line = self.reader.readline()
        if not line:
            raise EOFError('server closed connection')
        kind, value = line[:1], line[1:-2]
        if kind == b'+':
            return value.decode()
        if kind == b'-':
            return {'error': value.decode()}
        if kind == b':':
            return int(value)
        if kind == b'$':
            count = int(value)
            if count == -1:
                return None
            result = self.reader.read(count)
            assert self.reader.read(2) == b'\r\n'
            return result.decode()
        if kind in (b'*', b'%'):
            return [self.read() for _ in range(int(value) * (2 if kind == b'%' else 1))]
        raise AssertionError(line)

    def command(self, *args):
        encoded = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
        self.sock.sendall(b'*%d\r\n' % len(encoded) + b''.join(
            b'$%d\r\n' % len(arg) + arg + b'\r\n' for arg in encoded))
        return self.read()


class Harness:
    def __init__(self, binary, root, port):
        self.binary, self.root, self.port = binary.resolve(), root.resolve(), port
        self.root.mkdir(parents=True, exist_ok=False)
        self.sequence = 0
        self.results = []

    def listening(self):
        for table in ('/proc/net/tcp', '/proc/net/tcp6'):
            for row in Path(table).read_text().splitlines()[1:]:
                fields = row.split()
                if fields[3] == '0A' and int(fields[1].split(':')[1], 16) == self.port:
                    return True
        return False

    def args(self, mode, directory, extra, config):
        args = ['taskset', '-c', '68-79', str(self.binary)]
        if config:
            args.append(str(config))
        args += ['--bind', '127.0.0.1', '--port', str(self.port), '--shards', '16',
                 '--thread-mode', mode, '--save', '', '--dir', str(directory),
                 '--enable-debug-command', 'yes']
        if mode == '2s':
            args += ['--ratio', '6:2']
        return args + list(extra)

    @contextlib.contextmanager
    def server(self, mode, extra=(), config=None, directory=None):
        # Check LISTEN state, not bind(): TIME_WAIT from an owned, already-exited child can
        # reject a bind probe even though the server's SO_REUSEPORT listener can safely restart.
        assert not self.listening(), 'port is occupied'
        self.sequence += 1
        directory = directory or self.root / ('data-%d' % self.sequence)
        directory.mkdir(exist_ok=True)
        log_path = self.root / ('server-%d.log' % self.sequence)
        args = self.args(mode, directory, extra, config)
        with log_path.open('wb') as log:
            child = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
            client = None
            try:
                deadline = time.monotonic() + 20
                while child.poll() is None and time.monotonic() < deadline:
                    try:
                        client = Resp(self.port)
                        assert client.command('PING') == 'PONG'
                        break
                    except (OSError, EOFError):
                        if client:
                            client.close()
                        client = None
                        time.sleep(.05)
                assert client is not None, log_path.read_text()
                # Inspect every actual worker mask, including threads pinned by the server.
                for task in Path('/proc/%d/task' % child.pid).iterdir():
                    assert os.sched_getaffinity(int(task.name)) <= set(range(68, 80))
                yield client, directory, log_path
            finally:
                if client:
                    client.close()
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=5)
                        raise AssertionError('owned server needed SIGKILL: ' + str(log_path))
                assert child.returncode == 0, log_path.read_text()
                # The kernel can finish releasing io_uring's listener references after waitpid.
                # Require actual closure before another arm; never overlap successive servers.
                deadline = time.monotonic() + 5
                while self.listening() and time.monotonic() < deadline:
                    time.sleep(.01)
                assert not self.listening(), 'owned listener did not close after process exit'

    def reject(self, mode, extra=(), config=None, directory=None, contains='unknown argument'):
        directory = directory or self.root
        args = self.args(mode, directory, extra, config)
        result = subprocess.run(args, capture_output=True, text=True, timeout=15)
        self.results.append({'argv': args, 'exit': result.returncode,
                             'stdout': result.stdout, 'stderr': result.stderr})
        assert result.returncode == 1 and contains in result.stderr, self.results[-1]


def get(c, name):
    result = c.command('CONFIG', 'GET', name)
    assert len(result) == 2, (name, result)
    return result[1]


def check_aliases(h, mode):
    config = h.root / (mode + '.conf')
    config.write_text('hash-max-ziplist-entries 3\nhash-max-listpack-value 1kb\n'
                      'zset-max-listpack-entries 3\nzset-max-ziplist-value 1KB\n'
                      'hll-sparse-max-bytes 1kb\n')
    socket_path = h.root / (mode + '.sock')
    with h.server(mode, ['--unixsocket', str(socket_path), '--unixsocketperm', '0600',
                         '--hash-max-listpack-entries', '2'], config) as (c, _, _):
        assert stat.S_IMODE(socket_path.stat().st_mode) == 0o600
        assert get(c, 'unixsocketperm') == '600'
        assert get(c, 'unixsocket') == str(socket_path)
        assert get(c, 'port') == str(h.port)
        assert get(c, 'bind') == '127.0.0.1'
        assert get(c, 'aof-load-truncated') == 'yes'
        assert get(c, 'hll-sparse-max-bytes') == '1024'
        assert c.command('PFADD', 'hll-small', 'member') == 1
        assert c.command('PFDEBUG', 'ENCODING', 'hll-small') == 'sparse'
        for name, value in [('unixsocketperm', '700'), ('aof-load-truncated', 'no'),
                            ('hll-sparse-max-bytes', '0'),
                            ('port', h.port), ('bind', '127.0.0.1'), ('unixsocket', '')]:
            assert 'immutable' in c.command('CONFIG', 'SET', name, value)['error']
        for family, initial in [('hash', '2'), ('zset', '3')]:
            names = [family + '-max-' + spelling + '-entries'
                     for spelling in ('listpack', 'ziplist', 'compact')]
            assert [get(c, name) for name in names] == [initial] * 3
            assert get(c, family + '-max-listpack-value') == '1024'
            for name in names:
                assert c.command('CONFIG', 'SET', name, '2') == 'OK'
                assert [get(c, alias) for alias in names] == ['2'] * 3
                key = name
                if family == 'hash':
                    assert c.command('HSET', key, 'a', '1', 'b', '2') == 2
                    assert c.command('OBJECT', 'ENCODING', key) == 'listpack'
                    assert c.command('HSET', key, 'c', '3') == 1
                    assert c.command('OBJECT', 'ENCODING', key) == 'hashtable'
                else:
                    assert c.command('ZADD', key, '1', 'a', '2', 'b') == 2
                    assert c.command('OBJECT', 'ENCODING', key) == 'listpack'
                    assert c.command('ZADD', key, '3', 'c') == 1
                    assert c.command('OBJECT', 'ENCODING', key) == 'skiplist'
            for spelling in ('listpack', 'ziplist'):
                value_name = family + '-max-' + spelling + '-value'
                assert c.command('CONFIG', 'SET', value_name, '4b') == 'OK'
                for size, encoding in [(4, 'listpack'), (5, 'hashtable' if family == 'hash' else 'skiplist')]:
                    key = value_name + str(size)
                    args = ('HSET', key, 'f', 'x' * size) if family == 'hash' else ('ZADD', key, 1, 'x' * size)
                    assert c.command(*args) == 1
                    assert c.command('OBJECT', 'ENCODING', key) == encoding
            for bad in ('-1', '01', '+1', '1kb', '4294967296', '1\x002'):
                assert 'error' in c.command('CONFIG', 'SET', names[0], bad)
                assert get(c, names[0]) == '2'
            assert c.command('CONFIG', 'SET', names[2].swapcase(), '02') == 'OK'
            assert c.command('CONFIG', 'SET', names[2], 1, names[2], 2) == 'OK'
            assert 'error' in c.command('CONFIG', 'SET', names[0], 1, names[0], 2)
            assert c.command('CONFIG', 'SET', names[0], 1, names[1], 2) == 'OK'
            assert [get(c, alias) for alias in names] == ['2'] * 3
            assert 'error' in c.command('CONFIG', 'SET', names[0], 1, names[1], '-1')
            assert [get(c, alias) for alias in names] == ['2'] * 3
            assert c.command('CONFIG', 'SET', names[0], '4294967295') == 'OK'
            assert get(c, names[0]) == '4294967295'
            assert c.command('CONFIG', 'SET', names[0], '0') == 'OK'
            # Fresh objects must cross the real promotion path, on multiple shard owners.
            for index in range(32):
                key = family + '-disabled-' + str(index)
                command = ('HSET', key, 'f', 'v') if family == 'hash' else ('ZADD', key, 1, 'm')
                assert c.command(*command) == 1
                assert c.command('OBJECT', 'ENCODING', key) == ('hashtable' if family == 'hash' else 'skiplist')
        for protocol in (2, 3):
            c.command('HELLO', protocol)
            assert c.command('CONFIG', 'GET', 'does-not-exist') == []
            assert c.command('CONFIG', 'GET', 'loglevel') == []
            assert 'Unknown option' in c.command('CONFIG', 'SET', 'loglevel', 'notice')['error']
            assert 'error' in c.command('CONFIG', 'SET', 'timeout', 17, 'does-not-exist', 1)
            assert get(c, 'timeout') == '0'
            values = c.command('CONFIG', 'GET', 'hash-max-*', '*-listpack-*')
            assert len(values[::2]) == len(set(values[::2]))
        assert c.command('CONFIG', 'REWRITE') == 'OK'
        text = config.read_text()
        assert 'hash-max-listpack-entries 0\n' in text
        assert 'zset-max-listpack-entries 0\n' in text
        assert 'max-ziplist-' not in text
        assert 'hash-max-compact-' not in text and 'zset-max-compact-' not in text
    assert not socket_path.exists()
    with h.server(mode, ['--hll-sparse-max-bytes', '0'], config=config) as (c, _, _):
        assert get(c, 'hash-max-ziplist-entries') == '0'
        assert get(c, 'zset-max-compact-entries') == '0'
        assert stat.S_IMODE(socket_path.stat().st_mode) == 0o600
        assert get(c, 'hll-sparse-max-bytes') == '0'
        assert c.command('PFADD', 'hll-dense', 'member') == 1
        assert c.command('PFDEBUG', 'ENCODING', 'hll-dense') == 'dense'
        # A valid sparse image with one register set. SET preserves its encoding, so PFMERGE
        # must consult the cutoff itself rather than merely inheriting a dense PFADD source.
        header = bytearray(16)
        header[:4], header[4], header[15] = b'HYLL', 1, 0x80
        assert c.command('SET', 'hll-source', bytes(header) + b'\x80\x7f\xfe') == 'OK'
        assert c.command('PFDEBUG', 'ENCODING', 'hll-source') == 'sparse'
        assert c.command('PFMERGE', 'hll-merged', 'hll-source') == 'OK'
        assert c.command('PFDEBUG', 'ENCODING', 'hll-merged') == 'dense'
        assert c.command('PFCOUNT', 'hll-merged') == 1
    for name in ('loglevel', 'cluster-enabled', 'maxmemroy'):
        unknown = h.root / ('unknown-' + name + '.conf')
        unknown.write_text('save ""\nmaxmemory 256mb\n' + name + ' notice\n')
        h.reject(mode, config=unknown, contains="unknown argument '--" + name + "'")
    for flag, value, error in [('hash-max-listpack-entries', '01', 'wants an integer'),
                               ('hash-max-ziplist-value', '4294967296', 'wants a byte count'),
                               ('unixsocketperm', '888', 'wants an octal mode'),
                               ('hll-sparse-max-bytes', '4294967296', 'wants a byte count'),
                               ('aof-load-truncated', '1', 'wants yes or no')]:
        h.reject(mode, ['--' + flag, value], contains=error)
    print('PASS', mode, 'aliases, grammar, actual promotion, HLL cutoff, unknowns, Unix permissions, rewrite/restart', flush=True)


def check_recovery(h, mode):
    for manifest in (True, False):
        with h.server(mode, ['--appendonly', 'yes', '--appendfsync', 'always',
                             '--auto-aof-rewrite-percentage', '0']) as (c, source, _):
            if manifest:
                assert c.command('SET', 'base', 'seed') == 'OK'
                assert c.command('BGREWRITEAOF') == 'Background append only file rewriting started'
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    info = c.command('INFO', 'persistence')
                    if ('aof_rewrite_in_progress:0' in info and
                            list((source / 'appendonlydir').glob('*.manifest'))):
                        break
                    time.sleep(.02)
                else:
                    raise AssertionError('AOF rewrite never produced a manifest')
            assert c.command('SET', 'durable', 'kept') == 'OK'
        directory = h.root / ('recovery-' + mode + '-' + str(manifest))
        shutil.copytree(source, directory)
        aof_dir = directory / 'appendonlydir'
        increments = sorted(aof_dir.glob('*.incr.tomo'))
        assert increments, list(aof_dir.iterdir())
        assert bool(list(aof_dir.glob('*.manifest'))) == manifest
        if not manifest:
            assert len(increments) == 1
        tail = increments[-1]
        intact = tail.read_bytes()
        tail.write_bytes(intact + b'x')
        h.reject(mode, ['--appendonly', 'yes', '--aof-load-truncated', 'no'],
                 directory=directory, contains='truncated AOF tail')
        assert tail.read_bytes() == intact + b'x', 'strict recovery modified the file'
        with h.server(mode, ['--appendonly', 'yes', '--aof-load-truncated', 'yes'],
                      directory=directory) as (c, _, log):
            assert c.command('GET', 'durable') == 'kept'
            assert 'truncated AOF tail' in log.read_text()
    print('PASS', mode, 'strict/permissive AOF recovery, manifest and legacy paths', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/tomokv'))
    parser.add_argument('--root', type=Path,
                        help='fresh fixture directory (default: unique directory under build/)')
    parser.add_argument('--port', type=int, default=8124, choices=range(8120, 8140))
    args = parser.parse_args()
    if args.root is None:
        args.root = Path('build') / ('redisgap-tests-%d' % time.time_ns())
    h = Harness(args.binary, args.root, args.port)
    try:
        for mode in ('2s', '1s'):
            check_aliases(h, mode)
            check_recovery(h, mode)
    finally:
        (h.root / 'rejections.json').write_text(json.dumps(h.results, indent=2) + '\n')
    print('PASS redisgap configuration checks (no gate rows added)', flush=True)


if __name__ == '__main__':
    main()
