#!/usr/bin/env python3
"""Manual regression for FIXREDIS.md; requires a fresh DEBUG-enabled split server.

Geometry: eight server cores, --shards 16 --ratio 6:2 --atomic 1.
The foreign-guard arm reproduces the old abandonment row's false positive; the
same-connection guard must force rejection. The final arm proves a candidate
was installed before abandonment and stays invisible to both readers.

Run directly against an owned server; this script never starts or stops one.
Every window has a bounded fresh-state arming budget. A missing witness fails.
"""

import json
import select
import sys
import time
from contextlib import ExitStack
import _lib

if len(sys.argv) != 3:
    raise SystemExit("usage: redisgap_atomic.py <host> <port>")
host, port = sys.argv[1], int(sys.argv[2])
admin = _lib.Conn(host, port, timeout=15)

def release(clients, *commands):
    # Attempt every disarm and close even if one cleanup command fails.
    with ExitStack() as cleanup:
        for client in clients:
            cleanup.callback(client.close)
        for command in commands:
            cleanup.callback(admin.must, 'DEBUG', *command)

def stats():
    return {k: int(v) for k, v in _lib.info(admin, 'stats').items()
            if k.startswith('atomic_') or k == 'evicted_keys'}

def drain():
    deadline = time.monotonic() + 8
    while True:
        s = stats()
        if s['atomic_inflight'] == 0 and s['atomic_pending_entries'] == 0:
            return s
        assert time.monotonic() < deadline, ('did not drain', s)
        time.sleep(.01)

def installed_abort():
    for trial in range(4):
        clients = []
        row = {'trial': trial, 'case': 'installed-aborted'}
        try:
            before = drain()
            buckets = _lib.owner_buckets(admin, 'audit-abort:%d' % time.time_ns(), per_owner=64)
            owners = sorted(owner for owner, keys in buckets.items() if len(keys) >= 64)[:2]
            a, b = [buckets[owner] for owner in owners]
            ap = _lib.shards_of(admin, a)
            guard, candidate = a[0], b[0]
            hold = [a[1], next(a[i] for i in range(2, len(a)) if ap[i][0] != ap[1][0])]
            geometry = _lib.shards_of(admin, [guard, candidate] + hold)
            assert geometry[0][1] != geometry[1][1]
            assert geometry[2][1] == geometry[3][1] == geometry[0][1]
            assert geometry[2][0] != geometry[3][0]
            row['geometry'] = geometry
            holder, writer, foreign = [_lib.Conn(host, port, timeout=15) for _ in range(3)]
            clients += [holder, writer, foreign]
            assert writer.must('SET', guard, 'guard') == b'OK'
            admin.must('DEBUG', 'ATOMIC-COMMIT-DELAY', 1000000)
            holder.send('MSET', hold[0], 'holder', hold[1], 'holder')
            time.sleep(.03)
            held = stats()
            if held['atomic_pending_entries'] != 2:
                row.update(outcome='clean-miss', reason='holder did not install')
                print('ATOMIC_ABORT ' + json.dumps(row), flush=True)
                continue
            value = b'abandoned:' + b'x' * 512
            writer.raw(_lib.encode('MSETNX', candidate, value, guard, value) +
                       _lib.encode('MGET', guard, candidate))
            deadline = time.monotonic() + .5
            while True:
                installed = stats()
                if installed['atomic_pending_entries'] >= 3:
                    break
                if time.monotonic() >= deadline:
                    break
                time.sleep(.001)
            row['pending_before'] = held['atomic_pending_entries']
            row['pending_installed'] = installed['atomic_pending_entries']
            if installed['atomic_pending_entries'] < 3:
                row['outcome'] = 'clean-miss'
                print('ATOMIC_ABORT ' + json.dumps(row), flush=True)
                continue
            assert not select.select([writer.sock], [], [], 0)[0], 'writer finished before candidate witness'
            assert foreign.must('GET', candidate) is None, 'foreign read exposed undecided candidate'
            excluded = stats()
            row['foreign_excluded_delta'] = (excluded['atomic_tripwire_excluded_conn_mismatch'] -
                                             installed['atomic_tripwire_excluded_conn_mismatch'])
            assert row['foreign_excluded_delta'] > 0, ('foreign read did not skip installed candidate', row)
            admin.must('DEBUG', 'ATOMIC-COMMIT-DELAY', 0)
            assert holder.read() == b'OK'
            verdict, values = writer.read(), writer.read()
            final = foreign.must('MGET', guard, candidate)
            row.update(reply=verdict,
                       values=[None if v is None else v[:32].decode() for v in values],
                       final=[None if v is None else v[:32].decode() for v in final],
                       outcome='witnessed')
            print('ATOMIC_ABORT ' + json.dumps(row), flush=True)
            assert verdict == 0 and values == [b'guard', None] and final == values, row
            return row
        finally:
            release(clients, ('ATOMIC-COMMIT-DELAY', 0))
    raise AssertionError('installed-aborted candidate window never witnessed')

