#!/usr/bin/env python3
"""Local M9p Vulkan controls; no phone, shaders, external translator or network."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-only', action='store_true')
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    ap.add_argument('--headers', type=Path, default=Path('/usr/include'))
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        test = Path(tmp)/'test'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-DFUXI_MTL_HOST_TEST', '-DFUXI_MTL_ICD="'+str(a.icd.resolve())+'"',
                        '-DFUXI_MTL_TRANSLATOR="unavailable-m9p-control"', '-I'+str(a.headers), '-I'+str(ROOT/'tools'),
                        str(ROOT/'tools/test-fuxi-metal-m9p.cpp'), '-ldl', '-pthread', '-o', str(test)], check=True)
        if a.compile_only:
            print('PASS: M9p production Backend compile-only')
            return
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', LP_NUM_THREADS='2')
        p = subprocess.run([str(test)], env=env,
                           text=True, capture_output=True, timeout=60)
        assert p.returncode == 0, p.stderr
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr
        print(p.stdout, end='')
if __name__ == '__main__':
    main()
