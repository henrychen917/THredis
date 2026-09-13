#!/usr/bin/env python3
"""Feature witnesses; each matrix cell is a mandatory boot, with ONE refusal class.

A fused server has no io/ex split, so a controller that rebalances that split has nothing to
actuate -- config.h refuses --flip-auto with --thread-mode 1s exactly as it refuses --ratio, for
the same stated reason ("every thread handles networking and execution"). The eight 1s cells with
flip-auto=1 therefore assert the REFUSAL and its documented message.

That is not an "expected rejection" escape hatch: the row FAILS if the server boots, so deleting
the guard turns these rows red, and it FAILS if the refusal cites a different reason. The
combination stays covered either way. Every other cell is still a mandatory boot.

gate.sh invokes --cell once per ledger row. --matrix runs just this tier for development.
No rates are scored. Independent connections carry pipelined work so reorder can permute.
"""
import argparse
from contextlib import ExitStack
import itertools
import json
import os
from pathlib import Path
import sys
import time

from _gate_process import (Conn, cpus, delta, encode, info, install_signals, lb_snapshot,
                           number, pin_driver, require, server, thread_readers)


MODES = ('1s', '2s')
SWITCHES = ('read-local', 'overlap', 'reorder', 'flip-auto')
MATRIX = [f'{mode}-{r}-{o}-{q}-{f}' for mode in MODES
          for r, o, q, f in itertools.product((0, 1), repeat=4)]
TOPOLOGY = ('split-home-min', 'fused-home-max-nopin', 'split-shards-auto')
# The refusal is part of the product surface, so its text is pinned here: a guard that starts
# rejecting for a different reason is a different guard.
FUSED_FLIP_REFUSAL = '--flip-auto is unavailable with --thread-mode 1s'


def refuses_boot(cell):
    """True for the cells the server must REJECT rather than boot."""
    if cell in TOPOLOGY:
        return False
    mode, _read_local, _overlap, _reorder, flip_auto = cell.split('-')
    return mode == '1s' and int(flip_auto) == 1
CELLS = MATRIX + list(TOPOLOGY)