try:
    server = _lib.info(admin, 'server')
    assert (server['thread_mode'], server['shards'], server['io_threads'],
            server['ex_threads']) == ('2s', '16', '6', '2'), server
    assert admin.must('CONFIG', 'GET', 'maxmemory') == [b'maxmemory', b'0']
    assert admin.must('CONFIG', 'GET', 'maxmemory-policy') == [b'maxmemory-policy', b'noeviction']
    admin.must('CONFIG', 'SET', 'atomic', 1)
    admin.must('DEBUG', 'TRIPWIRE', 'ARM')
    rows = []
    for own_guard in (False, True):
        witnessed = False
        for trial in range(4):
            clients = []
            started = time.monotonic()
            row = {'own_guard': own_guard, 'trial': trial}
            try:
                before = drain()
                buckets = _lib.owner_buckets(admin, 'audit-cut:%d' % time.time_ns(), per_owner=64)
                owners = sorted(owner for owner, keys in buckets.items() if len(keys) >= 64)[:2]
                a, b = [buckets[owner] for owner in owners]
                bp = _lib.shards_of(admin, b)
                keys = [a[0], b[0]]
                hold = [b[1], next(b[i] for i in range(2, len(b)) if bp[i][0] != bp[1][0])]
                geometry = _lib.shards_of(admin, keys + hold)
                assert geometry[0][1] != geometry[1][1]
                assert geometry[2][1] == geometry[3][1] == geometry[1][1]
                assert geometry[2][0] != geometry[3][0]
                row['geometry'] = geometry
                reader, holder, writer = [_lib.Conn(host, port, timeout=15) for _ in range(3)]
                clients += [reader, holder, writer]
                admin.must('DEBUG', 'ATOMIC-FANOUT-DEFER', 4000000)
                reader.raw(_lib.encode('MGET', *keys) + _lib.encode('PING'))
                time.sleep(.03)
                if select.select([reader.sock], [], [], 0)[0]:
                    row.update(outcome='clean-miss', reason='head read did not hold')
                    print('ATOMIC_CUT ' + json.dumps(row), flush=True)
                    continue
                assert admin.must('DEL', *keys) == 0
                # GET explicitly attempts owner-side promotion. Both tombstones must survive it,
                # witnessed while the delayed snapshot still has no reply.
                assert admin.must('GET', keys[0]) is None
                assert admin.must('GET', keys[1]) is None
                pinned = stats()
                if (pinned['atomic_pending_entries'] < 2 or
                        select.select([reader.sock], [], [], 0)[0]):
                    row.update(outcome='clean-miss', reason='old cut did not retain DEL')
                    print('ATOMIC_CUT ' + json.dumps(row), flush=True)
                    continue
                admin.must('DEBUG', 'ATOMIC-COMMIT-DELAY', 1000000)
                windows = pinned['atomic_commit_windows']
                holder.send('MSET', hold[0], 'holder', hold[1], 'holder')
                time.sleep(.03)
                target = writer if own_guard else admin
                t = time.monotonic()
                assert target.must('SET', keys[0], 'guard') == b'OK'
                row['guard_set_seconds'] = time.monotonic() - t
                guarded = stats()
                row['windows_delta'] = guarded['atomic_commit_windows'] - windows
                row['pinned_before_holder'] = pinned['atomic_pending_entries']
                row['guarded_pending'] = guarded['atomic_pending_entries']
                # The holder owns B only; the guard owner must finish a ticket while B holds
                # the safe watermark back. A miss is discarded only after complete cleanup.
                if not row['windows_delta']:
                    row['outcome'] = 'clean-miss'
                    print('ATOMIC_CUT ' + json.dumps(row), flush=True)
                    continue
                assert target.must('GET', keys[0]) == b'guard'
                value = b'candidate:' + b'x' * 512
                writer.raw(_lib.encode('MSETNX', keys[0], value, keys[1], value) +
                           _lib.encode('MGET', *keys))
                # Retain the first snapshot's captured deadline but avoid delaying this MGET.
                admin.must('DEBUG', 'ATOMIC-FANOUT-DEFER', 0)
                verdict, values = writer.read(), writer.read()
                assert holder.read() == b'OK'
                assert reader.read() == [None, None]
                assert reader.read() == b'PONG'
                final = admin.must('MGET', *keys)
                after = stats()
                row.update(reply=verdict, values=[None if v is None else v[:32].decode() for v in values],
                           final=[None if v is None else v[:32].decode() for v in final],
                           commit_holds_delta=after['atomic_commit_holds']-guarded['atomic_commit_holds'],
                           evicted_delta=after['evicted_keys']-before['evicted_keys'])
                assert row['commit_holds_delta'] > 0 and row['evicted_delta'] == 0, row
                if own_guard:
                    assert verdict == 0 and values == [b'guard', None] and final == values, row
                else:
                    assert verdict == 1 and values == [value, value] and final == values, row
                row['outcome'] = 'witnessed'
                row['seconds'] = time.monotonic() - started
                rows.append(row)
                print('ATOMIC_CUT ' + json.dumps(row), flush=True)
                print('ATOMIC_TRIPWIRE ' + admin.must('DEBUG', 'TRIPWIRE').decode(), flush=True)
                witnessed = True
                break
            finally:
                release(clients, ('ATOMIC-COMMIT-DELAY', 0), ('ATOMIC-FANOUT-DEFER', 0))
        assert witnessed, ('cut window never witnessed', own_guard)
    installed_abort()
    print('ATOMIC_CUT PASS foreign successful commit, own rejection, installed-aborted invisibility', flush=True)
finally:
    release([admin], ('TRIPWIRE', 'DISARM'))
