#!/usr/bin/env python3
"""CA-like portable frontend CPU model; excludes Objective-C dispatch and real GPU/driver work."""
import argparse
from collections import Counter
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--frames',type=int,default=20000)
    ap.add_argument('--baseline-ref',help='git revision with the pre-optimization TestBackend.hpp')
    ap.add_argument('--profile',action='store_true',help='1 kHz SIGPROF PC sampling (Linux x86_64; perf permissions unnecessary)')
    a=ap.parse_args()
    if a.frames<=0:ap.error('--frames must be positive')
    with tempfile.TemporaryDirectory(prefix='fuxi-encoding-') as tmp:
        tmp=Path(tmp)
        variants=[('after',ROOT/'tools/fuxi-metal',[])]
        if a.baseline_ref:
            archive=subprocess.run(['git','archive',a.baseline_ref,'tools/fuxi-metal'],cwd=ROOT,check=True,capture_output=True).stdout
            subprocess.run(['tar','-x','-C',str(tmp)],input=archive,check=True)
            variants.insert(0,('before',tmp/'tools/fuxi-metal',['-DFUXI_MTL_ENCODING_BASELINE']))
        for label,headers,flags in variants:
            exe=tmp/label
            subprocess.run(['clang++','-std=c++17','-O2','-g','-Wall','-Wextra','-Werror','-pthread','-fno-omit-frame-pointer','-no-pie','-rdynamic',*flags,'-I',str(headers),str(ROOT/'tools/fuxi-metal/FrontendEncodingBench.cpp'),'-ldl','-o',str(exe)],check=True)
            for run in range(3):
                result=subprocess.run([str(exe),str(a.frames)],check=True,capture_output=True,text=True)
                print(label,f'run={run+1}',result.stdout.strip(),flush=True)
            if a.profile:
                result=subprocess.run([str(exe),str(a.frames),'--profile'],check=True,capture_output=True,text=True)
                pcs=[line.split() for line in result.stdout.splitlines() if line.startswith('PC ')]
                resolved=subprocess.run(['addr2line','-f','-C','-e',str(exe),*[p[1] for p in pcs]],check=True,capture_output=True,text=True).stdout.splitlines()
                counts=Counter()
                for i,p in enumerate(pcs):counts[resolved[2*i] if resolved[2*i]!='??' else p[3]]+=int(p[2])
                total=sum(counts.values());print(f'{label} SIGPROF samples={total}')
                for function,count in counts.most_common(12):print(f'  {100*count/max(1,total):5.1f}% {function}')
if __name__=='__main__':main()
