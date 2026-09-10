#!/usr/bin/env python3
"""Prove existing gate row bodies detect compiling, known mechanism breakages.

Run tests/mutants.sh --list or --check-registry without building anything. Ordinary execution
builds a control and one mutant at a time, in detached throwaway worktrees. Nothing in the tested
source is committed. Logs/results survive in --output; worktrees do not survive any exit path.

This runner executes the existing row bodies. The main gate's ledger inventory separately proves
that its scheduler dispatched them: executing a battery here alone cannot prove a gate scheduler
reached it. Missing row evidence is UNREACHED; an unexpected failure is ERROR, never a killed
mutant. A reached green row is SURVIVED (vacuous for this particular mutation).
"""
import argparse
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
# feature_gate imports _gate_process, which in turn imports its adjacent _lib. Loading the module
# by filename without this entry fails before a single feature row runs.
sys.path.insert(0, str(ROOT / 'tests'))
from _gate_process import cpus, server


ROWS = {
    'retirement': dict(label='reads never wait for retirement quiescence',
                       target='build/rehash-waits-unit', arguments=['retirement'],
                       success='RETURNED expiry', timeout=60),
    'reorder': dict(label='reorder mechanism + 32/128-task geometry battery',
                    source='tests/reorder_unit.cc', success='reorder battery: PASS', timeout=60,
                    flags=['-O1', '-g', '-fsanitize=address,undefined',
                           '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']),
    'ryow-ring': dict(label='read-local write ring + arming transient unit',
                      source='tests/read_local_write_ring_unit.cc',
                      success='read_local write ring unit: ok', timeout=60),
    'foreign-filter': dict(label='B+ counting-fingerprint filter unit',
                           source='tests/foreign_read_safety_test.cc',
                           success='foreign-read-safety unit: PASS', timeout=60),
    'cache-churn': dict(label='armed block-cache churn battery', live='cache',
                        success='rlcache-churn PASS', timeout=180),
    'cache-invariants': dict(label='read-local ownership invariants', live='cache',
                             success='rlcache-churn PASS', timeout=180),
    'xscript-0': dict(label='xscript battery (atomic 0)', live='xscript', atomic=0,
                      success='XSCRIPT all directed battery passed', timeout=300),
    'xscript-1': dict(label='xscript battery (atomic 1)', live='xscript', atomic=1,
                      success='XSCRIPT all directed battery passed', timeout=300),
    'hash-ttl-bytes': dict(label='storage hash-bytes regression',
                           target='build/store-regression', arguments=['hash-bytes'],
                           gate_loop=('STORE_CASE', 'storage $STORE_CASE regression'),
                           success='PASS storage hash-bytes', timeout=60),
    'multi-arity': dict(label='atomic survivor: mset_arity',
                        target='build/atomic-survivors-unit', arguments=['mset_arity'],
                        gate_loop=('defect', 'atomic survivor: $defect'),
                        success='PASS mset_arity', timeout=60),
    'lua-budget': dict(label='atomic survivor: instruction_limit',
                       target='build/atomic-survivors-unit', arguments=['instruction_limit'],
                       gate_loop=('defect', 'atomic survivor: $defect'),
                       success='PASS instruction_limit', timeout=60),
    'receive-lifetime': dict(label='netcmd receive regression',
                             target='build/netcmd-unit', arguments=['receive'],
                             gate_loop=('NETCMD_CASE', 'netcmd $NETCMD_CASE regression'),
                             success='ok: netcmd receive', timeout=60),
    'deferred-output': dict(label='netcmd output regression',
                            target='build/netcmd-unit', arguments=['output'],
                            gate_loop=('NETCMD_CASE', 'netcmd $NETCMD_CASE regression'),
                            success='ok: netcmd output', timeout=60),
    'notification-reservation': dict(label='netcmd notify-oom regression',
                                    target='build/netcmd-unit', arguments=['notify-oom'],
                                    gate_loop=('NETCMD_CASE', 'netcmd $NETCMD_CASE regression'),
                                    success='ok: netcmd notify-oom', timeout=60),
}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def feature_module():
    spec = importlib.util.spec_from_file_location('mutant_feature_gate', ROOT / 'tests/feature_gate.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def revision_text(revision, path):
    return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{revision}:{path}'], text=True)


def named_row_declared(row, gate):
    if row.get('live') == 'xscript':
        return 'xacct xmove xscript' in gate and 'ok "$t battery (atomic $AT)"' in gate
    if 'gate_loop' not in row:
        return row['label'] in gate
    # These existing unit families emit labels from shell loops. Check the selected case AND
    # its executable/argument AND the counted success declaration in that same loop; finding
    # a case name somewhere in the file would conceal an accidentally removed iteration.
    variable, label = row['gate_loop']
    pattern = (rf'for {re.escape(variable)} in ([^;]+);\s*do\b(.*?)\bdone\b')
    for values, body in re.findall(pattern, gate, re.S):
        cases = shlex.split(values.replace('\\\n', ' '))
        command = f'./{row["target"]} "${variable}"'
        if row['arguments'][0] in cases and command in body and f'ok "{label}"' in body:
            return True
    return False


def registry(path, only=None, revision='HEAD'):
    data = json.loads(path.read_text())
    if data.get('schema') != 1:
        raise ValueError('unknown mutant registry schema')
    features = feature_module()
    gate = revision_text(revision, 'tests/gate.sh')
    result, names = [], set()
    for item in data['mutants']:
        name = item['name']
        if not re.fullmatch(r'[a-z0-9-]+', name) or name in names:
            raise ValueError(f'invalid/duplicate mutant name: {name}')
        names.add(name)
        if only and name not in only:
            continue
        source = ROOT / item['file']
        if not source.resolve().is_relative_to(ROOT):
            raise ValueError(f'{name}: invalid source path')
        # ROOT can be on another revision or have pending source edits. An anchor matching those
        # bytes says nothing about the detached revision that will actually be compiled.
        text = revision_text(revision, item['file'])
        if not item['find'] or text.count(item['find']) != 1 or item['find'] == item['replace']:
            raise ValueError(f'{name}: STALE registry: mutation must change exactly one source match')
        rows = []
        for selection in item['rows']:
            re.compile(selection['failure'])
            if 'feature' in selection:
                selected = [cell for cell in features.CELLS if re.fullmatch(selection['feature'], cell)]
                if len(selected) != selection['count'] or 'ok "feature $FEATURE_CELL"' not in gate:
                    raise ValueError(f'{name}: UNREACHED: feature selector/gate declaration missing')
                rows.extend(dict(id='feature-' + cell, label='feature ' + cell, feature=cell,
                                 failure=selection['failure'], timeout=120) for cell in selected)
            else:
                key = selection['row']
                row = dict(ROWS[key], id=key, failure=selection['failure'])
                if not named_row_declared(row, gate):
                    raise ValueError(f'{name}: UNREACHED: gate row declaration missing: {row["label"]}')
                rows.append(row)
        if not rows or len({row['id'] for row in rows}) != len(rows):
            raise ValueError(f'{name}: empty/duplicate row selection')
        result.append(dict(item, expanded_rows=rows))
    if only and set(only) - names:
        raise ValueError('unknown --only: ' + ', '.join(sorted(set(only) - names)))
    if not result:
        raise ValueError('registry selected no mutants')
    return result


def process_identity(pid):
    try:
        # /proc/<pid>/stat comm may contain spaces and parentheses. starttime protects against
        # PID reuse between discovery and an exact-PID signal.
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return int(fields[1]), fields[19]
    except (OSError, ValueError, IndexError):
        return None


def session_processes(session):
    owned = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[3]) == session and fields[0] != 'Z':
                owned[int(path.name)] = (int(fields[1]), fields[19])
        except (OSError, ValueError, IndexError):
            continue
    return owned


