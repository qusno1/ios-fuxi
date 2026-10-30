#!/usr/bin/env python3
"""Generate/check patch 0113 (fuxi-audio): the portable DSP framing and RSC arbiter sources as new, unbuilt files under bsd/dev.

Canonical sources are xnu/fuxi-audio/. No build list is touched: nothing is compiled into the kernel until a driver includes them
(docs-lanes/audio.md, P-AU2). --check also verifies that no earlier patch already creates these paths.
"""
import argparse
import difflib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-audio'
PATCH = ROOT / 'xnu/patches/0113-fuxi-audio.patch'
FILES = ('fuxi_gpr.h', 'fuxi_gpr.c', 'fuxi_audioreach.h', 'fuxi_audioreach.c', 'fuxi_rpmh_arb.h')


def expected():
    return ''.join(''.join(difflib.unified_diff([], (SRC / name).read_text().splitlines(True),
                                               fromfile='/dev/null', tofile='b/bsd/dev/' + name)) for name in FILES)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    patch = expected()
    for prior in sorted(PATCH.parent.glob('*.patch')):
        if prior.name == PATCH.name:
            continue
        text = prior.read_text(errors='surrogateescape')
        for name in FILES:
            assert not re.search(r'^\+\+\+ b/bsd/dev/' + re.escape(name) + '$', text, re.M), prior.name + ' already creates ' + name
    if args.check:
        assert PATCH.read_text() == patch, 'fuxi-audio sources differ; run tools/sync-fuxi-audio.py'
    else:
        PATCH.write_text(patch)
    print('PASS: fuxi-audio canonical sources match 0113 (new files only, not in any build list)')


if __name__ == '__main__':
    main()
