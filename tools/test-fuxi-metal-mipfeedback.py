#!/usr/bin/env python3
"""ASan/UBSan mip analysis and actual production Backend on local software Vulkan.
Uses authored GLSL and the test adapter; no Mac/device/downloads.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--glslang', default=shutil.which('glslangValidator'))
    a = ap.parse_args()
    if not a.glslang:
        ap.error('supply an existing glslangValidator with --glslang')
    spec = importlib.util.spec_from_file_location('m6', ROOT/'tools/test-fuxi-metal-m6.py')
    m6 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m6)
    flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2', FUXI_MTL_TEST_BOOTARGS='')
        unit = d/'unit'
        subprocess.run(flags+[str(ROOT/'tools/fuxi-metal/MipAccessTests.cpp'), '-o', str(unit)], check=True)
        subprocess.run([str(unit)], check=True, env=env)
        fragment = '''#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(push_constant) uniform Constants { float lod; } constants;
layout(location=0) out vec4 color;
void main() {color=textureLod(image,vec2(0.5),constants.lod);}
'''
        multi = fragment.replace('layout(location=0)', 'layout(set=0,binding=1) uniform sampler2D other;\nlayout(location=0)').replace('textureLod(image,vec2(0.5),constants.lod)', '(textureLod(image,vec2(0.5),constants.lod)+textureLod(other,vec2(0.5),0.0))*0.5')
        for stage, source in [('vert', m6.VERTEX), ('frag', fragment), ('multi.frag', multi)]:
            src = d/('control.'+stage)
            src.write_text(source)
            spv = d/(stage+'.spv')
            subprocess.run([a.glslang, '-V', '--target-env', 'vulkan1.2', str(src), '-o', str(spv)], check=True, capture_output=True)
            env[{'vert': 'CONTROL_VERTEX', 'frag': 'CONTROL_FRAGMENT', 'multi.frag': 'CONTROL_FRAGMENT_MULTI'}[stage]] = str(spv)
        subprocess.run(flags+['-shared', '-fPIC', str(ROOT/'tools/fuxi-metal/MipFeedbackBridge.cpp'), '-o', str(d/'bridge.so')], check=True)
        test = d/'test'
        subprocess.run(flags+['-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_ICD="'+str(a.icd.resolve())+'"',
            '-DFUXI_MTL_TRANSLATOR="'+str(d/'bridge.so')+'"', '-I'+str(a.headers),
            str(ROOT/'tools/fuxi-metal/MipFeedbackTests.cpp'), '-ldl', '-pthread', '-o', str(test)], check=True)
        for index, args in enumerate(('', 'fuxigpuskip=1', 'fuxigpuskip=1 fuxigpuskip=1', 'fuxigpuskip=bad', 'fuxigpufeedback=1', 'fuxigpusubmit=1', 'fuxigpusubmit=1 fuxigpuskip=1', 'fuxigpusubmit=1 fuxigpufeedback=1')):
            gate = d/('gate'+str(index))
            gate.mkdir(mode=0o700)
            p = subprocess.run([str(test)], env=dict(env, FUXI_MTL_TEST_BOOTARGS=args, CONTROL_GATE=str(gate)), capture_output=True, text=True, timeout=60)
            assert p.returncode == 0, p.stderr
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            assert 'runtime-failure next-respawn=CPU' not in p.stderr, p.stderr
            assert ('sampling skip draw' in p.stderr) == (args in ('fuxigpuskip=1', 'fuxigpusubmit=1 fuxigpuskip=1')), p.stderr
            if args in ('fuxigpuskip=1', 'fuxigpusubmit=1 fuxigpuskip=1'):
                assert p.stderr.count('sampling skip draw') == 9 and 'count=16' in p.stderr and 'count=9' not in p.stderr, p.stderr
            assert ('sampling allow feedback' in p.stderr) == (args in ('fuxigpufeedback=1', 'fuxigpusubmit=1 fuxigpufeedback=1')), p.stderr
            print(p.stdout, end='')
        print('PASS: default unchanged; feedback opt-in runs same-image draws; skip opt-in exact-token/duplicate/malformed policy, successful subsequent GPU draws')
if __name__ == '__main__':
    main()