def become_subreaper():
    # A nested row helper starts its own session too. If its parent exits, a session-only scan
    # cannot find that child. Linux reparents our orphaned descendants to this runner when it is a
    # subreaper; unrelated processes can never become its children through this mechanism.
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'cannot retain ownership of orphaned row descendants')


def adopted_children(before, primary, reap=True):
    owned = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        pid = int(path.name)
        if pid == primary or pid in before:
            continue
        try:
            fields = (path / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[1]) != os.getpid():
                continue
            if fields[0] == 'Z' and reap:
                with contextlib.suppress(ChildProcessError):
                    os.waitpid(pid, os.WNOHANG)
            else:
                owned[pid] = (int(fields[1]), fields[19])
        except (OSError, ValueError, IndexError):
            continue
    return owned


def descendants(pid):
    found = {}
    for path in Path('/proc').iterdir():
        if path.name.isdigit():
            identity = process_identity(int(path.name))
            if identity:
                found[int(path.name)] = identity
    owned, parents = {}, {pid}
    while True:
        children = {child for child, (parent, _) in found.items()
                    if parent in parents and child not in parents}
        if not children:
            return owned
        for child in children:
            owned[child] = found[child]
        parents.update(children)


def _stop_owned(process, owned=None, prior_children=None):
    # Every command starts its own session. An orphaned child retains that session after its
    # parent exits, even though a PPID-only walk can no longer find it. These are still exactly
    # the processes we started; no process-name/argv matching and no process-group signals.
    owned = dict(owned or {})
    owned.update(descendants(process.pid))
    owned.update(session_processes(process.pid))
    if prior_children is not None:
        owned.update(adopted_children(prior_children, process.pid))
    identity = process_identity(process.pid)
    if identity:
        owned[process.pid] = identity
    # Ask the parent first: Python battery context managers get an opportunity to stop their own
    # server. Then signal only descendants observed under this child, never argv/name matches.
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
    for sig in (signal.SIGTERM, signal.SIGKILL):
        owned.update(session_processes(process.pid))
        if prior_children is not None:
            owned.update(adopted_children(prior_children, process.pid))
        for pid, expected in owned.items():
            actual = process_identity(pid)
            # A surviving child can be reparented after its battery exits. Its starttime still
            # identifies our process; comparing PPID as well would leak that owned server.
            if actual and actual[1] == expected[1]:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(pid, sig)
        if process.poll() is None:
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=5)
        if sig == signal.SIGTERM:
            until = time.monotonic() + 2
            while time.monotonic() < until and session_processes(process.pid):
                time.sleep(.025)
    # Killing a nested parent can expose one more orphan generation. Drain that finite tree,
    # reaping only adopted children, before the worktree containing their binary is removed.
    until = time.monotonic() + 5
    while prior_children is not None:
        remaining = adopted_children(prior_children, process.pid)
        if not remaining:
            break
        owned.update(remaining)
        for pid, expected in remaining.items():
            actual = process_identity(pid)
            if actual and actual[1] == expected[1]:
                with contextlib.suppress(ProcessLookupError):
                    os.kill(pid, signal.SIGKILL)
        if time.monotonic() >= until:
            raise RuntimeError('owned descendants survived SIGKILL: ' + ','.join(map(str, remaining)))
        time.sleep(.025)
    if process.poll() is None:
        raise RuntimeError(f'owned command PID {process.pid} survived SIGKILL')
    return sorted(owned)


