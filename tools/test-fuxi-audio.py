#!/usr/bin/env python3
"""Audio lane host tests (P-AU1): GPR codec, AudioReach commands and the RSC sequencer arbiter.

Compiles xnu/fuxi-audio/*.c natively with the host tests in tools/fuxi-audio-host/ (ASan+UBSan).
No device, firmware, network or Apple code.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-audio'
HOST = ROOT / 'tools/fuxi-audio-host'
CC = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g']
ASAN = ['-fsanitize=address,undefined']


def run(tmp, name, sources, flags):
    exe = Path(tmp) / name
    subprocess.run(CC + flags + [str(s) for s in sources] + ['-lpthread', '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stderr.write(out.stderr)
    assert out.returncode == 0 and out.stdout.startswith('PASS'), name + ': ' + out.stdout + out.stderr
    print(out.stdout.strip())


def main():
    with tempfile.TemporaryDirectory(prefix='fuxi-audio-') as tmp:
        run(tmp, 'gpr', [HOST / 'gpr_host.c', SRC / 'fuxi_gpr.c'], ASAN)
        run(tmp, 'audioreach', [HOST / 'audioreach_host.c', SRC / 'fuxi_audioreach.c', SRC / 'fuxi_gpr.c'], ASAN)
        run(tmp, 'rpmh-arb', [HOST / 'rpmh_arb_host.c'], ASAN)
    print('PASS: fuxi-audio host tests')


if __name__ == '__main__':
    main()
