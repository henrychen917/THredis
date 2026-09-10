#!/usr/bin/env python3
"""Same seeded RESP byte stream, same replies under every supported execution/knob combination.

The 32 cells are mode x read-local x overlap x reorder x atomic; flip-auto is explicitly off
because fused+flip-auto is a documented refusal, already covered by feature_gate. One connection
carries the seeded p32 stream, so cross-client order cannot make a legal schedule look divergent.
Fresh process/state per cell; no byte normalization. A separate disjoint-client long/short mix
uses the existing feature witnesses to prove enabled lanes actually fired, with their unchanged
bounded rearming and disabled-allocation controls. This helper adds no gate ledger row by itself.

Clock/expiry replies, random-member commands, hash/set iteration order, SCAN cursors, INFO and
connection IDs are intentionally outside the exact-byte stream. The existing Redis differential
property suites remain their oracle. This test claims equivalence only for its recorded stream;
it neither replaces those suites nor pretends that single-connection traffic proves all possible
cross-client schedules equivalent.
"""
import argparse
import contextlib
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import random
import socket
import sys
import time

from _gate_process import (cpus, encode, info, install_signals, pin_driver, require, server)
from feature_gate import check_config, smoke
from _differ_history import default_history, failing_seeds, record_leg

ROOT = Path(__file__).resolve().parents[1]
CELLS = [f'{mode}-{r}-{o}-{q}-{a}' for mode in ('1s', '2s')
         for r, o, q, a in itertools.product((0, 1), repeat=4)]
EXCLUDED = ['clock/expiry-derived replies', 'random-member commands', 'unordered hash/set iteration',
            'SCAN cursors', 'INFO telemetry', 'connection/process identifiers']


def command_stream(seed, rounds=800):
    rng = random.Random(seed)
    keys = [f'eq:k:{index}' for index in range(64)]
    operations = [['MSET', *[part for key in keys for part in (key, '0')]],
                  ['SET', 'eq:bitmap', b'\xff' * 65536]]
    for index in range(8):
        operations.extend([['RPUSH', f'eq:l:{index}', 'initial'],
                           ['HSET', f'eq:h:{index}', 'n', '0'],
                           ['SADD', f'eq:s:{index}', 'initial'],
                           ['ZADD', f'eq:z:{index}', '0', 'initial']])
    for iteration in range(rounds):
        key, other = rng.sample(keys, 2)
        selected = rng.sample(keys, 8)
        value = str(rng.randrange(100000))
        operations.extend([['SET', key, value], ['GET', key],
                           ['INCR', other], ['MGET', *selected]])
        choice, index = rng.randrange(8), rng.randrange(8)
        if choice == 0:
            pairs = [part for candidate in selected for part in (candidate, value)]
            operations.extend([['MSET', *pairs], ['MGET', *selected]])
        elif choice == 1:
            operations.extend([['MULTI'], ['SET', key, value], ['GET', key],
                               ['INCR', other], ['EXEC'], ['MGET', key, other]])
        elif choice == 2:
            operations.extend([['LPUSH', f'eq:l:{index}', value],
                               ['LTRIM', f'eq:l:{index}', '0', '15'],
                               ['LRANGE', f'eq:l:{index}', '0', '-1']])
        elif choice == 3:
            operations.extend([['HINCRBY', f'eq:h:{index}', 'n', '1'],
                               ['HSET', f'eq:h:{index}', 'value', value],
                               ['HMGET', f'eq:h:{index}', 'n', 'value', 'absent']])
        elif choice == 4:
            operations.extend([['SADD', f'eq:s:{index}', value],
                               ['SISMEMBER', f'eq:s:{index}', value],
                               ['SCARD', f'eq:s:{index}'],
                               ['SREM', f'eq:s:{index}', value]])
        elif choice == 5:
            operations.extend([['ZADD', f'eq:z:{index}', str(iteration), value],
                               ['ZRANK', f'eq:z:{index}', value],
                               ['ZRANGE', f'eq:z:{index}', '-4', '-1', 'WITHSCORES']])
        elif choice == 6:
            script = "local a=redis.call('GET',KEYS[1]); local b=redis.call('GET',KEYS[2]); return {a,b}"
            operations.extend([['EVAL', script, '2', key, other], ['BITCOUNT', 'eq:bitmap']])
        else:
            operations.extend([['GET', f'eq:l:{index}'], ['SET', key, value, 'NX', 'XX'],
                               ['GETRANGE', key, '-4', '-1'], ['EXISTS', key, other],
                               ['PING'], ['ECHO', b'binary\x00\xffvalue']])
    # Pure reads at the end also make it impossible for all comparisons to consist only of write
    # acknowledgements. Include every state family modified above, in a deterministic order.
    operations.extend([['MGET', *keys], ['DBSIZE']])
    for index in range(8):
        operations.extend([['LRANGE', f'eq:l:{index}', '0', '-1'],
                           ['HMGET', f'eq:h:{index}', 'n', 'value'],
                           ['SCARD', f'eq:s:{index}'],
                           ['ZRANGE', f'eq:z:{index}', '0', '-1', 'WITHSCORES']])
    return operations