def stop_owned(process, owned=None, prior_children=None):
    # A second Ctrl-C must not interrupt cleanup halfway through and strand a TERM-ignoring child.
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
    try:
        return _stop_owned(process, owned, prior_children)
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)


def command(argv, cwd, logfile, timeout, env=None):
    start = time.monotonic()
    become_subreaper()
    prior_children = set(adopted_children(set(), None, reap=False))
    logfile.parent.mkdir(parents=True, exist_ok=True)
    leaked = []
    with logfile.open('w') as output:
        process = None
        # Block cancellation only across acquiring the Popen ownership token. Restore the mask in
        # the child before exec: an inherited blocked SIGTERM would defeat its cleanup handlers.
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        try:
            process = subprocess.Popen(list(map(str, argv)), cwd=cwd, env=env, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True,
                                       preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_SETMASK, mask))
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            code = process.wait(timeout=timeout)
            expired = False
            remaining = session_processes(process.pid)
            remaining.update(adopted_children(prior_children, process.pid))
            if remaining:
                leaked = sorted(remaining)
                stop_owned(process, remaining, prior_children)
        except subprocess.TimeoutExpired:
            stop_owned(process, prior_children=prior_children)
            code, expired = process.returncode, True
        except BaseException:
            if process is not None:
                stop_owned(process, prior_children=prior_children)
            raise
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
    return dict(pid=process.pid, returncode=code, expired=expired, seconds=time.monotonic() - start,
                output=logfile.read_text(errors='replace'), leaked_pids=leaked)


@contextlib.contextmanager
def worktree(parent, name, revision):
    path = parent / name
    created = False
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
    try:
        subprocess.run(['git', '-C', str(ROOT), 'worktree', 'add', '--detach', str(path), revision],
                       check=True, stdout=subprocess.DEVNULL)
        created = True
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        yield path
    finally:
        # Removal is strictly the path created above. Keep neither mutant source nor a stale git
        # worktree registration on exceptions, Ctrl-C, failed compilation, or failed controls.
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
        try:
            if created or (path / '.git').exists():
                subprocess.run(['git', '-C', str(ROOT), 'worktree', 'remove', '--force', str(path)], check=True)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)


