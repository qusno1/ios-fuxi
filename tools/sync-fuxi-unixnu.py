#!/usr/bin/env python3
"""Generate/check P-UNIXNU's overlay after all preceding relevant patches."""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0106-fuxi-unixnu.patch'
FILES = {
    'bsd/vfs/fuxi_unixnu_lookup.h': 'fuxi_unixnu_lookup.h',
    'bsd/dev/sweet_ufs.c': 'sweet_ufs.c',
    'bsd/dev/fuxi_unixnu_ufs.h': 'fuxi_unixnu_ufs.h',
    'bsd/dev/LICENSE.NativeUfs': 'LICENSE.NativeUfs',
    'bsd/dev/LICENSE.UniXNU': 'LICENSE.UniXNU',
    'iokit/Kernel/arm/SweetFramebuffer.cpp': 'SweetFramebuffer.cpp',
    'iokit/Kernel/arm/fuxi_unixnu_copy.h': 'fuxi_unixnu_copy.h',
    'iokit/Kernel/arm/fuxi_unixnu_display.h': 'fuxi_unixnu_display.h',
    'iokit/Kernel/arm/LICENSE.NativeMobileDisplay': 'LICENSE.NativeMobileDisplay',
}


def baseline():
    """Reconstruct affected sources with real git apply context validation."""
    with tempfile.TemporaryDirectory(prefix='fuxi-unixnu-stack-') as tmp:
        for patch in sorted(PATCH.parent.glob('*.patch')):
            if patch.name >= PATCH.name:
                break
            chunks = re.split(r'(?=^--- (?:a/|/dev/null))',
                              patch.read_text(errors='surrogateescape'), flags=re.M)
            for chunk in chunks:
                match = re.search(r'^\+\+\+ b/(.+)$', chunk, re.M)
                if not match or match[1] not in FILES:
                    continue
                subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'], input=chunk,
                               text=True, cwd=tmp, check=True, capture_output=True)
        return {name: (Path(tmp) / name).read_text() if (Path(tmp) / name).exists() else ''
                for name in FILES}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    before = baseline()
    expected = ''.join(''.join(difflib.unified_diff(
        before[name].splitlines(True), (ROOT / 'xnu/fuxi-unixnu' / local).read_text().splitlines(True),
        fromfile='a/' + name if before[name] else '/dev/null', tofile='b/' + name))
        for name, local in FILES.items())
    if args.check:
        assert PATCH.read_text() == expected, 'P-UNIXNU sources differ; run tools/sync-fuxi-unixnu.py'
    else:
        PATCH.write_text(expected)
    with tempfile.TemporaryDirectory(prefix='fuxi-unixnu-check-') as tmp:
        for name, value in before.items():
            if value:
                dest = Path(tmp) / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(value)
        subprocess.run(['git', 'apply', '--check', '-'], input=expected,
                       text=True, cwd=tmp, check=True)
    print('PASS: P-UNIXNU sources match 0106; preceding patches and overlay apply')


if __name__ == '__main__':
    main()
