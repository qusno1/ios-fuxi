#!/usr/bin/env python3
"""Compile the actual SweetSurface cache with ownership/failure stubs (offline)."""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('submit', ROOT / 'tools/test-fuxi-adreno.py')
submit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(submit)

PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#define FG_CACHE_SLOTS 4u
#define bzero(p,n) memset(p,0,n)
#define OSDynamicCast(type,p) ((p)->buffer ? (p) : nullptr)
#define kIODirectionOut 1
#define kIOReturnSuccess 0
#define kIOPreparationIDAlwaysPrepared 2
struct fg_times {
 uint64_t pin, lookup, map, clean, submit, wait, unmap, cpu;
 unsigned attempts, hits, misses, sleeps, blocks, groups;
};
static uint64_t ticks;
static uint64_t mach_absolute_time() { return ++ticks; }
extern "C" uint64_t thread_get_runtime_self() { return ++ticks; }
struct IOLock { bool held; };
#define THREAD_UNINT 0
static void IOLockSleep(IOLock *,void *,int);
static IOLock *IOLockAlloc() { return new IOLock{false}; }
static void IOLockFree(IOLock *p) { delete p; }
static void IOLockLock(IOLock *p) { assert(!p->held); p->held=true; }
static void IOLockWakeup(IOLock *p,void *,bool) { assert(p->held); }
static void IOLockUnlock(IOLock *p) { assert(p->held); p->held=false; }
static bool OSCompareAndSwapPtr(IOLock *old,IOLock *p,IOLock **dst) {
 if(*dst!=old) return false;
 *dst=p;return true;
}
struct IOMemoryDescriptor {
 bool buffer=true, fail_prepare=false;
 unsigned refs=1, prepared=0, length=100000;
 uint64_t pageGeneration=100;
 uint64_t getPreparationID() { return pageGeneration; }
 uint64_t getLength() { return length; }
 void retain() { refs++; }
 void release() { assert(refs>1);refs--; }
 int prepare(int d) { assert(d==1);if(fail_prepare) return 1;prepared++;return 0; }
 void complete(int d) { assert(d==1 && prepared);prepared--; }
};
struct SweetSurface {
 unsigned refs=1, gpuWriters=0; bool gpuQuarantined=false; uint64_t gpuReadyTime=0;
 void retain() { refs++; }
 void release() { assert(refs>1);refs--; }
 IOMemoryDescriptor *memory; unsigned offset=0,pixelFormat=0x42475241,bytesPerRow=4352; };
static unsigned maps,unmaps,copies;
static void (*copyHook)();
static bool ready=true, fail_map=false, fail_unmap=false;
static int copy_result=2, poll_result=1;
static int fuxi_gpu_poll(fg_times *) { return poll_result; }
static uint64_t generations[4];
static int fuxi_gpu_ready() { return ready; }
static int fuxi_gpu_map(unsigned slot,uint64_t generation,uintptr_t,uint64_t,fg_times *t) {
 assert(slot<4 && generation && !generations[slot]); maps++;
 if(fail_map) { ready=false; return 0; }
 generations[slot]=generation;t->misses++;return 1;
}
static int fuxi_gpu_unmap(unsigned slot,fg_times *t) {
 assert(slot<4);unmaps++;t->unmap++;generations[slot]=0;
 if(fail_unmap) ready=false;
 return !fail_unmap;
}
static int fuxi_gpu_copy(unsigned slot,uint64_t generation,uintptr_t,uint64_t,
 unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,fg_times *) {
 assert(generations[slot]==generation);copies++;if(copyHook)copyHook();
 if(copy_result!=2) ready=false;
 return copy_result;
}
'''
MODEL = r'''
static void IOLockSleep(IOLock *p,void *,int mode) {
 assert(!mode); IOLockUnlock(p);uint64_t elapsed=0;
 assert(sweet_surface_gpu_poll(&elapsed)==1);IOLockLock(p);
}