def config(cell, cpu_list, ratio):
    topo = cell in TOPOLOGY
    if not topo:
        mode, r, o, q, f = cell.split('-')
        r, o, q, f = map(int, (r, o, q, f))
        # Each binary knob is explicit, including both independent LB controls and atomic.
        knobs = dict(zip(SWITCHES, (r, o, q, f)))
        knobs.update({'thread-mode': mode, 'atomic': r ^ o, 'key-lb': q, 'client-lb': o,
                      'shards': 16})
        if mode == '2s':
            knobs['ratio'] = ratio
    else:
        mode = '1s' if cell.startswith('fused') else '2s'
        knobs = {'thread-mode': mode, 'read-local': 1, 'overlap': 0, 'reorder': 0,
                 'flip-auto': 0, 'atomic': 1, 'key-lb': 0, 'client-lb': 0}
        if cell == 'split-shards-auto':
            knobs.update(shards=-1, ratio=ratio)
        else:
            # Explicit CPUs deliberately reverse the ordinary order. Dense tids, not CPU
            # numbers, name shard homes. The split row leaves several executors empty.
            selected = list(reversed(cpus(cpu_list)))
            require(len(selected) >= 4, 'topology rows need at least four server CPUs')
            roles = ['ifid'] * (len(selected) // 2) + ['ex'] * (len(selected) - len(selected) // 2)
            knobs['place'] = ','.join(f'{role}@{cpu}' for role, cpu in zip(roles, selected))
            knobs['shards'] = 1 if mode == '2s' else 256
            owners = [len(selected) - 1] if mode == '2s' else list(range(len(selected)))
            knobs['shard-home'] = ','.join(f'{sid}:{owners[-1 - sid % len(owners)]}'
                                         for sid in range(knobs['shards']))
    args = [part for k, v in knobs.items() for part in (f'--{k}', str(v))]
    if cell == 'fused-home-max-nopin':
        args.append('--no-pin')
    return knobs, args


def inventory(cpu_list, ratio):
    require(len(MATRIX) == 32 and len(set(MATRIX)) == 32, 'five-switch matrix lost a row')
    values = {}
    for cell in CELLS:
        knobs, argv = config(cell, cpu_list, ratio)
        for key, value in knobs.items():
            values.setdefault(key, set()).add(value)
        require(all(f'--{key}' in argv for key in ('thread-mode', *SWITCHES,
                                                  'atomic', 'key-lb', 'client-lb')),
                f'{cell}: inherited a feature default')
    for key in (*SWITCHES, 'atomic', 'key-lb', 'client-lb'):
        require(values[key] == {0, 1}, f'{key} no longer covers both values')
    require(values['thread-mode'] == set(MODES), 'thread-mode coverage')
    require(values['shards'] == {-1, 1, 16, 256}, 'shards grammar/boundary coverage')
    require(all(key in values for key in ('ratio', 'place', 'shard-home')), 'topology coverage')
    require('--no-pin' in config('fused-home-max-nopin', cpu_list, ratio)[1], 'no-pin coverage')


def check_config(row, knobs, cell, server_cpus):
    for key in ('thread-mode', *SWITCHES, 'atomic', 'key-lb', 'client-lb'):
        field = key.replace('-', '_')
        require(row.get(field) == str(knobs[key]),
                f'{key} requested {knobs[key]} but effective {field}={row.get(field)!r}')
    nshards = knobs['shards']
    if nshards == -1:
        nshards = min(8 * int(knobs['ratio'].split(':')[1]), 256)
    require(number(row, 'shards') == nshards, '--shards was not applied')
    if 'shard-home' in knobs:
        require(row.get('shard_home') == knobs['shard-home'], '--shard-home was not applied')
        require(row.get('shard_owners') == knobs['shard-home'], 'live owners differ from explicit homes')
    if 'place' in knobs:
        expected = ','.join(f'{tid}:{entry.split("@")[1]}'
                            for tid, entry in enumerate(knobs['place'].split(',')))
        require(row.get('thread_cpus') == expected, '--place was not applied')
    require(number(row, 'pin_threads') == int(cell != 'fused-home-max-nopin'), '--no-pin effective state')
    observed = {int(pair.split(':')[1]) for pair in row['thread_cpus'].split(',')}
    require(observed <= set(cpus(server_cpus)), 'server placement escaped assigned CPUs')
    if knobs['flip-auto']:
        require(row.get('flipctl_state') not in (None, 'unavailable', 'disabled'),
                'requested flip controller is unavailable/disabled')
    else:
        allowed = ('unavailable', 'disabled') if knobs['thread-mode'] == '1s' else ('disabled',)
        require(row.get('flipctl_state') in allowed, 'flip off still runs its controller')


def check_readers(before, after, enabled):
    b, a = thread_readers(before), thread_readers(after)
    require(set(a) == set(b), 'read-local thread inventory changed')
    if not enabled:
        require('read_local_active_threads' not in after and not a,
                'read-local off allocated/armed reader reporting state')
        for key in ('read_local_hits', 'read_local_mget_local_hits', 'read_local_arms',
                    'read_local_write_ring_sidecars', 'read_local_write_ring_records'):
            require(number(after, key) == 0, f'read-local off fired/allocated {key}')
        return
    active = number(after, 'read_local_active_threads')
    require(active > 0 and a, 'no active reader threads')
    require(active == sum(int(t['active']) for t in a.values()), 'active reader count disagrees')
    for tid, t in a.items():
        was = b[tid]
        eligible = t['role'] in ('ifid', 'unified')
        require(int(t['active']) == int(eligible), f'thread {tid}: role/reader mismatch {t}')
        # Lifetime totals can legitimately remain on a former reader after FLIP. Its delta
        # must be zero in a stable nonreader interval; fresh ex-only threads must stay at zero.
        for key in ('hits_total', 'mget_hits_total'):
            d = int(t[key]) - int(was[key])
            require(d >= 0, f'thread {tid}: {key} reset')
            if eligible:
                require(d > 0, f'active reader {tid} did not accumulate {key}')
            elif was['role'] == t['role'] and was['active'] == '0':
                require(d == 0 and int(was[key]) == 0,
                        f'nonreader {tid} accumulated {key}: {was} -> {t}')


def check_activity(before, after, knobs, nthreads, cross_owner):
    check_readers(before, after, knobs['read-local'])
    # O1 has one effective split schedule; fused --overlap 1 must be a literal no-op.
    overlap_enabled = knobs['overlap'] and knobs['thread-mode'] == '2s'
    require(after.get('overlap_enabled') == str(int(overlap_enabled)),
            'wrong effective overlap mode')
    expected_schedule = 'split-io-overlap' if overlap_enabled else 'plain'
    stats_on = overlap_enabled or knobs['reorder']
    require(after.get('overlap_schedule') == (expected_schedule if stats_on else None),
            'wrong overlap schedule/allocation')
    for field in ('overlap_passes', 'overlap_interleaved_passes'):
        if not stats_on:
            require(field not in after, 'disabled scheduling allocated counters')
            continue
        require((delta(before, after, field) > 0) if overlap_enabled
                else number(after, field) == 0, f'overlap witness {field} did not match knob')
    for field in ('reorder_batches', 'reorder_multi_client_runs', 'reorder_permuted_runs'):
        if not stats_on:
            require(field not in after, 'disabled scheduling allocated counters')
            continue
        require((delta(before, after, field) > 0) if knobs['reorder']
                else number(after, field) == 0, f'reorder witness {field} did not match knob')
    expect_stats = str(nthreads) if stats_on else None
    require(after.get('schedule_stats_threads') == expect_stats,
            'schedule counters allocated while both features off / missing when on')
    if cross_owner:
        require((delta(before, after, 'atomic_groups') > 0) if knobs['atomic']
                else number(after, 'atomic_groups') == 0, 'atomic groups did not match knob')
    if knobs['key-lb'] or knobs['client-lb']:
        require(delta(before, after, 'tomokv_keylb_ticks') > 0, 'LB controller did not tick')
    else:
        require(number(after, 'tomokv_keylb_ticks') == 0, 'LB off still ticks')
    # A controller tick alone does not prove either independent signal collector is alive.
    # The common hot bitmap and differently placed clients deliberately create nonzero weights.
    for knob, field in (('key-lb', 'tomokv_keylb_bucket_weight_spread_current'),
                        ('client-lb', 'tomokv_keylb_client_weight_spread_current')):
        require(field in after, f'missing LB signal witness {field}')
        weight = float(after[field])
        require(weight > 0 if knobs[knob] else weight == 0, f'{knob} signal collector did not match knob')
    if not knobs['key-lb']:
        require(number(after, 'tomokv_keylb_bucket_moves') == 0, 'key-lb off moved a bucket')
    if not knobs['client-lb']:
        require(number(after, 'tomokv_keylb_client_moves') == 0, 'client-lb off moved a client')
    if knobs['flip-auto']:
        require(delta(before, after, 'flipctl_forced_triggers') > 0,
                'enabled flip controller never consumed its trigger')
    elif after.get('flipctl_state') == 'unavailable':
        require('flipctl_triggers' not in after, 'unavailable flip controller exposed live counters')
    else:
        require(number(after, 'flipctl_triggers') == 0, 'disabled flip controller triggered')


def smoke(conn, port, knobs):
    # Fresh connections/state on every bounded arming attempt. BITCOUNT supplies actual longer
    # execution among short INCRs; reads alone would bypass reorder on the armed local lane.
    before = info(conn, 'SERVER', 'STATS', 'LB', 'FLIPCTL')
    baseline = lb_snapshot(conn)
    if knobs['flip-auto']:
        require(conn.must('DEBUG', 'FLIPCTL', 'TRIGGER') == b'OK', 'flip trigger rejected')
    last_error = None
    for attempt in range(3):
        with ExitStack() as stack:
            clients = []
            for _ in range(64):
                c = Conn('127.0.0.1', port, timeout=8)
                stack.callback(c.close)
                clients.append(c)
            bitmap = f'feature-bits-{attempt}'
            require(conn.must('SET', bitmap, b'\xff' * 65536) == b'OK', 'bitmap seed failed')
            keys = [f'feature-{attempt}-{i}' for i in range(len(clients))]
            for c, key in zip(clients, keys):
                require(c.must('SET', key, '0') == b'OK', 'counter seed failed')
            read_frames = [encode('GET', key) + encode('MGET', key, key) for key in keys]
            end = time.monotonic() + 1.1
            rounds = 0
            while rounds < 4 or time.monotonic() < end:
                for c, key in zip(clients, keys):
                    c.raw(encode('BITCOUNT', bitmap) + encode('INCR', key) * 31)
                for c in clients:
                    require(c.read() == 65536 * 8, 'wrong BITCOUNT/reply order')
                    for n in range(31):
                        require(c.read() == rounds * 31 + n + 1, 'INCR reply reordered/lost')
                rounds += 1
                for c, frame in zip(clients, read_frames):
                    c.raw(frame * 8)
                value = str(rounds * 31).encode()
                for c in clients:
                    for _ in range(8):
                        require(c.read() == value, 'GET violated RYOW')
                        require(c.read() == [value, value], 'MGET violated RYOW')
            # Several actual cross-owner groups, discovered from the live directory.
            located = conn.must('DEBUG', 'SHARDS', *keys)
            pair = next(((keys[0], k) for k, loc in zip(keys, located)
                         if loc[1] != located[0][1]), None)
            if len(set(baseline['shards'].values())) > 1:
                require(pair is not None, 'failed to discover cross-owner keys')
            if pair:
                for _ in range(8):
                    require(conn.must('MSET', pair[0], '7', pair[1], '7') == b'OK', 'MSET failed')
                    require(conn.must('MGET', *pair) == [b'7', b'7'], 'MGET tore')
            after = info(conn, 'SERVER', 'STATS', 'LB', 'FLIPCTL')
            try:
                check_activity(before, after, knobs, len(baseline['threads']), bool(pair))
                return dict(before=before, after=after, rounds=rounds, attempts=attempt + 1)
            except AssertionError as exc:
                last_error = exc
        # No skip or relaxed threshold: try a fresh connection distribution and fresh keys.
    raise AssertionError(f'feature did not fire after 3 fresh arming attempts: {last_error}')


def run_cell(args, cell):
    knobs, argv = config(cell, args.server_cpus, args.ratio)
    directory = Path(args.output) / cell
    start = time.monotonic()
    evidence = dict(cell=cell, requested=knobs)
    if refuses_boot(cell):
        evidence['contract'] = 'refusal'
        try:
            with server(args.binary, args.server_cpus, args.port, directory, argv):
                pass
            evidence.update(verdict='FAIL',
                            reason='server BOOTED with --flip-auto in 1s: the config guard is gone')
        except Exception as exc:
            reason = str(exc)
            if FUSED_FLIP_REFUSAL in reason:
                evidence.update(verdict='ok', reason=f'refused as documented: {FUSED_FLIP_REFUSAL}')
            else:
                evidence.update(verdict='FAIL',
                                reason=f'refused, but not for the documented reason: {reason}')
        evidence['seconds'] = time.monotonic() - start
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'result.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print(f'FEATURE {cell} {evidence["verdict"]} {evidence["seconds"]:.2f}s '
              f'{evidence["reason"]}', flush=True)
        return evidence['verdict'] == 'ok'
    try:
        with server(args.binary, args.server_cpus, args.port, directory, argv) as (conn, process):
            actual = info(conn, 'SERVER', 'STATS', 'FLIPCTL')
            check_config(actual, knobs, cell, args.server_cpus)
            masks = [sorted(os.sched_getaffinity(int(path.name)))
                     for path in Path(f'/proc/{process.pid}/task').iterdir()]
            allowed = set(cpus(args.server_cpus))
            require(all(set(mask) <= allowed for mask in masks), 'server task escaped assigned CPUs')
            if cell == 'fused-home-max-nopin':
                require(all(set(mask) == allowed for mask in masks), '--no-pin still pinned a task')
            else:
                selected = {int(pair.split(':')[1]) for pair in actual['thread_cpus'].split(',')}
                require(selected <= {mask[0] for mask in masks if len(mask) == 1},
                        'pin_threads=1 but worker CPU affinities are not installed')
            evidence['task_affinities'] = masks
            geometry = lb_snapshot(conn)
            if 'ratio' in knobs:
                actual_ratio = ':'.join(str(sum(t['role'] == role for t in geometry['threads'].values()))
                                        for role in ('io', 'ex'))
                require(actual_ratio == knobs['ratio'], '--ratio effective geometry mismatch')
            evidence['evidence'] = smoke(conn, args.port, knobs)
        evidence['verdict'] = 'ok'
    except Exception as exc:
        evidence.update(verdict='FAIL', reason=str(exc))
    evidence['seconds'] = time.monotonic() - start
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'result.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f'FEATURE {cell} {evidence["verdict"]} {evidence["seconds"]:.2f}s '
          f'{evidence.get("reason", "all effective states and firing witnesses checked")}', flush=True)
    return evidence['verdict'] == 'ok'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build/tomokv')
    parser.add_argument('--server-cpus', default='8-15')
    parser.add_argument('--load-cpus', default='96-127')
    parser.add_argument('--ratio', default='6:2')
    parser.add_argument('--port', type=int, default=8620)
    parser.add_argument('--output', required=True, help='new artifact directory; never reuse a run')
    which = parser.add_mutually_exclusive_group(required=True)
    which.add_argument('--cell', choices=CELLS)
    which.add_argument('--matrix', action='store_true')
    args = parser.parse_args()
    install_signals()
    pin_driver(args.server_cpus, args.load_cpus)
    inventory(args.server_cpus, args.ratio)
    results = [run_cell(args, cell) for cell in (CELLS if args.matrix else [args.cell])]
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
