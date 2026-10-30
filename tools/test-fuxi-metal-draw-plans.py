#!/usr/bin/env python3
"""Pinned external draw-planner differential tests and ring-buffer SubmitBench A/B; no downloads.

Build --translator from tools/fuxi-metal2vulkan-bridge.rs and the pinned external
source with the existing host bridge recipe. Uses original authored AIR, not Xcode.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--headers', type=Path)
    ap.add_argument('--icd', default='libvulkan_lvp.so')
    ap.add_argument('--llvm-as', default=shutil.which('llvm-as-18'))
    a = ap.parse_args()
    if not a.llvm_as:
        ap.error('supply --llvm-as')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2')
        for key in ('PIPE_BENCH_EXACT', 'PIPE_BENCH_UNCACHED', 'PIPE_BENCH_RING', 'PIPE_BENCH_SHAPES', 'MERGE_BENCH_ENCODERS'):
            env.pop(key, None)
        flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror']
        if a.headers:
            flags += ['-I'+str(a.headers)]
        definitions = ['-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_ICD='+json.dumps(a.icd),
                       '-DFUXI_MTL_TRANSLATOR='+json.dumps(str(a.translator.resolve()))]
        def build(name, sanitized=True, backend=True):
            command = flags + (['-O1', '-fsanitize=address,undefined'] if sanitized else ['-O2'])
            if backend:
                command += definitions
            subprocess.run(command + [str(ROOT/'tools/fuxi-metal'/(name+'.cpp')), '-ldl', '-pthread', '-o', str(d/name)], check=True)
        def run(name, cli=(), **overrides):
            p = subprocess.run([str(d/name), *map(str, cli)], env=dict(env, **overrides), text=True, capture_output=True, timeout=120)
            assert p.returncode == 0, p.stderr
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            print(p.stdout, end='', flush=True)
            return p.stdout
        for name in ('DrawPlanTests', 'DrawPlanCacheTests', 'FrameTraceTests'):
            build(name, backend=False)
            run(name, FUXI_MTL_TEST_BOOTARGS='fuxigpuframetrace=1 fuxigpumerge=1')
        air = d/'control.bc'
        subprocess.run([a.llvm_as, str(ROOT/'tools/shaders/metal-m7-control.ll'), '-o', str(air)], check=True)
        build('DrawPlanRebaseTests')
        run('DrawPlanRebaseTests', (air,), FUXI_MTL_TEST_BOOTARGS='')
        build('SubmitBench', sanitized=False)
        controls = 'fuxigpuframetrace=1 fuxigpumerge=1 fuxigpusubmit=1'
        # The optional extension disabled is the original exact-input cache.
        for exact in (True, False, False, True):
            output = run('SubmitBench', (air,), FUXI_MTL_TEST_BOOTARGS=controls, PIPE_BENCH_RING='1', PIPE_BENCH_SHAPES='1', **({'PIPE_BENCH_EXACT': '1'} if exact else {}))
            assert ('hit_rate=0.00%' if exact else 'hit_rate=100.00%') in output
        output = run('SubmitBench', (air,), FUXI_MTL_TEST_BOOTARGS=controls+' fuxigputrace=1', PIPE_BENCH_RING='1')
        assert 'plan_hits/frame=0.00 plan_misses/frame=120.00' in output
        print('PASS: external ring-buffer A/B pixels, lifetime checks, exact/shape hit counters and trace bypass (Linux only)')


if __name__ == '__main__':
    main()