def read_raw(file):
    line = file.readline()
    require(bool(line) and line.endswith(b'\r\n'), 'truncated RESP reply line')
    marker = line[:1]
    if marker in (b'+', b'-', b':'):
        return line
    require(marker in (b'$', b'*'), f'unexpected RESP2 marker: {marker!r}')
    size = int(line[1:-2])
    require(-1 <= size <= 64 * 1024 * 1024, f'invalid RESP length: {size}')
    if size == -1:
        return line
    if marker == b'$':
        payload = file.read(size + 2)
        require(len(payload) == size + 2 and payload.endswith(b'\r\n'), 'truncated RESP bulk')
        return line + payload
    return line + b''.join(read_raw(file) for _ in range(size))


def replay(sock, file, operations, expected=None, pipeline=32):
    transcript = []
    for first in range(0, len(operations), pipeline):
        chunk = operations[first:first + pipeline]
        sock.sendall(b''.join(encode(*operation) for operation in chunk))
        for offset, operation in enumerate(chunk):
            reply = read_raw(file)
            index = first + offset
            if expected is not None and reply != expected[index]:
                raise AssertionError(f'byte divergence at op {index} {operation[:4]!r}: '
                                     f'baseline={expected[index][:256]!r} actual={reply[:256]!r}')
            transcript.append(reply)
    require(len(transcript) == len(operations), 'comparison stream lost a reply')
    return transcript


def configuration(cell):
    mode, read_local, overlap, reorder, atomic = cell.split('-')
    knobs = dict(zip(('read-local', 'overlap', 'reorder', 'atomic'),
                     map(int, (read_local, overlap, reorder, atomic))))
    knobs.update({'thread-mode': mode, 'shards': 16, 'flip-auto': 0, 'key-lb': 0, 'client-lb': 0})
    if mode == '2s':
        knobs['ratio'] = '6:2'
    return knobs, [part for name, value in knobs.items() for part in (f'--{name}', str(value))]


def validate_geometry(server_cpus, load_cpus):
    servers, loads = set(cpus(server_cpus)), set(cpus(load_cpus))
    require(len(servers) == 8, 'equivalence fixture requires eight server CPUs and split ratio 6:2')
    require((servers | loads) <= set(os.sched_getaffinity(0)), 'requested CPUs escape process affinity')
    for cpu in servers:
        siblings = set(cpus(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text().strip()))
        require(not siblings & loads, 'server/load CPUs share a physical core (including SMT siblings)')
        require(len(siblings & servers) == 1, 'server CPUs must name distinct physical cores')


