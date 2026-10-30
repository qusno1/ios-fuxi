#!/usr/bin/env python3
"""Generate/check 0112 from profiler registry-trace sources and reviewed hooks.

--tree is an optional XNU tree with patches through 0111 applied.
"""
import argparse
import difflib
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-prof'
PATCH = ROOT / 'xnu/patches/0112-fuxi-registry-trace.patch'
SOURCES = {'bsd/dev/fuxi_regtrace.c': 'fuxi_regtrace.c',
           'osfmk/kern/fuxi_regtrace.h': 'fuxi_regtrace.h',
           'iokit/Kernel/fuxi_regtrace_iokit.h': 'fuxi_regtrace_iokit.h'}


def expected():
    return (SRC / 'regtrace-hooks.diff').read_text() + ''.join(''.join(difflib.unified_diff(
        [], (SRC / local).read_text().splitlines(True), fromfile='/dev/null', tofile='b/'+name))
        for name, local in SOURCES.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--tree', type=Path)
    args = ap.parse_args()
    patch = expected()
    if args.check:
        assert PATCH.read_text() == patch, 'run tools/sync-fuxi-regtrace.py'
    else:
        PATCH.write_text(patch)
    if args.tree:
        subprocess.run(['git', 'apply', '--check', '-'], input=patch, text=True, cwd=args.tree, check=True)
    print('PASS: registry-trace canonical sources/hooks match 0112' + ('; 0112 applies' if args.tree else ''))


if __name__ == '__main__':
    main()
