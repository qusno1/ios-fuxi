#!/usr/bin/env python3
"""Generate/check 0111 from canonical profiler sources and replay its hooks."""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'xnu/patches/0111-fuxi-user-profiler.patch'
FILES = ('bsd/conf/files', 'bsd/dev/sweet_ufs.c', 'osfmk/conf/files')
SOURCES = {'bsd/dev/sweet_prof.c': 'sweet_prof.c', 'osfmk/kern/sweet_prof_sample.c': 'sweet_prof_sample.c',
           'osfmk/kern/sweet_prof_logic.h': 'sweet_prof_logic.h'}


def baseline():
    with tempfile.TemporaryDirectory(prefix='fuxi-prof-stack-') as tmp:
        # Only the device build-list region is seeded; pre-0028 deltas touch
        # unrelated pthread/HFS entries. UFS is reconstructed in full.
        dest = Path(tmp) / FILES[0]
        dest.parent.mkdir(parents=True)
        dest.write_text('\n' * 103 + 'bsd/dev/random/randomdev.c\t\tstandard\n\n'
                        'bsd/dev/memdev.c\t\t\tstandard\nbsd/dev/mem.c\t\t\t\tstandard\n'
                        'bsd/dev/munge.c \t\t\toptional config_requires_u32_munging\n'
                        'bsd/dev/unix_startup.c\t\t\tstandard\n')
        dest = Path(tmp) / FILES[2]
        dest.parent.mkdir(parents=True)
        dest.write_text('\n' * 122 + 'osfmk/kern/debug.c\t\t\tstandard\n'
                        'osfmk/kern/ecc_logging.c\t\t\toptional config_ecc_logging\n'
                        'osfmk/kern/energy_perf.c\t\tstandard\n'
                        'osfmk/kern/exception.c\t\tstandard\n'
                        'osfmk/kern/extmod_statistics.c\t\tstandard\n'
                        'osfmk/kern/host.c\t\t\tstandard\n'
                        'osfmk/kern/host_notify.c\t\tstandard\n')
        for patch in sorted(PATCH.parent.glob('*.patch')):
            if patch.name >= PATCH.name:
                break
            for chunk in re.split(r'(?=^--- (?:a/|/dev/null))', patch.read_text(errors='surrogateescape'), flags=re.M):
                match = re.search(r'^\+\+\+ b/(.+)$', chunk, re.M)
                if match and match[1] in FILES:
                    if match[1] == FILES[0] and patch.name < '0028':
                        continue
                    # A following diff --git header is not part of this delta.
                    chunk = re.split(r'^diff --git ', chunk, flags=re.M)[0]
                    subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'], input=chunk,
                                   text=True, cwd=tmp, check=True)
        return {name: (Path(tmp) / name).read_text() for name in FILES}


def expected(before):
    after = dict(before)
    anchor = 'bsd/dev/sweet_sample.c\t\t\tstandard\n'
    assert after[FILES[0]].count(anchor) == 1
    after[FILES[0]] = after[FILES[0]].replace(anchor, anchor + 'bsd/dev/sweet_prof.c\t\t\tstandard\n')
    anchor = 'extern void sweet_sample_start(void);\n'
    assert after[FILES[1]].count(anchor) == 1
    after[FILES[1]] = after[FILES[1]].replace(anchor, anchor + 'extern void sweet_prof_start(void);\n')
    anchor = '\tchar smp[8];\n'
    assert after[FILES[1]].count(anchor) == 1
    after[FILES[1]] = after[FILES[1]].replace(anchor, '\tsweet_prof_start();\n' + anchor)
    anchor = 'osfmk/kern/exception.c\t\tstandard\n'
    assert after[FILES[2]].count(anchor) == 1
    after[FILES[2]] = after[FILES[2]].replace(anchor, anchor + 'osfmk/kern/sweet_prof_sample.c\t\tstandard\n')
    for path, name in SOURCES.items():
        after[path] = (ROOT / 'xnu/fuxi-prof' / name).read_text()
    return ''.join(''.join(difflib.unified_diff(before.get(name, '').splitlines(True), text.splitlines(True),
                    fromfile='a/' + name if name in before else '/dev/null', tofile='b/' + name))
                   for name, text in after.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    before = baseline()
    patch = expected(before)
    if args.check:
        assert PATCH.read_text() == patch, 'run tools/sync-fuxi-prof.py'
    else:
        PATCH.write_text(patch)
    with tempfile.TemporaryDirectory(prefix='fuxi-prof-check-') as tmp:
        for name, text in before.items():
            dest = Path(tmp) / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(text)
        subprocess.run(['git', 'apply', '--check', '-'], input=patch, text=True, cwd=tmp, check=True)
        subprocess.run(['git', 'apply', '-'], input=patch, text=True, cwd=tmp, check=True)
        for path, name in SOURCES.items():
            assert (Path(tmp) / path).read_bytes() == (ROOT / 'xnu/fuxi-prof' / name).read_bytes()
    print('PASS: profiler canonical sources match 0111; preceding hook patches and 0111 apply')


if __name__ == '__main__':
    main()
