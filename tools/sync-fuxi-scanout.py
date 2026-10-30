#!/usr/bin/env python3
"""Generate/check GPU-SCANOUT's surface/console overlay after preceding patches.

The GPU implementation is carried by sync-fuxi-metal.py; framebuffer integration
by sync-fuxi-unixnu.py. This overlay owns the final SweetSurface source/header snapshots and
only the existing display-kick function in pe_serial (no unrelated console code).
"""
import argparse
import difflib
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0108-fuxi-gpu-scanout.patch'
KICK_BEFORE = '''void fuxi_display_kick(void)
{
    if (fuxi_intf1 == NULL || (fuxi_intf1[0x2b4 / 4] & 0x80000000u)) {
        __asm__ volatile("dsb sy" ::: "memory");
        return;
    }
    if (fuxi_ctl0 != NULL && !(fuxi_ctl0[0x1c / 4] & 1)) {
        __asm__ volatile("dsb sy" ::: "memory");
        fuxi_ctl0[0x1c / 4] = 1;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}
'''
# First line of fuxi_display_kick in pe_serial.c after 0003/0012/0082/0087.
# git apply anchors a hunk that starts at line 1 to the top of the file, so the
# fragment diff must carry the function's real position.
KICK_LINE = 730


def shift(diff, offset):
    out = []
    for line in diff.splitlines(True):
        if line.startswith('@@ '):
            _, old, new, rest = line.split(' ', 3)
            a, b = old[1:].split(','); c, d = new[1:].split(',')
            line = f'@@ -{int(a)+offset},{b} +{int(c)+offset},{d} {rest}'
        out.append(line)
    return ''.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('unixnu_sync', ROOT / 'tools/sync-fuxi-unixnu.py')
    sync = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sync)
    surface = 'iokit/Kernel/arm/SweetSurface.cpp'
    console = 'pexpert/arm/pe_serial.c'
    sync.PATCH = PATCH
    header = 'iokit/Kernel/arm/SweetSurface.h'
    sync.FILES = {surface: 'SweetSurface.cpp', header: 'SweetSurface.h'}
    before = sync.baseline()
    before[console] = KICK_BEFORE
    after = {surface: (ROOT / 'xnu/fuxi-scanout/SweetSurface.cpp').read_text(),
             header: (ROOT / 'xnu/fuxi-scanout/SweetSurface.h').read_text(),
             console: (ROOT / 'xnu/fuxi-scanout/display-kick.c').read_text()}
    expected = ''.join(shift(''.join(difflib.unified_diff(before[name].splitlines(True),
        after[name].splitlines(True), fromfile='a/' + name, tofile='b/' + name)),
        KICK_LINE - 1 if name == console else 0) for name in before)
    if args.check:
        assert PATCH.read_text() == expected, 'GPU-SCANOUT differs; run tools/sync-fuxi-scanout.py'
    else:
        PATCH.write_text(expected)
    with tempfile.TemporaryDirectory(prefix='fuxi-scanout-patch-') as tmp:
        for name, value in before.items():
            dest = Path(tmp) / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text('\n' * (KICK_LINE - 1) + value if name == console else value)
        subprocess.run(['git', 'apply', '--check', '-'], input=expected, text=True, cwd=tmp, check=True)
    print('PASS: GPU-SCANOUT sources match 0108; preceding surface stack and overlay apply')


if __name__ == '__main__':
    main()
