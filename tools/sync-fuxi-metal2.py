#!/usr/bin/env python3
"""Check/regenerate 0100's canonical exec helper and ABI tracing delta.

The kern_exec.c hunks are reviewed directly; 0098 stays independently usable.
"""
import argparse
import difflib
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
PATCH=ROOT/'xnu/patches/0100-sweet-execswap-gpu-abi-trace.patch'
CLIENT='iokit/Kernel/arm/FuxiAdrenoUserClient.cpp'
HELPER='bsd/kern/sweet_execswap.h'
INDEX='iokit/Kernel/arm/FuxiHandleIndex.h'

def old_client():
    patch=(ROOT/'xnu/patches/0098-fuxi-native-adreno-abi.patch').read_text()
    block=next(p for p in re.split(r'(?=^--- a/)',patch,flags=re.M) if p.startswith(f'--- a/{CLIENT}\n'))
    return ''.join(line[1:] for line in block.splitlines(True) if line.startswith('+') and not line.startswith('+++'))

def delta():
    result=''
    for target,old,new in [(HELPER,'',(ROOT/'xnu/fuxi-exec/sweet_execswap.h').read_text()),
            (INDEX,'',(ROOT/'xnu/fuxi-adreno/FuxiHandleIndex.h').read_text()),
            (CLIENT,old_client(),(ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text())]:
        result+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+target,tofile='b/'+target))
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--check',action='store_true');args=ap.parse_args()
    text=PATCH.read_text(); prefix=text[:text.index('--- a/'+HELPER)]
    expected=prefix+delta()
    if args.check: assert text==expected,'0100 differs; run sync-fuxi-metal2.py'
    else: PATCH.write_text(expected)
    print('PASS: 0100 canonical exec helper and ABI trace/guard delta')
if __name__=='__main__': main()
