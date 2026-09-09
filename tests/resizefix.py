"""PID-owning launcher for the resize batteries and a traffic-free idle observation.

Only these batteries run. Servers: cores 8-15, ports 8540-8543, 16 shards, split 6:2 or fused.
The idle observer stops its own child and reads its memory; no diagnostic command can wake the
owner into finishing the resize. GDB reads DWARF from the binary only, never attaches or calls
inferior functions. Requires the normal Linux parent-to-child /proc/PID/mem permission.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

from _lib import Conn, encode, info, topology
from rehash_readonly import (BOUND_SECONDS, MAX_KEYS, TARGET_OLD_CAPACITY, VALUE,
                             candidates, read_batch, require, state)

ROOT = Path(__file__).resolve().parents[1]


class StoreMemory:
    def __init__(self, binary, pid):
        self.pid = pid
        # Derive offsets from THIS build, including optional sidecars, rather than hard-coding
        # a layout and risking a vacuous all-zero read from the wrong object.
        fields = {
            'shards': '&((tomo::Server*)0)->shards_._M_impl._M_start',
            'store': '&((tomo::Shard*)0)->store_',
            'tab': '&((tomo::FlatStore*)0)->tab_',
            'cap': '&((tomo::FlatStore*)0)->cap_',
            'live': '&((tomo::FlatStore*)0)->live_',
            'cursor': '&((tomo::FlatStore*)0)->rehash_pos_',
            'pending': '&((tomo::FlatStore*)0)->atomic_pending_',
            'sink': '&((tomo::ReadLocalStoreState*)0)->retire_sink.context',
            'resizes': '&((tomo::ReadLocalDeferredQueue*)0)->resizes_',
            'resize_count': '&((tomo::ResizeRetireQueue*)0)->count_',
        }
        cmd = ['gdb', '-q', '-nx', '-batch', str(binary)]
        for name, expression in fields.items():
            cmd += ['-ex', 'printf "OFFSET %s=%%llu\\n", (unsigned long long)%s' %
                    (name, expression)]
        layout = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.offset = {}
        for line in layout.stdout.splitlines():
            if line.startswith('OFFSET '):
                name, value = line.removeprefix('OFFSET ').split('=')
                self.offset[name] = int(value)
        require(set(fields) - {'resizes', 'resize_count'} <= self.offset.keys(),
                'required DWARF offsets unavailable: ' + layout.stderr)
        symbols = subprocess.check_output(['nm', '-C', str(binary)], text=True, timeout=30)
        matches = [int(line.split()[0], 16) for line in symbols.splitlines()
                   if line.endswith('tomo::(anonymous namespace)::g_server')]
        require(len(matches) == 1, 'unique live Server pointer symbol required')
        with binary.open('rb') as elf:
            elf.seek(16)
            elf_type = int.from_bytes(elf.read(2), 'little')
        require(elf_type in (2, 3), 'Linux ELF executable required')
        bases = [int(line.split()[0].split('-')[0], 16)
                 for line in Path('/proc/%d/maps' % pid).read_text().splitlines()
                 if line.split()[-1] == str(binary) and int(line.split()[2], 16) == 0]
        require(len(bases) == 1, 'unique executable load base required')
        self.server_address = matches[0] + (bases[0] if elf_type == 3 else 0)

    def sample(self, local):
        # Only the launcher parent may stop/read this PID. No attach, server-side function call,
        # protocol observer or command is involved in this snapshot.
        parent = int(Path('/proc/%d/stat' % self.pid).read_text().rsplit(')', 1)[1].split()[1])
        require(parent == os.getpid(), 'idle observer must own the server PID')
        os.kill(self.pid, signal.SIGSTOP)
        try:
            child, status = os.waitpid(self.pid, os.WUNTRACED)
            require(child == self.pid and os.WIFSTOPPED(status), 'our child must stop')
            stopped_at = time.monotonic()
            with open('/proc/%d/mem' % self.pid, 'rb', buffering=0) as memory:
                def integer(address, size=8):
                    require(address != 0, 'null diagnostic address')
                    memory.seek(address)
                    data = memory.read(size)
                    require(len(data) == size, 'short diagnostic read')
                    return int.from_bytes(data, 'little')
                offset = self.offset
                server = integer(self.server_address)
                vector = integer(server + offset['shards'])
                store = integer(vector) + offset['store']
                result = dict(capacity=integer(store + offset['cap'], 4),
                              old_capacity=integer(store + offset['cap'] + 4, 4),
                              cursor=integer(store + offset['cursor'], 4),
                              old_live=integer(store + offset['live'] + 4, 4),
                              keys=integer(store + offset['live'], 4) +
                                   integer(store + offset['live'] + 4, 4),
                              old_table=integer(store + offset['tab'] + 8))
                if local and 'resizes' in offset and 'resize_count' in offset:
                    pending = integer(store + offset['pending'])
                    queue = integer(pending + offset['sink'])
                    result['retired_tables'] = integer(queue + offset['resizes'] +
                                                       offset['resize_count'], 4)
            return stopped_at, result
        finally:
            os.kill(self.pid, signal.SIGCONT)


def idle_battery(binary, pid, port, mode, local):
    observer = StoreMemory(binary, pid)
    conn = Conn('127.0.0.1', port, timeout=3.0)
    try:
        topo = topology(conn)
        meta = info(conn, 'server')
        require(topo.mode == {'split': '2s', 'fused': '1s'}[mode] and
                int(meta['read_local']) == local and len(topo.roles) == 8 and
                len(topo.shard_owner) == 16, 'gate geometry and requested posture')
        require(conn.cmd('FLUSHDB') == b'OK', 'fresh state')
        require(conn.cmd('DEBUG', 'SET-ACTIVE-EXPIRE', 0) == b'OK', 'pause maintenance')
        baseline = state(conn)
        require(baseline.keys == 0 and baseline.old_capacity == 0, 'empty stable baseline')
        source = candidates(conn)
        inserted = []
        for _ in range(MAX_KEYS // 64):
            batch = [next(source) for _ in range(64)]
            conn.raw(b''.join(encode('SET', key, VALUE) for key in batch))
            for key in batch:
                require(conn.read() == b'OK', 'grow insertion: ' + key)
            inserted.extend(batch)
            armed = state(conn)
            if armed.old_capacity >= TARGET_OLD_CAPACITY:
                break
        else:
            raise AssertionError('bounded arming failed; no skip')
        require(armed.starts > baseline.starts and armed.remaining > 0 and armed.old_live > 0 and
                armed.capacity == 2 * armed.old_capacity and armed.keys == len(inserted),
                'real resize with live old keys and unexamined slots')
        _, before = observer.sample(local)
        for name in ('capacity', 'old_capacity', 'cursor', 'old_live', 'keys'):
            require(before[name] == getattr(armed, name), 'memory observer must match owner: ' + name)
        require(before['old_table'] != 0, 'old table pointer must actually exist')
        read_batch(conn, inserted[:32] + inserted[-32:])
        require(state(conn) == armed, 'paused controls cannot advance resize')
        print('ARMED idle mode=%s read_local=%d %s memory=%s' % (mode, local, armed, before),
              flush=True)

        require(conn.cmd('DEBUG', 'SET-ACTIVE-EXPIRE', 1) == b'OK', 'resume maintenance')
        start = time.monotonic()
        # Keep the socket open but send NOTHING: no GET, INFO, DEBUG, PING, connect or close.
        time.sleep(1.0)
        stopped_at, after = observer.sample(local)
        elapsed = stopped_at - start
        print('RESULT idle quiet=%.6fs protocol_requests=0 memory=%s' % (elapsed, after), flush=True)
        require(elapsed <= BOUND_SECONDS and after['old_capacity'] == 0 and
                after['old_table'] == 0 and after['cursor'] == 0 and after['old_live'] == 0 and
                after['capacity'] == armed.capacity and after['keys'] == armed.keys,
                'idle resize did not complete before the traffic-free memory observation')
        if 'retired_tables' in after:
            require(after['retired_tables'] == 0, 'idle owner must also drain the table retirement')
        for first in range(0, len(inserted), 128):
            read_batch(conn, inserted[first:first + 128])
        print('PASS idle completion and all %d values' % len(inserted), flush=True)
    finally:
        try:
            require(conn.cmd('DEBUG', 'SET-ACTIVE-EXPIRE', 1) == b'OK', 'restore maintenance')
        finally:
            conn.close()

def execute(cmd, log, timeout):
    with log.open('w') as out:
        result = subprocess.run(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                                timeout=timeout)
    print(log.name, 'PASS' if result.returncode == 0 else 'FAIL', flush=True)
    return result.returncode

def main():
    p = argparse.ArgumentParser()
    p.add_argument('binary', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--repeats', type=int, default=1)
    p.add_argument('--idle', action='store_true')
    args = p.parse_args()
    require(args.repeats > 0, 'at least one complete battery repetition is required')
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for repetition in range(1, args.repeats + 1):
        if not args.idle:
            rc = execute(['taskset', '-c', '24-31', str(binary.parent / 'rehash-waits-unit'),
                          'retirement'], output / ('retirement-%d.log' % repetition), 60)
            results.append(dict(row='retirement', repetition=repetition, rc=rc))
        for mode, local in [('split', 0), ('split', 1), ('fused', 0), ('fused', 1)]:
            tag = '%s-%d-%d' % (mode, local, repetition)
            port = 8540 + (0 if mode == 'split' else 2) + local
            with socket.socket() as guard:
                guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                guard.bind(('127.0.0.1', port))
            data = output / ('data-' + tag)
            data.mkdir()
            log = output / ('server-' + tag + '.log')
            cmd = ['taskset', '-c', '8-15', str(binary), '--bind', '127.0.0.1',
                   '--port', str(port), '--shards', '16', '--dir', str(data),
                   '--atomic', '1', '--read-local', str(local), '--overlap', '0',
                   '--flip-auto', '0', '--enable-debug-command', 'yes']
            cmd += ['--ratio', '6:2'] if mode == 'split' else ['--thread-mode', 'fused']
            srv = None
            try:
                with log.open('w') as out:
                    srv = subprocess.Popen(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT)
                print('BOOT', tag, 'pid', srv.pid, 'port', port, flush=True)
                deadline = time.monotonic() + 30
                while True:
                    if srv.poll() is not None:
                        raise RuntimeError('server exited: ' + log.read_text()[-3000:])
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(.1)
                battery_log = output / ('battery-' + tag + '.log')
                if args.idle:
                    rc = 0
                    with battery_log.open('w') as out, contextlib.redirect_stdout(out):
                        try:
                            idle_battery(binary, srv.pid, port, mode, local)
                        except Exception:
                            import traceback
                            traceback.print_exc(file=out)
                            rc = 1
                    print(battery_log.name, 'PASS' if rc == 0 else 'FAIL', flush=True)
                else:
                    rc = execute(['taskset', '-c', '16-23', 'python3', 'tests/rehash_readonly.py',
                                  '127.0.0.1', str(port), mode, str(local)], battery_log, 60)
            finally:
                if srv is not None:
                    if srv.poll() is None:
                        srv.terminate()
                    try:
                        srv.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        srv.kill()
                        srv.wait()
                        raise
                    print('STOP', srv.pid, 'exit', srv.returncode, flush=True)
            clean = execute(['taskset', '-c', '24-31', 'python3', 'tests/shutdown_report.py',
                             str(log), 'clean'], output / ('shutdown-' + tag + '.log'), 10)
            results.append(dict(row=mode, local=local, repetition=repetition,
                                rc=rc, shutdown=clean, server_exit=srv.returncode))
            (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2), flush=True)
    return int(any(row['rc'] or row.get('shutdown', 0) or row.get('server_exit', 0)
                   for row in results))

if __name__ == '__main__':
    sys.exit(main())