static int copy(SweetSurface &s) {
 int r=sweet_surface_gpu_copy(&s,0x10000,0,0,0,0,1080,10);
 if(r!=2) return r;
 uint64_t elapsed=0; assert(sweet_surface_gpu_poll(&elapsed)==1 && elapsed);
 return 1;
}
static void balanced(IOMemoryDescriptor &m) { assert(m.refs==1 && !m.prepared); }
int main() {
 IOMemoryDescriptor mem[8];
 SweetSurface s[8];for(unsigned i=0;i<8;i++) s[i].memory=&mem[i];
 for(unsigned i=0;i<4;i++) assert(copy(s[i])==1);
 assert(maps==4 && !unmaps);
 for(unsigned i=0;i<1000;i++) assert(copy(s[i%4])==1);
 assert(maps==4 && !unmaps && sweet_gpu_times.hits==1000);
 for(unsigned i=0;i<4;i++) assert(mem[i].refs==2 && mem[i].prepared==1);
 assert(copy(s[4])==1 && maps==5 && unmaps==1);balanced(mem[0]);
 uint64_t old_generation=sweet_gpu_generation;
 s[4].offset=4;assert(copy(s[4])==1);
 assert(maps==6 && unmaps==2 && sweet_gpu_generation>old_generation);
 assert(mem[4].refs==2 && mem[4].prepared==1); /* replacement, no duplicate */
 mem[4].length--;assert(copy(s[4])==1 && maps==7 && unmaps==3);
 mem[4].pageGeneration++;assert(copy(s[4])==1 && maps==8 && unmaps==4);
 sweet_gpu_release(&s[4]);balanced(mem[4]);
 for(unsigned i=1;i<4;i++) {sweet_gpu_release(&s[i]);balanced(mem[i]);}
 assert(unmaps==8);
 mem[5].buffer=false;assert(copy(s[5])==0 && maps==8);balanced(mem[5]);
 mem[5].buffer=true;mem[5].fail_prepare=true;
 assert(copy(s[5])==0 && maps==8);balanced(mem[5]);
 mem[5].fail_prepare=false;mem[5].pageGeneration=1;
 assert(copy(s[5])==0 && maps==8);balanced(mem[5]);
 mem[5].pageGeneration=100;fail_map=true;
 assert(copy(s[5])==0 && maps==9 && unmaps==9);balanced(mem[5]);
 ready=true;fail_map=false;
 assert(copy(s[6])==1);copy_result=-1;
 assert(copy(s[6])==-1);
 assert(s[6].refs==2 && s[6].gpuQuarantined && !sweet_gpu_writer(&s[6],true));
 sweet_gpu_release(&s[6]);
 assert(mem[6].refs==2 && mem[6].prepared==1); /* late DMA quarantine */
 unsigned before=maps;
 assert(copy(s[7])==0 && maps==before);balanced(mem[7]);
 ready=true;copy_result=2;assert(copy(s[7])==1);
 fail_unmap=true;sweet_gpu_release(&s[7]);
 assert(mem[7].refs==2 && mem[7].prepared==1); /* unmap sync + drain failure */
 fg_times t;sweet_surface_gpu_stats(&t);
 assert(t.attempts && t.hits==1001 && t.cpu && !sweet_gpu_times.attempts);
 puts("PASS: actual C++ cache, 1000 hits, bounded LRU, replacement/generation, release, prepare/map failures and DMA quarantine");
 /* Async retains the exact source through delayed ticks; no new map/copy. */
 ready=true;fail_unmap=false;copy_result=2;
 IOMemoryDescriptor extra; SweetSurface frame; frame.memory=&extra;
 assert(sweet_surface_gpu_copy(&frame,0x10000,0,0,0,0,1080,10)==2);
 assert(frame.refs==2 && extra.refs==2 && extra.prepared==1);
 unsigned submits=copies, mapped=maps; uint64_t elapsed=0;
 poll_result=0;
 for(unsigned i=0;i<4;i++) {
  assert(!sweet_surface_gpu_poll(&elapsed) && frame.refs==2);
  assert(sweet_surface_gpu_copy(&frame,0x10000,0,0,0,0,1080,10)==2);
  assert(copies==submits && maps==mapped);
 }
 assert(!sweet_gpu_drop(sweet_gpu_slot));
 poll_result=1;assert(sweet_surface_gpu_poll(&elapsed)==1 && frame.refs==1 && elapsed);
 assert(sweet_gpu_writer(&frame,true) && frame.gpuWriters==1);
 assert(!sweet_surface_gpu_copy(&frame,0x10000,0,0,0,0,1080,10));
 assert(sweet_gpu_writer(&frame,false) && !frame.gpuWriters);
 assert(sweet_surface_gpu_copy(&frame,0x10000,0,0,0,0,1080,10)==2);
 assert(sweet_gpu_writer(&frame,true)); /* only producer waits; retires fence */
 assert(frame.refs==1 && frame.gpuWriters==1);
 assert(sweet_gpu_writer(&frame,false));
 sweet_gpu_release(&frame);balanced(extra);
 puts("PASS: async retained source, four busy ticks, no resubmit/unmap, fence release");
 IOLockFree(sweet_gpu_lock);
}
'''


def main():
    patch = (ROOT / 'xnu/patches/0096-fuxi-adreno.patch').read_text()
    block = re.search(r'^--- a/iokit/Kernel/arm/SweetSurface.cpp\n.*?(?=^--- a/|\Z)', patch, re.M | re.S)[0]
    added = ''.join(line[1:] for line in block.splitlines(True) if line.startswith('+') and not line.startswith('+++'))
    declarations = added[added.index('struct sweet_gpu_backing'):added.index('static bool sweet_gpu_init')]
    functions = ''.join(submit.function(added, n) for n in (
        'sweet_gpu_init', 'sweet_gpu_drop', 'sweet_gpu_release', 'sweet_surface_gpu_copy', 'sweet_surface_gpu_poll', 'sweet_gpu_writer', 'sweet_surface_gpu_stats'))
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'cache.cc'
        src.write_text(PREFIX + declarations + functions + MODEL)
        exe = Path(tmp) / 'cache'
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
