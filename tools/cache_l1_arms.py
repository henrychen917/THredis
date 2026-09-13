#!/usr/bin/env python3
"""Prepare/build candidate 1 and its padding control; never start a workload.

Run `prepare --base <PRE commit>`, then `build --cores <build CPUs> --jobs <N>`.
Use `pad --cores <build CPUs>` to resume from already built PRE/POST arms.
All generated files stay in this worktree's ignored build/.
The PRE and POST snapshots use the same Makefile, including t_string's inline
budget. PAD reuses PRE objects; only unreachable executable padding is added.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'build/cache-L1'
ARMS = HERE / 'arms'
FLAGS = '-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread'


def command(argv, cwd=ROOT, **kwargs):
    return subprocess.run(argv, cwd=cwd, check=True, **kwargs)


def output(argv, cwd=ROOT):
    return command(argv, cwd=cwd, stdout=subprocess.PIPE).stdout.decode()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(root):
    paths = [root / 'Makefile']
    for name in ('src', 'third_party'):
        paths.extend(p for p in (root / name).rglob('*') if p.is_file())
    return {str(p.relative_to(root)): digest(p) for p in sorted(paths)}


def prepare(args):
    commit = output(['git', 'rev-parse', '--verify', args.base + '^{commit}']).strip()
    changes = output(['git', 'diff', commit, '--name-only', '--',
                      'Makefile', 'src', 'third_party']).splitlines()
    if changes != ['src/core/thread.h']:
        raise RuntimeError(f'expected only candidate 1 in the tracked diff, got {changes}')
    patch = command(['git', 'diff', commit, '--', 'src/core/thread.h'],
                    stdout=subprocess.PIPE).stdout
    archive = command(['git', 'archive', commit, 'Makefile', 'src', 'third_party'],
                      stdout=subprocess.PIPE).stdout
    ARMS.mkdir(parents=True, exist_ok=False)
    (HERE / 'candidate-01.patch').write_bytes(patch)
    for arm in ('pre', 'post'):
        target = ARMS / arm
        target.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
            contents.extractall(target, filter='data')
    # These snapshots live under the parent Git worktree. `git apply` there would
    # filter root-relative paths against build/... and silently skip the patch.
    command(['patch', '--batch', '--forward', '--dry-run', '-p1'],
            cwd=ARMS / 'post', input=patch)
    command(['patch', '--batch', '--forward', '-p1'], cwd=ARMS / 'post', input=patch)
    manifests = {arm: sources(ARMS / arm) for arm in ('pre', 'post')}
    changed = [p for p in manifests['pre']
               if manifests['pre'][p] != manifests['post'][p]]
    if changed != ['src/core/thread.h']:
        raise RuntimeError(f'unexpected arm differences: {changed}')
    manifest = {'base_commit': commit, 'status': 'sources prepared; not built or measured',
                'patch_sha256': digest(HERE / 'candidate-01.patch'),
                'source_difference': changed, 'sources': manifests}
    (HERE / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'PRE and POST sources prepared at {ARMS}; no compiler or workload started.')


def cores(value):
    result = set()
    for part in value.split(','):
        if not re.fullmatch(r'\d+(?:-\d+)?', part):
            raise argparse.ArgumentTypeError('use CPU numbers/ranges, e.g. 0-7')
        ends = [int(n) for n in part.split('-')]
        first, last = ends[0], ends[-1]
        if first > last or last > 111:
            raise argparse.ArgumentTypeError('build CPUs must lie in the allowed 0-111 range')
        result.update(range(first, last + 1))
    if not result:
        raise argparse.ArgumentTypeError('an assigned CPU set is required')
    return ','.join(str(n) for n in sorted(result))


def build(args):
    manifest = json.loads((HERE / 'source-manifest.json').read_text())
    for arm in ('pre', 'post'):
        if sources(ARMS / arm) != manifest['sources'][arm]:
            raise RuntimeError(f'{arm} sources changed after preparation')
    if args.jobs < 1 or args.jobs > len(args.cores.split(',')):
        raise RuntimeError('jobs must be positive and no larger than the assigned CPU set')
    env = dict(os.environ, CXXFLAGS=FLAGS)
    env.pop('MAKEFLAGS', None)
    env.pop('MFLAGS', None)
    env.pop('MAKEOVERRIDES', None)
    prefix = ['taskset', '-c', args.cores]
    prior = None
    if args.action == 'pad':
        prior = json.loads((HERE / 'build-provenance.json').read_text())
        for arm in ('pre', 'post'):
            binary = ARMS / arm / 'build/tomokv'
            if digest(binary) != prior[arm]['sha256']:
                raise RuntimeError(f'{arm} binary changed after its recorded build')
        if (prior['base_commit'] != manifest['base_commit'] or prior['cxxflags'] != FLAGS or
                prior['compiler'] != output(['g++', '--version']).splitlines()[0]):
            raise RuntimeError('saved PRE/POST provenance differs from this build environment')
    else:
        # No command-line CXXFLAGS assignment: it suppresses t_string's target-specific flag.
        for arm in ('pre', 'post'):
            with (HERE / f'{arm}-build.log').open('a') as log:
                command(prefix + ['make', '-j' + str(args.jobs), 'CXX=g++', 'JE=1', 'all'],
                        cwd=ARMS / arm, env=env, stdout=log, stderr=subprocess.STDOUT)
            if '--param large-unit-insns=10600' not in (HERE / f'{arm}-build.log').read_text():
                raise RuntimeError(f'{arm}: no t_string inline-budget evidence; use fresh snapshots')

    def executable(path, name):
        text_file = HERE / f'{name}.text'
        command(['objcopy', '--only-section=.text', '-O', 'binary', str(path), str(text_file)])
        return {'binary': str(path), 'sha256': digest(path),
                'text_bytes': text_file.stat().st_size, 'text_sha256': digest(text_file)}

    result = {arm: executable(ARMS / arm / 'build/tomokv', arm) for arm in ('pre', 'post')}

    def properties(path):
        notes = output(['readelf', '-n', str(path)])
        marker = 'Displaying notes found in: .note.gnu.property\n'
        return notes.split(marker, 1)[1].split('Displaying notes found in:', 1)[0].strip() if marker in notes else ''

    expected_properties = properties(result['pre']['binary'])
    if properties(result['post']['binary']) != expected_properties:
        raise RuntimeError('PRE and POST have different GNU properties')
    delta = result['post']['text_bytes'] - result['pre']['text_bytes']
    if delta < 0:
        raise RuntimeError('POST .text shrank; a positive-padding twin needs a new placement control')

    pad_dir = ARMS / 'pad'
    pad_dir.mkdir(exist_ok=False)
    objects = output(['make', '--no-print-directory', '-s',
                      '--eval=cache_l1_objects: ; @echo $(OBJ)', 'cache_l1_objects'],
                     cwd=ARMS / 'pre').strip()
    pad_bin = pad_dir / 'tomokv'

    def link_pad(front, tail):
        for name, count in (('front', front), ('tail', tail)):
            source = pad_dir / f'{name}.cc'
            # A bare .S file omits GCC's GNU property note. That removed IBT/SHSTK and
            # .plt.sec from the first PAD, changing call linkage as well as placement.
            # File-scope asm in a C++ TU gets the same compiler-generated properties
            # as PRE, with no callable function, constructor, or executed instruction.
            asm = f'.pushsection .text,"ax",@progbits\n.fill {count},1,0x90\n.popsection\n'
            source.write_text('asm(' + json.dumps(asm) + ');\n')
            command(prefix + ['g++', *shlex.split(FLAGS), '-c', str(source),
                              '-o', str(pad_dir / f'{name}.o')])
        pad_objects = f'{pad_dir / "front.o"} {objects} {pad_dir / "tail.o"}'
        # Relink with unchanged PRE objects and source. No NOP is on a called path.
        with (pad_dir / 'link.log').open('a') as log:
            command(prefix + ['make', '-j1', 'CXX=g++', 'JE=1', f'BIN={pad_bin}',
                              f'OBJ={pad_objects}', 'all'], cwd=ARMS / 'pre', env=env,
                    stdout=log, stderr=subprocess.STDOUT)
        return executable(pad_bin, 'pad')

    # Leading padding moves ordinary .text functions; a trailing remainder matches exact size.
    # Alignment may round the leading request up. Reduce it without measuring any workloads.
    front = delta
    for _ in range(8):
        pad = link_pad(front, 0)
        growth = pad['text_bytes'] - result['pre']['text_bytes']
        if growth <= delta:
            break
        next_front = max(0, front - (growth - delta))
        if next_front == front:
            raise RuntimeError('cannot fit leading padding to the requested .text size')
        front = next_front
    else:
        raise RuntimeError('padding alignment did not converge')
    tail = delta - growth
    if tail:
        pad = link_pad(front, tail)
    if pad['text_bytes'] != result['post']['text_bytes']:
        raise RuntimeError('PAD .text does not exactly match POST; S6 control is unready')
    if properties(pad_bin) != expected_properties:
        raise RuntimeError('PAD changed GNU properties; S6 control is unready')
    # Preserving size alone missed a 4816-byte .plt.sec removal. Compare every PRE
    # PLT section byte for byte, and retain the proof beside the arm hashes.
    plt_hashes = {}
    sections = re.findall(r'\]\s+(\.plt\S*)\s+PROGBITS',
                          output(['readelf', '-SW', result['pre']['binary']]))
    for section in sections:
        for arm in ('pre', 'pad'):
            path = Path(result['pre']['binary']) if arm == 'pre' else pad_bin
            dumped = pad_dir / f'{arm}{section}'
            command(['objcopy', '--only-section=' + section, '-O', 'binary', str(path), str(dumped)])
        if (pad_dir / f'pre{section}').read_bytes() != (pad_dir / f'pad{section}').read_bytes():
            raise RuntimeError(f'PAD changed {section}; S6 control is unready')
        plt_hashes[section] = digest(pad_dir / f'pad{section}')
    pad.update(front_padding_bytes=front, tail_padding_bytes=tail,
               source_arm='pre', fields_moved=False,
               gnu_properties=expected_properties, unchanged_plt_sha256=plt_hashes,
               limitation='Size control only; function addresses are not matched to POST.')
    result['pad'] = pad
    result.update(base_commit=manifest['base_commit'], cxxflags=FLAGS,
                  compiler=output(['g++', '--version']).splitlines()[0],
                  cores=args.cores, jobs=args.jobs, measured=False)
    if prior is not None:
        # The resume only links PAD; do not relabel the earlier PRE/POST build CPU set.
        result['cores'], result['jobs'] = prior['cores'], prior['jobs']
        result['pad'].update(build_cores=args.cores, build_jobs=args.jobs)
    # Inspect emitted types without running the binaries. All existing size asserts compiled above.
    for arm in ('pre', 'post', 'pad'):
        binary = result[arm]['binary']
        with (HERE / f'{arm}-layout.txt').open('w') as layout:
            command(['gdb', '-q', '-nx', '-batch', binary,
                     '-ex', 'ptype /o tomo::ThreadCtx'], stdout=layout)
        with (HERE / f'{arm}-symbols.txt').open('w') as symbols:
            command(['nm', '-nS', '--defined-only', binary], stdout=symbols)
    (HERE / 'build-provenance.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PRE/POST/PAD ready; PAD size, GNU properties and PLT checked. No workload started.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('prepare').add_argument('--base', required=True)
    for action in ('build', 'pad'):
        build_parser = sub.add_parser(action)
        build_parser.add_argument('--cores', type=cores, required=True)
        build_parser.add_argument('--jobs', type=int, default=1)
    args = parser.parse_args()
    prepare(args) if args.action == 'prepare' else build(args)
