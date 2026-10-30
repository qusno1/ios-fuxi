#!/usr/bin/env python3
"""Generate/check H1's overlay against the preceding patch stack (no XNU checkout)."""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0103-fuxi-kernel-h1.patch'
FILES = {
    'bsd/kern/sweet_amfi.c': 'sweet_amfi.c',
    'bsd/dev/fuxi_keys.c': 'fuxi_keys.c',
    'bsd/dev/sweet_keys.c': 'sweet_keys.c',
    'bsd/dev/sweet_spmi.c': 'sweet_spmi.c',
    'bsd/dev/fuxi_touch.c': 'fuxi_touch.c',
    'iokit/Kernel/arm/hid/SweetHID.cpp': 'SweetHID.cpp',
    'iokit/Kernel/arm/hid/LICENSE.NativeHidInput': 'LICENSE.NativeHidInput',
    'bsd/kern/fuxi_amfi.h': 'fuxi_amfi.h',
    'bsd/dev/fuxi_key_state.h': 'fuxi_key_state.h',
    'bsd/dev/sweet_spmi_bus.h': 'sweet_spmi_bus.h',
}


def baseline():
    """Use git apply, including context validation, for every earlier relevant delta."""
    with tempfile.TemporaryDirectory(prefix='fuxi-h1-stack-') as tmp:
        for patch in sorted(PATCH.parent.glob('*.patch')):
            if patch.name >= PATCH.name:
                break
            chunks = re.split(r'(?=^--- (?:a/|/dev/null))', patch.read_text(errors='surrogateescape'), flags=re.M)
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
        before[name].splitlines(True), (ROOT / 'xnu/fuxi-h1' / local).read_text().splitlines(True),
        fromfile='a/' + name if before[name] else '/dev/null', tofile='b/' + name))
        for name, local in FILES.items())
    if args.check:
        assert PATCH.read_text() == expected, 'H1 sources differ; run tools/sync-fuxi-h1.py'
    else:
        PATCH.write_text(expected)
    # Verify the generated overlay really applies after the input stack.
    with tempfile.TemporaryDirectory(prefix='fuxi-h1-check-') as tmp:
        for name, text in before.items():
            if text:
                dest = Path(tmp) / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(text)
        subprocess.run(['git', 'apply', '--check', '-'], input=expected, text=True,
                       cwd=tmp, check=True)
    print('PASS: H1 canonical sources match overlay; preceding patches and overlay apply')


if __name__ == '__main__':
    main()