def build(tree, mutant, rows, output, args):
    def status(result):
        if result['expired']:
            return 'timeout'
        if result.get('leaked_pids'):
            return 'leaked-children'
        return 'ok' if result['returncode'] == 0 else 'compile-failed'

    flags = '-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread'
    if mutant.get('debug_cache'):
        flags += ' -DTOMO_RL_CACHE_DEBUG'
    targets = ['all', *sorted({row['target'] for row in rows if 'target' in row})]
    result = command(['taskset', '-c', args.build_cpus, 'make', '-j' + str(args.jobs),
                      'CXXFLAGS=' + flags, *targets], tree, output / 'build.log', args.build_timeout)
    if status(result) != 'ok':
        return status(result)
    for row in rows:
        if 'source' not in row:
            continue
        compile_flags = ['-std=c++20', '-O2', '-march=native', '-pthread', '-I.', *row.get('flags', [])]
        result = command(['taskset', '-c', args.build_cpus, os.environ.get('CXX', 'g++'),
                          *compile_flags, row['source'], '-o', 'build/mutant-' + row['id']],
                         tree, output / ('build-' + row['id'] + '.log'), args.build_timeout)
        if status(result) != 'ok':
            return status(result)
    return 'ok'


def row_command(args, tree, row, output):
    if 'feature' in row:
        argv = [sys.executable, tree / 'tests/feature_gate.py', '--cell', row['feature'],
                '--binary', tree / 'build/tomokv', '--server-cpus', args.server_cpus,
                '--load-cpus', args.load_cpus, '--ratio', '6:2', '--port', str(args.port),
                '--output', output / 'feature']
    elif 'live' in row:
        # The same script owns boot and battery, so its signal/finally path tears down the exact
        # server PID even when the outer per-row deadline fires.
        argv = [sys.executable, tree / 'tests/mutants.py', '--_live-row', row['id'], '--_tree', str(tree),
                '--output', str(output), '--server-cpus', args.server_cpus,
                '--load-cpus', args.load_cpus, '--port', str(args.port)]
    else:
        binary = row.get('target', 'build/mutant-' + row['id'])
        argv = ['taskset', '-c', args.server_cpus, tree / binary, *row.get('arguments', [])]
    env = dict(os.environ, TOMO_GATE_STRICT='1', ASAN_OPTIONS='detect_leaks=1',
               UBSAN_OPTIONS='halt_on_error=1')
    return command(argv, tree, output / 'row.log', row['timeout'], env)


def classify(result, *, reached, passed, failure, evidence):
    # Failure exit codes alone cannot distinguish a defect from a missing Python import, occupied
    # port, overlapping CPUs, or compile failure. A matching assertion is mandatory evidence.
    if result['expired']:
        return 'ERROR', 'row deadline expired; no mechanism verdict'
    if result.get('leaked_pids'):
        return 'ERROR', 'row parent left owned child processes running; forcibly cleaned up'
    if not reached:
        return 'UNREACHED', 'row did not reach its assertion/contract'
    if result['returncode'] == 0 and passed:
        return 'SURVIVED', 'row reached and stayed green; vacuous for this mutation'
    if result['returncode'] != 0 and re.search(failure, evidence):
        return 'KILLED', 'named mechanism assertion failed'
    return 'ERROR', 'row failed for another reason, or exit status disagrees with its result'


def read_row_artifact(path):
    try:
        data = json.loads(path.read_text())
        if not isinstance(data, dict):
            raise ValueError('expected a JSON object')
        for field in ('reached', 'passed'):
            if field in data and not isinstance(data[field], bool):
                raise ValueError(f'{field} must be a boolean')
        if 'verdict' in data and data['verdict'] not in ('ok', 'FAIL'):
            raise ValueError('unknown verdict')
        if 'reason' in data and not isinstance(data['reason'], str):
            raise ValueError('reason must be text')
        return data, None
    except (OSError, ValueError) as exc:
        return {}, f'invalid row artifact {path}: {exc}'