def self_test():
    a, b, other = command_stream(7, 20), command_stream(7, 20), command_stream(19, 20)
    require(a == b and a != other, 'seed determinism/rotation control')
    require(len(CELLS) == len(set(CELLS)) == 32, 'matrix lost a combination')
    require({configuration(cell)[0]['atomic'] for cell in CELLS} == {0, 1}, 'atomic axis missing')
    class Sink:
        def __init__(self): self.sends = []
        def sendall(self, payload): self.sends.append(payload)
    operations = [['GET', 'a'], ['MGET', 'a', 'b'], ['SET', 'a', 'v']]
    raw = b'$1\r\nx\r\n*2\r\n$1\r\nx\r\n$-1\r\n+OK\r\n'
    sink = Sink()
    baseline = replay(sink, io.BytesIO(raw), operations, pipeline=2)
    require(len(sink.sends) == 2 and len(baseline) == 3, 'real replay loop/pipeline controls')
    replay(Sink(), io.BytesIO(raw), operations, baseline)
    try:
        replay(Sink(), io.BytesIO(raw.replace(b'x\r\n', b'y\r\n', 1)), operations, baseline)
    except AssertionError as exc:
        require('byte divergence at op 0' in str(exc), 'wrong negative control failure')
    else:
        raise AssertionError('changed reply survived the real replay comparator')
    print('MODE EQUIVALENCE self-test: seed/matrix/raw replay/divergence controls passed; no servers')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build/tomokv')
    parser.add_argument('--server-cpus', default='0-7')
    parser.add_argument('--load-cpus', default='8-15')
    parser.add_argument('--port', type=int, default=8950)
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--_single-seed', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    require(args.output is not None, '--output is required')
    if not args._single_seed:
        seeds = list(dict.fromkeys([args.seed, *failing_seeds(default_history(), 'mode-equivalence')]))
        if len(seeds) > 1:
            args.output.mkdir(parents=True, exist_ok=False)
            affinity = os.sched_getaffinity(0)
            failed = 0
            try:
                for seed in seeds:
                    os.sched_setaffinity(0, affinity)
                    failed += main(['--_single-seed', '--binary', args.binary,
                                    '--server-cpus', args.server_cpus, '--load-cpus', args.load_cpus,
                                    '--port', str(args.port), '--seed', str(seed),
                                    '--output', str(args.output / f'seed-{seed}')])
            finally:
                os.sched_setaffinity(0, affinity)
            print(f'MODE EQUIVALENCE CORPUS: {len(seeds)} permanent streams, {len(seeds) * 32} cells, '
                  f'{failed} failing streams')
            return int(failed != 0)
    validate_geometry(args.server_cpus, args.load_cpus)
    pin_driver(args.server_cpus, args.load_cpus)
    install_signals()
    args.output.mkdir(parents=True, exist_ok=False)
    operations = command_stream(args.seed)
    stream_hash = hashlib.sha256(b''.join(encode(*operation) for operation in operations)).hexdigest()
    manifest = dict(seed=args.seed, cells=CELLS, operation_count=len(operations), pipeline=32,
                    stream_sha256=stream_hash, exact_byte_exclusions=EXCLUDED,
                    commands=sorted({operation[0] for operation in operations}))
    (args.output / 'stream.json').write_text(json.dumps(manifest, indent=2) + '\n')
    baseline, baseline_cell = None, None
    reports = []
    for cell in CELLS:
        start = time.monotonic()
        knobs, argv = configuration(cell)
        report = dict(cell=cell, knobs=knobs, verdict='FAIL', reached=False)
        try:
            with server(args.binary, args.server_cpus, args.port, args.output / cell, argv) as (admin, _process):
                actual = info(admin, 'SERVER', 'STATS', 'FLIPCTL')
                check_config(actual, knobs, cell, args.server_cpus)
                with contextlib.closing(socket.create_connection(('127.0.0.1', args.port), timeout=30)) as sock:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    with sock.makefile('rb') as file:
                        report['reached'] = True
                        transcript = replay(sock, file, operations, baseline)
                report['reply_sha256'] = hashlib.sha256(b''.join(transcript)).hexdigest()
                # Existing witness assertions are reused verbatim, including three fresh-state
                # arming attempts and exact zero-allocation checks for disabled features.
                report['witnesses'] = smoke(admin, args.port, knobs)
            if baseline is None:
                baseline, baseline_cell = transcript, cell
                (args.output / 'baseline.resp').write_bytes(b''.join(transcript))
            report.update(verdict='ok', compared_replies=len(transcript), baseline=baseline_cell)
        except Exception as exc:
            report['reason'] = str(exc)
        report['seconds'] = time.monotonic() - start
        reports.append(report)
        directory = args.output / cell
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        (args.output / 'results.json').write_text(json.dumps(reports, indent=2) + '\n')
        # A caller may supply a newly rotating seed. Retain its counterexample in the same durable
        # replay corpus as Redis differential failures; never silently lose it on the next run.
        record_leg(default_history(), argparse.Namespace(
            run=os.environ.get('GATE_RUN_ID', str(args.output.resolve())), seed=args.seed,
            suite='mode-equivalence', geometry=cell, atomic=knobs['atomic'],
            verdict=report['verdict'], log=directory / 'result.json'))
        print(f'MODE EQUIVALENCE {cell} {report["verdict"]} {report["seconds"]:.2f}s '
              f'{report.get("reason", str(len(operations)) + " byte-identical replies; mechanisms witnessed")}', flush=True)
    failed = sum(report['verdict'] != 'ok' for report in reports)
    print(f'MODE EQUIVALENCE: {len(reports) - failed}/32 cells, seed={args.seed}, '
          f'{len(manifest["commands"])} command types, {len(operations)} replies per cell')
    return int(failed != 0)


if __name__ == '__main__':
    sys.exit(main())
