#!/usr/bin/env python3
"""Frozen F11 decomposition and storage controls. Only compiles; never starts a server."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/cache-L3-f11'
ARMS = OUT / 'arms'
PRE = '3aadb258e'
OP = '70c8f369c'
POST = 'fe5041604'
FLAGS = '-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread'

def run(args, cwd=ROOT, **kw):
    return subprocess.run(args, cwd=cwd, check=True, **kw)

def capture(args, cwd=ROOT):
    return run(args, cwd, stdout=subprocess.PIPE).stdout

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def snapshot(name, commit):
    dst = ARMS / name
    dst.mkdir(parents=True)
    archive = capture(['git', 'archive', commit, 'Makefile', 'src', 'third_party'])
    with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
        contents.extractall(dst, filter='data')
    return dst

def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'{path}: expected one occurrence of {old[:80]!r}')
    path.write_text(text.replace(old, new))

def pad_op(dst):
    # Same total raw storage and one allocation as POST, without moving any original Op field.
    # The smaller POST header stride is the mechanism, and cannot also be a no-field-move control.
    path = dst / 'src/exec/op.h'
    replace(path, '}  // namespace tomo', '''template <size_t Count> struct OpChunk {
    Op ops[Count];
    unsigned char unused[Count * 8];
};
static_assert(sizeof(OpChunk<8>) == 2752);
}  // namespace tomo''')
    path = dst / 'src/net/rob.h'
    replace(path, 'delete[] chunks_[i]', 'delete chunks_[i]')
    replace(path, 'Op*& ch = chunks_[idx / kChunkOps];',
            'OpChunk<kChunkOps>*& ch = chunks_[idx / kChunkOps];')
    replace(path, 'ch = new Op[kChunkOps];', 'ch = new OpChunk<kChunkOps>;')
    replace(path, 'return &ch[idx % kChunkOps];', 'return &ch->ops[idx % kChunkOps];')
    replace(path, 'Op* chunks_[kChunks] = {};', 'OpChunk<kChunkOps>* chunks_[kChunks] = {};')

def pad_client(dst):
    # Old descriptors remain inline. The 8-byte owner pointer fits an existing alignment hole;
    # a lazy 56-byte unused allocation matches POST's 2040 total raw bytes after a segmented send.
    # No negative padding can match POST's *smaller* idle header while retaining all PRE fields.
    path = dst / 'src/net/conn.h'
    replace(path, '#include <limits>', '#include <limits>\n#include <memory>')
    replace(path, 'Session   session_;                 // 68..71', '''Session   session_;                 // 68..71
    struct SendPad { unsigned char unused[56] = {}; };
    std::unique_ptr<SendPad> send_pad_; // uses PRE's 56-byte hole; all original offsets stay fixed''')
    replace(path, 'uint32_t build_segment_iov(bool& has_borrow, uint32_t& bytes) {',
            '''uint32_t build_segment_iov(bool& has_borrow, uint32_t& bytes) {
        if (!send_pad_ && !segments_.empty()) send_pad_ = std::make_unique<SendPad>();''')

def sources(dst):
    paths = [dst / 'Makefile']
    for name in ('src', 'third_party'):
        paths.extend(p for p in (dst / name).rglob('*') if p.is_file())
    return {str(p.relative_to(dst)): sha(p) for p in sorted(paths)}

def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    ARMS.mkdir(exist_ok=False)
    snapshot('pre', PRE)
    op = snapshot('op', OP)
    post = snapshot('post', POST)
    client = snapshot('client', PRE)
    for name in ('src/net/conn.h', 'src/cmd/t_server.cc'):
        shutil.copy2(post / name, client / name)
    for name in ('pad-op', 'pad-client', 'pad-post'):
        dst = snapshot(name, PRE)
        if name in ('pad-op', 'pad-post'): pad_op(dst)
        if name in ('pad-client', 'pad-post'): pad_client(dst)

    # Expose the argv-header permutation separately from moving reply bytes, so the whole
    # Op result cannot quietly attribute candidate 2's benefit to the F11 body mechanism.
    header = snapshot('header', PRE)
    fields = '''    Slice*   argv_heap_ = nullptr;
    uint32_t argv_cap_  = 0;
    uint32_t argc_      = 0;
'''
    replace(header / 'src/exec/op.h', fields, '')
    replace(header / 'src/exec/op.h', '    SmallBuf<kInlineReply> reply;',
            'private:\n' + fields + 'public:\n    SmallBuf<kInlineReply> reply;')
    body = snapshot('body', OP)
    group = '''private:
    // Parse and execute both inspect argc/heap even for two inline arguments. Keeping that
    // metadata beside routing avoids fetching the tail solely to discover there is no heap.
    Slice*   argv_heap_ = nullptr;
    uint32_t argv_cap_  = 0;
    uint32_t argc_      = 0;
public:
'''
    replace(body / 'src/exec/op.h', group, '')
    replace(body / 'src/exec/op.h', '    Slice    argv_inline_[kInlineArgv];',
            '    Slice    argv_inline_[kInlineArgv];\n' + fields.rstrip())

    # Every binary is compiled from its archived sources. Never import root objects: their
    # timestamps do not establish which source/flags produced them after an interrupted build.
    manifest = {'pre': PRE, 'op': OP, 'post': POST,
                'sources': {p.name: sources(p) for p in sorted(ARMS.iterdir())}}
    (OUT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Sources frozen; no server or measurement started.', flush=True)

def build():
    if not set(os.sched_getaffinity(0)) <= set(range(112, 128)):
        raise RuntimeError('invoke with taskset -c 112-127')
    manifest = json.loads((OUT / 'source-manifest.json').read_text())
    env = dict(os.environ, CXXFLAGS=FLAGS)
    for key in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'): env.pop(key, None)
    result = {}
    for name in ('pre', 'post', 'op', 'client', 'pad-op', 'pad-client', 'pad-post', 'header', 'body'):
        dst = ARMS / name
        if sources(dst) != manifest['sources'][name]: raise RuntimeError(f'{name}: source drift')
        print(f'Building {name}', flush=True)
        with (OUT / f'{name}-build.log').open('a') as log:
            run(['make', '-j5', 'CXX=g++', 'JE=1', 'all'], cwd=dst, env=env,
                stdout=log, stderr=subprocess.STDOUT)
        # A resumed build may need only the objects interrupted by ENOSPC. Inspect the
        # explicit recipe too: an incremental log need not mention an unchanged string TU.
        recipe = run(['make', '-n', '-B', 'CXX=g++', 'JE=1', 'build/src/cmd/t_string.o'],
                     cwd=dst, env=env, stdout=subprocess.PIPE).stdout.decode()
        (OUT / f'{name}-string-recipe.txt').write_text(recipe)
        if '--param large-unit-insns=10600' not in recipe:
            raise RuntimeError('missing string inline-budget evidence')
        binary = dst / 'build/tomokv'
        text = OUT / f'{name}.text'
        run(['objcopy', '--only-section=.text', '-O', 'binary', str(binary), str(text)])
        result[name] = {'binary': str(binary), 'sha256': sha(binary),
                        'text_bytes': text.stat().st_size, 'text_sha256': sha(text)}
        with (OUT / f'{name}-layout.txt').open('w') as log:
            command = ['gdb', '-q', '-nx', '-batch', str(binary)]
            for typ in ('Op', 'OpReply', 'Client', 'ThreadCtx', 'Shard', 'FlatStore',
                        'Rob<64>', 'AtomicEntry', 'Config'):
                command.extend(['-ex', f'p sizeof(tomo::{typ})'])
            command.extend(['-ex', 'ptype /o tomo::Op', '-ex', 'ptype /o tomo::Client'])
            # PRE has no OpReply; GDB reports that absence and continues. No inferior is run.
            run(command, stdout=log, stderr=subprocess.STDOUT)
        (OUT / 'build-provenance.json').write_text(json.dumps({
            'compiler': capture(['g++', '--version']).decode().splitlines()[0],
            'cxxflags': FLAGS, 'affinity': sorted(os.sched_getaffinity(0)),
            'measured': False, 'arms': result}, indent=2) + '\n')
        print(f'Ready {name}: {result[name]["sha256"]}', flush=True)

if __name__ == '__main__':
    if not ARMS.exists(): prepare()
    build()