def run_row(args, tree, row, output, control=False):
    output.mkdir(parents=True, exist_ok=False)
    result = row_command(args, tree, row, output)
    evidence = result['output']
    reached = passed = False
    artifact_error = None
    if 'feature' in row:
        artifact = output / 'feature' / row['feature'] / 'result.json'
        if artifact.exists():
            data, artifact_error = read_row_artifact(artifact)
            if data.get('cell') == row['feature']:
                evidence += '\n' + data.get('reason', '')
                # Existing feature results have no reached flag. Their actual child PID plus the
                # completed witness payload (or exact refusal contract) establishes it. Merely
                # writing {verdict:ok}, or exiting 0 with no result, cannot become a green control.
                completed = (data.get('contract') == 'refusal' and
                             'refused as documented:' in data.get('reason', '')) or (
                             isinstance(data.get('task_affinities'), list) and
                             isinstance(data.get('evidence'), dict))
                passed = data.get('verdict') == 'ok' and completed
                started = (artifact.parent / 'pid').is_file()
                reached = started and (data.get('reached', False) or passed or
                                       bool(re.search(row['failure'], evidence)))
    elif 'live' in row:
        artifact = output / 'live-result.json'
        if artifact.exists():
            data, artifact_error = read_row_artifact(artifact)
            reached = (data.get('reached', False) and (output / 'server/pid').is_file() and
                       (output / 'battery.log').is_file())
            passed = data.get('passed', False) and bool(re.search(row['success'], evidence))
            evidence += '\n' + data.get('reason', '')
            log = output / 'server' / 'server.log'
            if log.exists():
                evidence += '\n' + log.read_text(errors='replace')
    else:
        passed = bool(re.search(row['success'], evidence))
        reached = passed or bool(re.search(row['failure'], evidence))
    status, reason = classify(result, reached=reached, passed=passed,
                              failure=row['failure'], evidence=evidence)
    if artifact_error:
        status, reason = 'ERROR', artifact_error
    if control:
        status = 'CONTROL-PASS' if status == 'SURVIVED' else 'CONTROL-FAIL'
        reason = 'unmutated row passed' if status == 'CONTROL-PASS' else reason
    report = dict(row=row['label'], status=status, reason=reason, reached=reached,
                  returncode=result['returncode'], expired=result['expired'], seconds=result['seconds'],
                  leaked_pids=result.get('leaked_pids', []))
    write_json(output / 'result.json', report)
    print(f'  {status:12} {row["label"]}: {reason}', flush=True)
    return report


def live_row(args):
    row = ROWS[args._live_row]
    output = Path(args.output)
    report = dict(reached=False, passed=False)
    argv = ['--shards', '16', '--ratio', '6:2', '--atomic', str(row.get('atomic', 1))]
    if row['live'] == 'cache':
        argv = ['--thread-mode', '1s', '--shards', '64', '--atomic', '1', '--read-local', '1']
    from _gate_process import install_signals, pin_driver
    install_signals()
    pin_driver(args.server_cpus, args.load_cpus)
    try:
        with server(Path(args._tree) / 'build/tomokv', args.server_cpus, args.port,
                    output / 'server', argv):
            # A successfully booted server alone is not enough: final assertion evidence below
            # must also be present before a red result can count as a killed mutant.
            report['reached'] = True
            script = 'rlcache_churn.py' if row['live'] == 'cache' else 'xscript.py'
            extras = ['25', '48'] if row['live'] == 'cache' else []
            result = command([sys.executable, ROOT / 'tests' / script, '127.0.0.1', str(args.port), *extras],
                             ROOT, output / 'battery.log', row['timeout'] - 30,
                             dict(os.environ, TOMO_GATE_STRICT='1'))
            print(result['output'], end='', flush=True)
            report['passed'] = (result['returncode'] == 0 and not result['expired'] and
                                bool(re.search(row['success'], result['output'])))
            if result['expired']:
                report['reason'] = 'battery deadline expired'
    except Exception as exc:
        report.update(passed=False, reason=str(exc))
    finally:
        write_json(output / 'live-result.json', report)
    return 0 if report['passed'] else 1


