#!/usr/bin/env python3
"""Mandatory loopback regression cells. See GATES.md before collecting/reviewing a pin.

check reads a pin and never writes it. measure writes raw experiments to a NEW directory.
candidate validates a complete null experiment and writes a NEW candidate; installing it is
an explicit maintainer action. Missing/unarmed references exit 3, never 0.
"""
import argparse
from contextlib import ExitStack
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

from _gate_process import (cpus, cpu_spec, delta, encode, fields, info, install_signals, number,
                           parse_lb, pin_driver, populate, require, server, stop_process, thread_readers)


CELLS = [f'{cmd}-p{pipe}-{mode}-rl{rl}' for cmd, pipe, mode, rl in
         itertools.product(('GET', 'SET', 'MGET', 'MSET'), (1, 32), ('1s', '2s'), (0, 1))]
SCHEMA = 1
SATURATION = .98
GENERATOR_CPU_LIMIT = .90
MAX_NULL_SPREAD = .02  # Box law supplied by the maintainer: >2% is contention/a defect, not noise.


def decode_cell(cell):
    cmd, pipe, mode, rl = cell.split('-')
    return cmd, int(pipe[1:]), mode, int(rl[2:])


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def load_groups(cpu_list, instances):
    selected = set(cpus(cpu_list))
    groups = {}
    for cpu in sorted(selected):
        siblings = set(cpus(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list')
                            .read_text().strip()))
        groups.setdefault(min(siblings), set()).add(cpu)
    physical = list(groups.values())
    require(len(physical) >= instances, 'fewer physical load cores than instances')
    # Contiguous physical-core groups keep every SMT pair in the same memtier process.
    return [cpu_spec(set().union(*physical[i * len(physical) // instances:
                                          (i + 1) * len(physical) // instances]))
            for i in range(instances)]


def machine(args):
    selected = set(cpus(args.load_cpus))
    siblings = {cpu: cpus(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list')
                          .read_text().strip()) for cpu in selected}
    cpu_model = next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text()
                     .splitlines() if line.startswith('model name'))
    return dict(kernel=platform.release(), cpu_model=cpu_model,
                server_cpus=cpu_spec(cpus(args.server_cpus)), load_cpus=cpu_spec(selected),
                smt_complete=all(len(s) > 1 and set(s) <= selected for s in siblings.values()),
                memtier_sha256=digest(shutil.which(args.memtier)), ratio=args.ratio,
                shards=16, keymax=args.keymax, value_bytes=64, multi_keys=4,
                window_seconds=args.window, warmup_seconds=args.warmup,
                max_instances=args.max_instances, workers_per_cpu=1,
                p1_connections=len(selected) * 2, p32_connections=len(selected) * 8,
                knobs={'atomic': 1, 'flip-auto': 0, 'overlap': 0, 'reorder': 0,
                       'key-lb': 0, 'client-lb': 0})


def cpu_ticks(process):
    result = {}
    for path in Path(f'/proc/{process.pid}/task').glob('*/stat'):
        v = path.read_text().rsplit(')', 1)[1].split()
        result[path.parent.name] = int(v[11]) + int(v[12])
    require(result, f'load generator {process.pid} exited before the measurement ended')
    return result


def snapshot(conn):
    start = time.monotonic_ns()
    conn.raw(encode('DEBUG', 'LBSIGNALS') + encode('INFO', 'ALL') + encode('DBSIZE'))
    lb = parse_lb(conn.read())
    row = fields(conn.read())
    size = conn.read()
    finish = time.monotonic_ns()
    return dict(lb=lb, info=row, dbsize=size, midpoint_ns=(start + finish) // 2,
                collection_ns=finish - start)


def calls(row, command):
    value = row.get('cmdstat_' + command.lower())
    require(value is not None, f'{command} did not execute / missing COMMANDSTATS')
    return int(dict(item.split('=', 1) for item in value.split(','))['calls'])


def summarize_window(before, after, cell, keymax):
    command, pipeline, mode, local = decode_cell(cell)
    require(before['dbsize'] == after['dbsize'] == keymax, 'DBSIZE != keymax across cell')
    b, a = before['info'], after['info']
    for key, expected in (('thread_mode', mode), ('read_local', str(local)), ('atomic', '1'),
                          ('flip_auto', '0'), ('overlap', '0'), ('reorder', '0')):
        require(b.get(key) == a.get(key) == expected, f'cell effective {key} mismatch')
    require(number(a, 'keyspace_misses') == number(b, 'keyspace_misses') == 0,
            'read miss path measured: keyspace_misses != 0')
    for key in ('rejected_connections', 'send_errors', 'peer_aborts'):
        require(delta(b, a, key) == 0, f'{key} during measurement')
    reads = command in ('GET', 'MGET')
    get_hits = delta(b, a, 'read_local_hits')
    mget_hits = delta(b, a, 'read_local_mget_local_hits')
    if local and reads:
        require((get_hits if command == 'GET' else mget_hits) > 0,
                f'{command} read-local enabled but hit counter never fired')
    else:
        require(get_hits == mget_hits == 0, 'unexpected read-local hits in off/write cell')
    # JSON object keys are strings when an archived window is replayed for validation.
    before_threads = {int(tid): row for tid, row in before['lb']['threads'].items()}
    after_threads = {int(tid): row for tid, row in after['lb']['threads'].items()}
    require(set(before_threads) == set(after_threads), 'thread inventory changed during cell')
    readers = thread_readers(a)
    if local:
        require(number(a, 'read_local_active_threads') > 0, 'read-local silently disarmed')
        require(set(readers) == set(thread_readers(b)) == set(after_threads),
                'read-local thread inventory incomplete')
        require(number(a, 'read_local_active_threads') == sum(int(t['active']) for t in readers.values()),
                'read-local active count disagrees with reader threads')
        for tid, row in readers.items():
            expect = row['role'] in ('ifid', 'unified')
            require(int(row['active']) == int(expect), f'reader {tid} role/active mismatch')
            if not expect:
                require(int(row['hits_total']) == int(row['mget_hits_total']) == 0,
                        f'nonreader {tid} accumulated hits')
            elif reads:
                key = 'hits_total' if command == 'GET' else 'mget_hits_total'
                prior = thread_readers(b)[tid]
                require(int(row[key]) > int(prior[key]), f'active reader {tid} had no {command} hits')
    executing_role = 'fused' if mode == '1s' else 'io' if local and reads else 'ex'
    busy, executing = {}, []
    for tid, row in after_threads.items():
        prior = before_threads[tid]
        require(row['role'] == prior['role'], 'role changed during fixed-placement measurement')
        db, di = row['busy_ns'] - prior['busy_ns'], row['idle_ns'] - prior['idle_ns']
        require(db >= 0 and di >= 0, f'thread {tid}: busy/idle counter reset')
        require(db + di > 0, f'thread {tid}: busy/idle accounting did not advance')
        busy[tid] = dict(role=row['role'], busy_ns=db, idle_ns=di, busy_fraction=db / (db + di))
        if row['role'] == executing_role:
            require(row['ops'] > prior['ops'], f'executing thread {tid} did no work')
            executing.append(tid)
    require(executing, 'no executing threads identified')
    minimum = min(busy[tid]['busy_fraction'] for tid in executing)
    wall_ns = after['midpoint_ns'] - before['midpoint_ns']
    require(wall_ns > 0, 'nonpositive measurement interval')
    count = calls(a, command) - calls(b, command)
    require(count > 0, 'server command counter did not advance')
    skew = (before['collection_ns'] + after['collection_ns']) / wall_ns
    require(skew < .002, f'telemetry window uncertainty {skew:.3%} exceeds 0.2%; lengthen window')
    return dict(rate=count * 1e9 / wall_ns, commands=count, wall_seconds=wall_ns / 1e9,
                timing_uncertainty=skew, dbsize=after['dbsize'], read_local_hits=get_hits,
                read_local_mget_hits=mget_hits, executing_threads=executing,
                minimum_busy=minimum, threads=busy,
                saturated=minimum >= SATURATION,
                measurement='capacity' if pipeline == 32 else 'latency/round-trip')


def memtier_args(args, cell, instances, index, directory):
    command, pipeline, _, _ = decode_cell(cell)
    clients = 2 if pipeline == 1 else 8
    connections = len(cpus(args.load_cpus)) * clients
    groups = load_groups(args.load_cpus, instances)
    argv = ['taskset', '-c', groups[index], args.memtier, '-s', '127.0.0.1', '-p', str(args.port),
            '-t', str(len(cpus(groups[index]))), '-c', str(clients), '--pipeline=' + str(pipeline),
            '--key-prefix=gate-', '--key-minimum=1', '--key-maximum=' + str(args.keymax),
            '--data-size=64', '--distinct-client-seed', '--hide-histogram',
            '--test-time=' + str(math.ceil(args.warmup + args.window + 2)),
            '--json-out-file=' + str(directory / f'load-{index}.json')]
    if command in ('GET', 'SET'):
        argv += ['--ratio=' + ('0:1' if command == 'GET' else '1:0'), '--key-pattern=R:R']
    else:
        # __key__ expands to the COMPLETE generated key, including --key-prefix.
        keys = ['__key__'] * 4
        words = keys if command == 'MGET' else [word for key in keys for word in (key, '__data__')]
        argv += ['--command=' + ' '.join([command, *words]), '--command-key-pattern=R']
    return argv, connections


def trial(args, cell, instances, tag):
    directory = (Path(args.output) / cell / tag).resolve()
    command, pipeline, mode, local = decode_cell(cell)
    knobs = ['--thread-mode', mode, '--shards', '16', '--read-local', str(local)]
    for key, value in machine(args)['knobs'].items():
        knobs += ['--' + key, str(value)]
    if mode == '2s':
        knobs += ['--ratio', args.ratio]
    started = time.monotonic()
    binary_sha256 = digest(args.binary)
    with server(args.binary, args.server_cpus, args.port, directory, knobs) as (conn, srv):
        populate(conn, args.keymax)
        # A write cell has zero reads in its timed window. Probe the armed lane beforehand so
        # those zero deltas cannot conceal a disabled feature; retain the separate proof.
        for _ in range(4):
            require(conn.must('GET', 'gate-1') == b'x' * 64, 'populated GET probe failed')
            require(conn.must('MGET', 'gate-1', 'gate-2') == [b'x' * 64] * 2, 'MGET probe failed')
        probe = info(conn, 'STATS')
        if local:
            require(number(probe, 'read_local_hits') > 0 and
                    number(probe, 'read_local_mget_local_hits') > 0, 'read-local preflight never fired')
        processes = []
        with ExitStack() as stack:
            for index in range(instances):
                log = stack.enter_context((directory / f'load-{index}.log').open('w'))
                argv, connections = memtier_args(args, cell, instances, index, directory)
                (directory / f'load-{index}-argv.json').write_text(json.dumps(argv, indent=2) + '\n')
                process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
                processes.append(process)
                stack.callback(stop_process, process)
            deadline = time.monotonic() + 8
            while True:
                require(all(p.poll() is None for p in processes), 'load generator died during startup')
                clients = number(info(conn, 'CLIENTS'), 'connected_clients')
                if clients == connections + 1:
                    break
                require(time.monotonic() < deadline, f'load did not connect {connections} clients; got {clients}')
                time.sleep(.02)
            time.sleep(args.warmup)
            cpu_before = [cpu_ticks(p) for p in processes]
            before = snapshot(conn)
            time.sleep(args.window)
            after = snapshot(conn)
            cpu_after = [cpu_ticks(p) for p in processes]
            (directory / 'telemetry.json').write_text(json.dumps(dict(before=before, after=after), indent=2) + '\n')
            require(number(before['info'], 'connected_clients') ==
                    number(after['info'], 'connected_clients') == connections + 1,
                    'connections changed in the measurement window')
            result = summarize_window(before, after, cell, args.keymax)
            workers = []
            hz = os.sysconf('SC_CLK_TCK')
            for b, a in zip(cpu_before, cpu_after):
                require(set(a) == set(b), 'generator thread inventory changed during measurement')
                workers += [(a[tid] - b[tid]) / hz / result['wall_seconds'] for tid in a]
            result['generator_max_cpu'] = max(workers)
            if pipeline == 1:
                # Fixed closed-loop concurrency, all clients connected, every generator thread
                # retains >=10% CPU headroom. C/rate is the mean completed round-trip interval,
                # including client dispatch. It is not a server capacity measurement.
                p1_validity(result, connections)
            client_count, client_ms = 0, 0
            for index, p in enumerate(processes):
                require(p.wait(timeout=15) == 0, f'load generator {index} failed')
                raw = json.loads((directory / f'load-{index}.json').read_text())
                totals = raw['ALL STATS']['Totals']
                require(totals['Count'] > 0 and totals['Connection Errors'] == 0,
                        f'generator {index}: no replies or connection errors')
                require(totals.get('Misses/sec') == 0, f'generator {index}: misses or missing miss telemetry')
                require(raw['ALL STATS']['Runtime']['Interrupted'] == 'false', 'generator was interrupted')
                log_text = (directory / f'load-{index}.log').read_text()
                require(not re.search(r'(?im)(error response|^error[: ]|failed|protocol error)', log_text),
                        f'generator {index}: error response; see its log')
                client_count += totals['Count']
                client_ms += totals['Count'] * totals['Average Latency']
            result.update(cell=cell, binary_sha256=binary_sha256,
                          instances=instances, load_groups=load_groups(args.load_cpus, instances),
                          connections=connections, read_local_probe_hits=number(probe, 'read_local_hits'),
                          read_local_probe_mget_hits=number(probe, 'read_local_mget_local_hits'),
                          client_mean_latency_ms=client_ms / client_count,
                          before=before, after=after)
    result['seconds'] = time.monotonic() - started
    require(digest(args.binary) == binary_sha256, 'binary changed during measurement')
    (directory / 'window.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PERF {cell} n={instances} {result["rate"]:.0f} commands/s '
          f'min_busy={result["minimum_busy"]:.3%} dbsize={result["dbsize"]} '
          f'rl_get={result["read_local_hits"]} rl_mget={result["read_local_mget_hits"]} '
          f'{result["measurement"]}', flush=True)
    return {key: value for key, value in result.items() if key not in ('before', 'after')}


def p1_validity(result, connections):
    require(generator_headroom(result), 'p1 invalid: load generator has no CPU headroom')
    result['round_trip_us'] = connections * 1e6 / result['rate']


def generator_headroom(sample):
    value = sample['generator_max_cpu']
    return math.isfinite(value) and 0 <= value < GENERATOR_CPU_LIMIT


def null_statistics(samples):
    require(len(samples) >= 12 and len(samples) % 2 == 0,
            'pin requires at least six adjacent A/A pairs per cell')
    require(len({sample['instances'] for sample in samples}) == 1, 'null comparison changed generator count')
    require(len({s['binary_sha256'] for s in samples}) == 1, 'null comparison used different binaries')
    require(all(math.isfinite(s['rate']) and s['rate'] > 0 and
                math.isfinite(s['timing_uncertainty']) and 0 <= s['timing_uncertainty'] < .002
                for s in samples), 'invalid null rate/timing uncertainty')
    logs = [abs(math.log(b['rate'] / a['rate'])) for a, b in zip(samples[::2], samples[1::2])]
    # Include NON-adjacent windows: an adjacent A/A pair can hide a slow drift that moves both
    # arms together, whereas the eventual gate compares a future run against a historical pin.
    # Worst observed bidirectional null difference + both timestamp uncertainties. No multiplier,
    # automatic relaxation, outlier removal, or clipping at the box's 2% validity ceiling.
    differences = [(abs(math.log(b['rate'] / a['rate'])) + a['timing_uncertainty'] + b['timing_uncertainty'], i, j)
                   for i, a in enumerate(samples) for j, b in enumerate(samples) if i < j]
    bound_log, worst_a, worst_b = max(differences)
    bound = 1 - math.exp(-bound_log)
    return dict(loss_fraction=bound, log_bound=bound_log, adjacent_pair_abs_log_differences=logs,
                pairs=len(logs), worst_pair=[worst_a, worst_b],
                formula='max over all i<j(abs(log(rate_j/rate_i)) + uncertainty_i + uncertainty_j)')


def null_bound(samples):
    bound = null_statistics(samples)
    require(bound['loss_fraction'] <= MAX_NULL_SPREAD,
            f'null spread {bound["loss_fraction"]:.3%} exceeds the 2% box law; '
            'investigate contention/defect, do not widen')
    return bound


def capacity(sample):
    require(math.isfinite(sample['minimum_busy']) and 0 <= sample['minimum_busy'] <= 1,
            'invalid busy fraction')
    if decode_cell(sample['cell'])[1] == 32:
        require(sample['minimum_busy'] >= SATURATION,
                f'could not saturate {sample["cell"]}: executing-thread minimum busy '
                f'{sample["minimum_busy"]:.3%} < 98%, instances={sample["instances"]}')
    require(generator_headroom(sample),
            f'could not qualify {sample["cell"]}: load generator has no CPU headroom')


def compare_rate(cell, rate, reference):
    require(math.isfinite(rate) and rate > 0, 'invalid measured rate')
    loss_log = math.log(reference['rate'] / rate)
    require(loss_log <= reference['bound']['log_bound'],
            f'regression {cell}: {rate:.0f} vs pinned {reference["rate"]:.0f} '
            f'({rate / reference["rate"] - 1:+.3%}); bound -{reference["bound"]["loss_fraction"]:.3%}')


def load_reference(path):
    if not Path(path).is_file():
        print(f'SKIP LOUDLY: performance tier UNARMED: reference file absent: {path}', flush=True)
        raise SystemExit(3)
    refs = json.loads(Path(path).read_text())
    require(refs.get('schema') == SCHEMA, 'unsupported performance reference schema')
    if refs.get('armed') is not True:
        print(f'SKIP LOUDLY: performance tier UNARMED: {path}: {refs.get("reason", "no reviewed pin")}', flush=True)
        raise SystemExit(3)
    require(set(refs['cells']) == set(CELLS), 'pinned reference is incomplete/contains extra cells')
    require(refs['provenance']['machine']['smt_complete'], 'pin lacks physical-core SMT siblings')
    for cell, row in refs['cells'].items():
        require(row['bound'] == null_bound(row['samples']), f'{cell}: bound was not derived from its null')
        require(row['rate'] == statistics.median(s['rate'] for s in row['samples']),
                f'{cell}: reference rate is not the measured median')
        require(row['instances'] == row['samples'][0]['instances'], f'{cell}: pinned instance mismatch')
        for sample in row['samples']:
            require(sample['cell'] == cell, 'reference sample mislabeled')
            require(sample['binary_sha256'] == refs['provenance']['binary_sha256'],
                    'reference sample came from a different binary')
            capacity(sample)
        if decode_cell(cell)[1] == 32:
            require(len(row['ladder']) >= 2, f'{cell}: missing generator plateau proof')
            for sample in row['ladder'][-2:]:
                capacity(sample)
            require(abs(math.log(row['ladder'][-1]['rate'] / row['ladder'][-2]['rate']))
                    <= row['bound']['log_bound'], f'{cell}: reference generator did not plateau')
    return refs


def collect(args, cell, reference=None):
    record = dict(cell=cell, ladder=[], samples=[])
    start = time.monotonic()
    try:
        selected = None
        # Always add an instance before claiming a plateau, even if the first rung is busy.
        # Apply this to EVERY arm, so whichever arm is fastest has its own capacity proof.
        for instances in range(1, args.max_instances + 1):
            sample = trial(args, cell, instances, f'ladder-{instances}')
            record['ladder'].append(sample)
            if reference and instances >= 2 and decode_cell(cell)[1] == 32:
                previous = record['ladder'][-2]
                gain = math.log(sample['rate'] / previous['rate'])
                if (sample['saturated'] and previous['saturated'] and
                        generator_headroom(sample) and generator_headroom(previous) and
                        abs(gain) <= reference['bound']['log_bound']):
                    selected = instances
                    break
        if not reference:
            selected = args.max_instances
        elif decode_cell(cell)[1] == 1:
            selected = args.max_instances  # fixed p1 generator/concurrency, no saturation assertion
        require(selected is not None, f'could not saturate / establish generator plateau for {cell}; '
                f'ladder exhausted at {args.max_instances} instances (not a passing rate)')
        if decode_cell(cell)[1] == 32:
            for sample in record['ladder'][-2:]:
                capacity(sample)
        repeats = 3 if reference else args.null_pairs * 2
        for repeat in range(repeats):
            sample = trial(args, cell, selected, f'repeat-{repeat:02}')
            capacity(sample)
            record['samples'].append(sample)
        if not reference:
            record['null_measurement'] = null_statistics(record['samples'])
        bound = reference['bound'] if reference else null_bound(record['samples'])
        if decode_cell(cell)[1] == 32:
            for sample in record['ladder'][-2:]:
                capacity(sample)
            gain = math.log(record['ladder'][-1]['rate'] / record['ladder'][-2]['rate'])
            require(abs(gain) <= bound['log_bound'],
                    f'generator plateau not established: last rung changed {math.expm1(gain):+.3%}; '
                    f'null bound {bound["loss_fraction"]:.3%}')
        rate = statistics.median(sample['rate'] for sample in record['samples'])
        record.update(rate=rate, bound=bound, instances=selected)
        if reference:
            compare_rate(cell, rate, reference)
        record['verdict'] = 'ok'
    except Exception as exc:
        record.update(verdict='FAIL', reason=str(exc))
    record['seconds'] = time.monotonic() - start
    output = Path(args.output) / cell
    output.mkdir(parents=True, exist_ok=True)
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'PERF RESULT {cell} {record["verdict"]} {record["seconds"]:.2f}s '
          f'{record.get("reason", "validated")}', flush=True)
    return record


def candidate(args):
    root = Path(args.measurements)
    metadata = json.loads((root / 'experiment.json').read_text())
    require(metadata['machine']['smt_complete'], 'cannot pin: experiment omitted physical-core SMT siblings')
    records = {cell: json.loads((root / cell / 'result.json').read_text()) for cell in CELLS}
    for cell, record in records.items():
        require(record['verdict'] == 'ok', f'cannot pin failing experiment {cell}: {record.get("reason")}')
        require(record['bound'] == null_bound(record['samples']), 'derived bound does not match raw null pairs')
        for sample in record['samples']:
            require(sample['binary_sha256'] == metadata['binary_sha256'],
                    'cannot pin: experiment changed binaries between samples')
            capacity(sample)
    pin = dict(schema=SCHEMA, armed=True, provenance=metadata,
               cells={cell: {key: row[key] for key in ('rate', 'bound', 'instances', 'samples', 'ladder')}
                      for cell, row in records.items()})
    with Path(args.output).open('x') as f:
        json.dump(pin, f, indent=2)
        f.write('\n')
    print(f'Candidate written to {args.output}; review and install manually. Existing pins were not touched.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('check', 'measure', 'candidate'))
    p.add_argument('--cell', choices=CELLS)
    p.add_argument('--binary', default='./build/tomokv')
    p.add_argument('--memtier', default='memtier_benchmark')
    p.add_argument('--refs', default='tests/gate_perf_refs.json')
    p.add_argument('--server-cpus', default='8-15')
    p.add_argument('--load-cpus', default='64-127,192-255')
    p.add_argument('--ratio', default='6:2')
    p.add_argument('--port', type=int, default=8621)
    p.add_argument('--keymax', type=int, default=200000)
    p.add_argument('--window', type=float, default=3)
    p.add_argument('--warmup', type=float, default=1)
    p.add_argument('--max-instances', type=int, choices=(2, 3, 4), default=4)
    p.add_argument('--null-pairs', type=int, default=6)
    p.add_argument('--measurements')
    p.add_argument('--output', required=True)
    args = p.parse_args()
    install_signals()
    if args.action == 'candidate':
        candidate(args)
        return 0
    refs = load_reference(args.refs) if args.action == 'check' else None
    pin_driver(args.server_cpus, args.load_cpus)
    require(args.window >= 1 and args.warmup >= .5 and args.keymax >= 1000, 'invalid measurement geometry')
    require(args.null_pairs >= 6, 'need at least six null pairs')
    metadata = dict(machine=machine(args), binary_sha256=digest(args.binary),
                    harness_sha256=digest(__file__), support_sha256=digest(Path(__file__).with_name('_gate_process.py')),
                    commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                    utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), command=sys.argv)
    if refs:
        require(metadata['machine'] == refs['provenance']['machine'],
                'machine/geometry/generator differs from pinned reference; maintainer must review a new pin')
    Path(args.output).mkdir(parents=True, exist_ok=False)
    (Path(args.output) / 'experiment.json').write_text(json.dumps(metadata, indent=2) + '\n')
    results = [collect(args, cell, refs['cells'][cell] if refs else None)
               for cell in ([args.cell] if args.cell else CELLS)]
    return 0 if all(r['verdict'] == 'ok' for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