def cpu_geometry(args):
    allowed = set(os.sched_getaffinity(0))
    physical, occupied = [], set()
    for cpu in sorted(allowed):
        siblings = set(cpus(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text().strip()))
        if not siblings & occupied:
            physical.append(cpu)
            occupied.update(siblings)
    if args.server_cpus is None:
        args.server_cpus = ','.join(map(str, physical[:8]))
    if args.load_cpus is None:
        server_set = set(cpus(args.server_cpus))
        server_physical = set().union(*(set(cpus(Path(
            f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text().strip()))
                                       for cpu in server_set))
        args.load_cpus = ','.join(map(str, [cpu for cpu in physical if cpu not in server_physical][:8]))
    server_set, load_set = set(cpus(args.server_cpus)), set(cpus(args.load_cpus))
    if len(server_set) != 8 or not load_set or not (server_set | load_set) <= allowed:
        raise ValueError('rows require eight allowed server CPUs (6:2), plus allowed load CPUs')
    sibling_sets = [set(cpus(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text().strip()))
                    for cpu in server_set]
    if any(siblings & load_set for siblings in sibling_sets):
        raise ValueError('server and load CPUs overlap physically (including SMT siblings)')
    if any(len(siblings & server_set) != 1 for siblings in sibling_sets):
        raise ValueError('server CPUs must name eight distinct physical cores')
    args.build_cpus = args.build_cpus or ','.join(map(str, sorted(allowed)))
    if not set(cpus(args.build_cpus)) <= allowed:
        raise ValueError('build CPUs escape process affinity')


def self_test():
    good = dict(returncode=0, expired=False)
    bad = dict(returncode=1, expired=False)
    cases = [
        (good, True, True, 'SURVIVED', ''),
        (bad, False, False, 'UNREACHED', 'server and load CPUs overlap'),
        (bad, True, False, 'ERROR', 'ModuleNotFoundError'),
        (bad, True, False, 'KILLED', 'mechanism assertion'),
        (good, True, False, 'ERROR', 'mechanism assertion'),
        (good, False, False, 'UNREACHED', ''),
        (dict(bad, expired=True), True, False, 'ERROR', 'mechanism assertion'),
        (dict(good, leaked_pids=[123]), True, True, 'ERROR', ''),
    ]
    for result, reached, passed, expected, evidence in cases:
        status, _ = classify(result, reached=reached, passed=passed,
                             failure='mechanism assertion', evidence=evidence)
        if status != expected:
            raise AssertionError((expected, status))
    gate = (ROOT / 'tests/gate.sh').read_text()
    for key, row in ROWS.items():
        if 'gate_loop' not in row:
            continue
        if not named_row_declared(row, gate):
            raise AssertionError(f'{key}: actual gate declaration not reached')
        # Neither an omitted loop member nor an unrelated matching label establishes
        # reachability. Keep both inadmissible as evidence that a row ran.
        if named_row_declared(row, gate.replace(row['arguments'][0], 'removed_case')):
            raise AssertionError(f'{key}: missing loop member was accepted')
        if named_row_declared(row, gate.replace('./' + row['target'], './build/wrong-binary')):
            raise AssertionError(f'{key}: wrong loop executable was accepted')
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mutants-self-test-', dir=ROOT / 'build') as temporary:
        output = Path(temporary)
        # Exercise row_command -> actual child -> artifact parser -> classifier, not just made-up
        # classify() inputs. The selected tree's fake driver exits 0 without running any server.
        # It must remain UNREACHED, proving both immutable-script selection and missing evidence.
        fake = output / 'fake-tree'
        (fake / 'tests').mkdir(parents=True)
        driver = fake / 'tests/feature_gate.py'
        driver.write_text('print("selected-tree driver")\n')
        args = argparse.Namespace(server_cpus='0-7', load_cpus='8-15', port=8990)
        row = dict(id='fake-feature', feature='1s-0-0-0-1', label='fake feature', timeout=5,
                   failure='mechanism assertion')
        report = run_row(args, fake, row, output / 'missing-result')
        if report['status'] != 'UNREACHED' or 'selected-tree driver' not in (output / 'missing-result/row.log').read_text():
            raise AssertionError('missing-result/selected-worktree end-to-end control failed')
        driver.write_text('import json,pathlib,sys\n'
                          'p=pathlib.Path(sys.argv[sys.argv.index("--output")+1])/"1s-0-0-0-1"\n'
                          'p.mkdir(parents=True)\n'
                          '(p/"result.json").write_text(json.dumps({"cell":"1s-0-0-0-1","verdict":"ok","reached":"false"}))\n')
        if run_row(args, fake, row, output / 'malformed-result')['status'] != 'ERROR':
            raise AssertionError('malformed boolean was accepted as reachability evidence')

        # A parent can exit 0 while leaving a TERM-ignoring child in ANOTHER session. Both a PPID
        # walk and the old parent's session scan miss it. Exercise subreaper ownership, cleanup,
        # and the independent existing-child control through the real subprocess runner.
        child = ('import os,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                 'print("CHILD",os.getpid(),flush=True); time.sleep(90)')
        parent = ('import subprocess,sys; '
                  f'p=subprocess.Popen([sys.executable,"-c",{child!r}],stdout=subprocess.PIPE,text=True,start_new_session=True); '
                  'print(p.stdout.readline(),end="",flush=True); print("ROW PASS",flush=True)')
        spectator = subprocess.Popen([sys.executable, '-c', 'import time; print("ready",flush=True); time.sleep(90)'],
                                     stdout=subprocess.PIPE, text=True, start_new_session=True)
        try:
            if spectator.stdout.readline().strip() != 'ready':
                raise AssertionError('existing-child control did not arm')
            result = command([sys.executable, '-c', parent], ROOT, output / 'orphan.log', 5)
            if spectator.poll() is not None:
                raise AssertionError('command cleanup stopped a child it did not start')
        finally:
            spectator.terminate()
            spectator.wait(timeout=5)
            spectator.stdout.close()
        if result['returncode'] != 0 or not result['leaked_pids'] or session_processes(result['pid']):
            raise AssertionError('normal-parent orphan cleanup did not run')
        if classify(result, reached=True, passed=True, failure='FAIL', evidence=result['output'])[0] != 'ERROR':
            raise AssertionError('orphaned child turned into a green row')

        def interrupted(_signum, _frame):
            raise KeyboardInterrupt('self-test cancellation')
        previous = signal.signal(signal.SIGTERM, interrupted)
        interrupted_path = output / 'interrupted-worktree'
        try:
            try:
                with worktree(output, interrupted_path.name, 'HEAD') as tree:
                    child = ('import os,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                             'print("OWNED",os.getpid(),flush=True); '
                             'os.kill(os.getppid(),signal.SIGTERM); time.sleep(90)')
                    command([sys.executable, '-c', child], tree, output / 'interrupt.log', 5)
            except KeyboardInterrupt:
                pass
            else:
                raise AssertionError('interruption negative control was never reached')
        finally:
            signal.signal(signal.SIGTERM, previous)
        child_pid = int((output / 'interrupt.log').read_text().split()[1])
        listed = subprocess.check_output(['git', '-C', str(ROOT), 'worktree', 'list', '--porcelain'], text=True)
        if interrupted_path.exists() or str(interrupted_path) in listed or session_processes(child_pid):
            raise AssertionError('interrupted child/worktree survived cleanup')
    print('MUTANTS self-test: 8 classification controls; actual selected-tree/artifact/orphan/cancellation/PID/worktree controls passed; no servers/builds')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', action='append', help='mutant name; repeat to select several')
    parser.add_argument('--registry', type=Path, default=ROOT / 'tests/mutants.registry')
    parser.add_argument('--revision', default='HEAD', help='commit to test; source/tests must match this checkout')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--check-registry', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--server-cpus')
    parser.add_argument('--load-cpus')
    parser.add_argument('--build-cpus')
    parser.add_argument('--port', type=int, default=8990)
    parser.add_argument('--jobs', type=int, default=min(32, len(os.sched_getaffinity(0))))
    parser.add_argument('--build-timeout', type=int, default=1800)
    parser.add_argument('--output', help='new artifact directory; never reused')
    parser.add_argument('--_live-row', choices=ROWS, help=argparse.SUPPRESS)
    parser.add_argument('--_tree', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args._live_row:
        return live_row(args)
    if args.self_test:
        return self_test()
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', '--verify',
                                       args.revision + '^{commit}'], text=True).strip()
    if not (args.list or args.check_registry):
        # Run source and row bodies from one immutable revision. A clean ROOT at another commit
        # is a mismatch too; silently mixing its tests with an older binary would test a different
        # claim. Untracked tests/modules can shadow imports, so they cannot be treated as harmless.
        changed = subprocess.check_output(['git', '-C', str(ROOT), 'diff', revision, '--name-only',
                                          '--', 'src', 'tests', 'Makefile'], text=True)
        untracked = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '--others',
                                            '--exclude-standard', '--', 'src', 'tests', 'Makefile'], text=True)
        if changed.strip() or untracked.strip():
            raise ValueError('source/tests must be committed and match --revision before mutation mode: ' +
                             '\n'.join(part.strip() for part in (changed, untracked) if part.strip()))
    items = registry(args.registry, args.only, revision)
    if args.list or args.check_registry:
        for item in items:
            print(f'{item["name"]}: {len(item["expanded_rows"])} rows')
            if args.list:
                for row in item['expanded_rows']:
                    print('  ' + row['label'])
        return 0
    cpu_geometry(args)
    if not 1 <= args.port <= 65535 or args.jobs < 1 or args.build_timeout < 1:
        raise ValueError('invalid port/jobs/build timeout')
    output = Path(args.output).resolve() if args.output else Path(tempfile.mkdtemp(prefix='gate-mutants-results-'))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    write_json(output / 'run.json', dict(revision=revision, server_cpus=args.server_cpus,
               load_cpus=args.load_cpus, mutants=[item['name'] for item in items]))
    print(f'MUTANTS revision={revision} server={args.server_cpus} load={args.load_cpus} output={output}', flush=True)
    reports = []
    # Signal exceptions unwind both the command and worktree finally blocks. Never leave a mutant
    # worktree behind on SIGTERM, and never stop another user's process to make the port available.
    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    with tempfile.TemporaryDirectory(prefix='worktrees-', dir=output) as temporary, contextlib.ExitStack() as controls_stack:
        parent = Path(temporary)
        controls = {}
        for item in items:
            name, rows = item['name'], item['expanded_rows']
            print(f'=== {name} ({len(rows)} mandatory rows) ===', flush=True)
            artifact = output / name
            artifact.mkdir()
            report = dict(mutant=name, status='ERROR', control=[], mutant_rows=[])
            profile = bool(item.get('debug_cache'))
            if profile not in controls:
                # A shared pristine control build is safe: each row still reruns against fresh
                # process/state. Debug and release objects must never share a make cache because
                # make's timestamp dependencies do not encode CXXFLAGS.
                profile_name = 'debug-cache' if profile else 'release'
                control = controls_stack.enter_context(worktree(parent, 'control-' + profile_name, revision))
                control_rows = {row['id']: row for candidate in items
                                if bool(candidate.get('debug_cache')) == profile
                                for row in candidate['expanded_rows']}
                built = build(control, item, list(control_rows.values()), output / ('control-build-' + profile_name), args)
                controls[profile] = (control, built)
            control, built = controls[profile]
            if built != 'ok':
                report.update(status='CONTROL-BUILD-FAIL', reason='unmutated build: ' + built)
            else:
                report['control'] = [run_row(args, control, row, artifact / 'control' / row['id'], True)
                                     for row in rows]
                if any(row['status'] != 'CONTROL-PASS' for row in report['control']):
                    report.update(status='CONTROL-FAIL', reason='no mutation verdict: an unmutated row failed')
                else:
                    report['status'] = 'CONTROL-PASS'
            if report['status'] == 'CONTROL-PASS':
                with worktree(parent, name + '-mutant', revision) as mutant:
                    source = mutant / item['file']
                    original = source.read_text()
                    if original.count(item['find']) != 1:
                        report.update(status='STALE', reason='mutation does not match checked-out revision exactly once')
                    else:
                        source.write_text(original.replace(item['find'], item['replace'], 1))
                        build_status = build(mutant, item, rows, artifact / 'mutant', args)
                        if build_status != 'ok':
                            report.update(status='STALE' if build_status == 'compile-failed' else 'MUTANT-BUILD-ERROR',
                                          reason='mutant build: ' + build_status + '; no row was tested')
                        else:
                            report['mutant_rows'] = [run_row(args, mutant, row, artifact / 'mutant' / row['id'])
                                                    for row in rows]
                            report['status'] = ('KILLED' if all(row['status'] == 'KILLED' for row in report['mutant_rows'])
                                                else 'FAIL')
            reports.append(report)
            write_json(artifact / 'result.json', report)
            write_json(output / 'results.json', reports)
            print(f'{name}: {report["status"]}', flush=True)
    passed = sum(report['status'] == 'KILLED' for report in reports)
    print(f'MUTANTS: {passed}/{len(reports)} mechanisms detected by EVERY named row; artifacts={output}')
    return 0 if passed == len(reports) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f'MUTANTS ERROR: {exc}', file=sys.stderr)
        sys.exit(2)
