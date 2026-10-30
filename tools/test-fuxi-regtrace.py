#!/usr/bin/env python3
"""Execute the registry tracer's actual C/C++ with mocked process, time and locks."""
import argparse
import importlib.util
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-prof'
MOCK = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdlib.h>
#define FUXI 1
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define LCK_GRP_DECLARE(n,s) int n __unused
#define LCK_MTX_DECLARE(n,g) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define os_atomic_load(p,o) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define os_atomic_store(p,v,o) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define os_atomic_cmpxchg(p,old,v,o) ({__typeof__(*(p)) expected=(old);__atomic_compare_exchange_n(p,&expected,(v),false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);})
#define PZERO 0
#define KERN_SUCCESS 0
int hz=100;
static _Thread_local unsigned locked;
static uint64_t ticks=1000,step;
static unsigned names,clocks,parses,boot_present,boot_value,starts,refs,fail,logs,sleeps;
static const char *process="backboardd";
static char logline[8192];
static jmp_buf worker_done;
typedef int wait_result_t,thread_t;
static void lck_mtx_lock(pthread_mutex_t *m){assert(!locked);pthread_mutex_lock(m);locked=1;}
static void lck_mtx_unlock(pthread_mutex_t *m){assert(locked);locked=0;pthread_mutex_unlock(m);}
#define strlcpy model_strlcpy
static size_t strlcpy(char *out,const char *s,size_t n){size_t len=strlen(s);if(n){size_t c=len<n-1?len:n-1;memcpy(out,s,c);out[c]=0;}return len;}
static uint64_t mach_absolute_time(void){__atomic_fetch_add(&clocks,1,__ATOMIC_RELAXED);return __atomic_fetch_add(&ticks,step,__ATOMIC_RELAXED);}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
static void proc_selfname(char *n,int len){names++;strlcpy(n,process,(size_t)len);}
static int PE_parse_boot_argn(const char *name,void *out,int len){assert(!strcmp(name,"fuxiregtrace") && len==4);parses++;*(unsigned *)out=boot_value;return boot_present;}
static int kernel_thread_start(void (*f)(void *,int),void *arg,thread_t *t){assert(f && !arg);starts++;if(fail)return 1;*t=1;refs++;return 0;}
static void thread_deallocate(thread_t t){assert(t==1 && refs);refs--;}
static int tsleep(void *event,int pri,const char *name,int timeout){assert(event && !pri && !strcmp(name,"fuxiregtrace") && timeout==10*hz && !locked);if(sleeps++)longjmp(worker_done,1);ticks+=10000000000ull;return 0;}
static int model_printf(const char *fmt,...){assert(!locked);logs++;va_list ap;va_start(ap,fmt);int n=vsnprintf(logline,sizeof(logline),fmt,ap);va_end(ap);assert(n>0 && (size_t)n<sizeof(logline));return n;}
#define printf model_printf
'''
TEST = r'''
static void reset(void){assert(!locked && !refs);memset(frt_rows,0,sizeof(frt_rows));memset(&frt_work,0,sizeof(frt_work));memset(&frt_drop,0,sizeof(frt_drop));frt_enabled=frt_parsed=0;names=clocks=parses=starts=logs=sleeps=0;boot_present=boot_value=fail=0;ticks=1000;step=0;process="backboardd";}
static void add(const char *cls,const char *key,const char *plane,unsigned options,unsigned api,uint64_t dt,uint64_t bytes){
 struct frt_token t={.start=ticks,.last=ticks};t.bytes=bytes;ticks+=dt;fuxi_regtrace_end(&t,cls,key,plane,options,api);
}
static unsigned occupied(void){unsigned n=0;for(unsigned i=0;i<FRT_BUCKETS;i++)n+=!!frt_rows[i].time.n;return n;}
static struct frt_row *find(const char *key){for(unsigned i=0;i<FRT_BUCKETS;i++)if(frt_rows[i].time.n && !strcmp(frt_rows[i].key,key))return &frt_rows[i];assert(0);return NULL;}
static void *stress(void *arg){(void)arg;for(unsigned i=0;i<10000;i++){struct frt_token t={.start=mach_absolute_time()};t.last=t.start;fuxi_regtrace_end(&t,"AppleCLCD","concurrent","IOService",0,32);}return NULL;}
int main(void){
 reset();assert(!fuxi_regtrace_begin() && !names && !clocks && !parses);
 for(unsigned present=0;present<2;present++)for(unsigned value=0;value<4;value++){
  reset();boot_present=present;boot_value=value;fuxi_regtrace_start();fuxi_regtrace_start();
  assert(parses==1 && starts==(present && value==1) && frt_enabled==(present && value==1));assert(!refs && !logs && !names);
 }
 reset();boot_present=boot_value=fail=1;fuxi_regtrace_start();assert(starts==1 && !frt_enabled && !refs);
 reset();frt_enabled=1;
 for(unsigned i=0;i<4;i++){process=i==0?"backboard":i==1?"backboardd-helper":i==2?"Backboardd":"SpringBoard";assert(!fuxi_regtrace_begin());}assert(!clocks);
 process="backboardd";assert(fuxi_regtrace_begin()==1000 && clocks==1);
 struct frt_token t={.start=ticks,.last=ticks};ticks+=3000;fuxi_regtrace_phase(&t,1);ticks+=5000;fuxi_regtrace_phase(&t,2);ticks+=7000;t.bytes=444;
 fuxi_regtrace_end(&t,"AppleCLCD","value","IOService",0,32);struct frt_row *r=find("value");assert(r->time.n==1 && r->time.total==15000 && r->time.max==15000 && r->bytes==444 && r->stage[0]==3000 && r->stage[1]==5000 && r->stage[2]==7000);
 add("AppleCLCD","value","IOService",0,2,1000,200);r=find("value");assert(r->time.n==2 && r->apis==34 && r->time.max==15000 && r->bytes==444);
 add("Other","value","IOService",0,32,1000,1);add("AppleCLCD","value","IOService",1,32,1000,1);add("AppleCLCD","value","IODeviceTree",0,32,1000,1);assert(occupied()==4 && !logs);
 uint64_t wi=ticks;ticks+=9000;fuxi_regtrace_work_end(&wi);wi=0;fuxi_regtrace_work_end(&wi);assert(frt_work.n==1 && frt_work.total==9000);
 reset();frt_enabled=1;
 /* Total, not call count or max, selects the top eight; multiple APIs merge. */
 for(unsigned i=1;i<=12;i++){char key[32];snprintf(key,sizeof(key),"key%02u",i);for(unsigned j=0;j<i;j++)add("AppleCLCD",key,"IOService",0,32,1000,4*i);}
 for(unsigned j=0;j<100;j++)add("AppleCLCD","frequent","IOService",0,1,1,2);
 frt_report(10000000000ull);assert(logs==1 && !occupied());
 for(unsigned i=0;i<8;i++){char key[32];snprintf(key,sizeof(key),"key%02u",12-i);assert(!strcmp(frt_top[i].key,key) && frt_top[i].time.total==(12-i)*1000);}
 assert(strstr(logline,"span_us=10000000 dropped=0/0/0 wi=0/0/0") && strstr(logline,"n=12 total_us=12 max_us=1 bytes=48 lookup_us=12 serialize_us=0 copy_us=0]"));
 frt_report(1000);assert(!frt_top[0].time.n && logs==2 && !strchr(logline,'['));
 reset();
 for(unsigned i=0;i<FRT_BUCKETS;i++){char key[32];snprintf(key,sizeof(key),"key%u",i);add("AppleCLCD",key,"IOService",0,32,1000,8);}
 assert(occupied()==FRT_BUCKETS);add("AppleCLCD","overflow","IOService",0,32,2000,8);assert(occupied()==FRT_BUCKETS && frt_drop.n==1 && frt_drop.total==2000);
 add("AppleCLCD","key0","IOService",0,32,3000,16);assert(find("key0")->time.n==2 && frt_drop.n==1);
 char longkey[FRT_NAME+1];memset(longkey,'a',sizeof(longkey));longkey[FRT_NAME]=0;add("AppleCLCD",longkey,"IOService",0,32,1000,8);assert(frt_drop.n==2);
 frt_report(10000000000ull);assert(strstr(logline,"dropped=2/3/2"));
 reset();char name[FRT_NAME];memset(name,'x',sizeof(name));name[FRT_NAME-1]=0;
 for(unsigned i=0;i<FRT_TOP;i++){name[0]=(char)('a'+i);add(name,name,name,0xffffffff,32,UINT64_MAX/32,UINT64_MAX);}
 frt_report(1000);assert(strlen(logline)<sizeof(logline)-2);unsigned brackets=0;for(char *c=logline;*c;c++)brackets+=*c=='[';assert(brackets==FRT_TOP);
 reset();add("Apple CLCD","unsafe\n[key]=x","",0,32,1000,8);frt_report(1000);assert(strstr(logline,"class=Apple_CLCD key=unsafe__key__x plane=-"));
 reset();step=1000;pthread_t a,b;assert(!pthread_create(&a,NULL,stress,NULL) && !pthread_create(&b,NULL,stress,NULL));pthread_join(a,NULL);pthread_join(b,NULL);assert(find("concurrent")->time.n==20000 && occupied()==1);
 reset();frt_enabled=1;uint64_t start=fuxi_regtrace_begin();ticks+=2000;fuxi_regtrace_work_end(&start);
 if(!setjmp(worker_done))frt_worker(NULL,0);assert(logs==1 && sleeps==2 && strstr(logline,"wi=1/2/2"));
#undef printf
 puts("PASS: actual regtrace C: once-only/off gate, exact process filter, phases, API merge, top8 by total, bounded/full table, overflow, size max, sanitization, 10s worker, concurrent calls, unlocked output");
}
'''
SCOPE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <kern/fuxi_regtrace.h>
struct Meta {const char *getClassName(){return "AppleCLCD";}};
struct IORegistryEntry {Meta m;Meta *getMetaClass(){return &m;}};
static unsigned enabled,finished,phases,work_finished;
extern "C" uint64_t fuxi_regtrace_begin(void){return enabled?10:0;}
extern "C" void fuxi_regtrace_phase(struct frt_token *t,unsigned p){assert(t->start==10 && p==1);phases++;}
extern "C" void fuxi_regtrace_end(struct frt_token *t,const char *c,const char *k,const char *p,uint32_t o,unsigned a){assert(t->start==10 && c && k && p && o==0 && a==32);finished++;}
extern "C" void fuxi_regtrace_work_end(uint64_t *t){if(*t)work_finished++;}
#include "fuxi_regtrace_iokit.h"
static int call(unsigned path){IORegistryEntry entry;FRT_SCOPE(&entry,"key","IOService",0,32);if(path==0)return -1;FRT_PHASE(1);if(path==1)return -2;FRT_SIZE(8);return 0;}
static int work(unsigned path){uint64_t start __attribute__((unused,cleanup(fuxi_regtrace_work_end)))=fuxi_regtrace_begin();if(path==0)return -1;return 0;}
int main(){for(enabled=0;enabled<2;enabled++)for(unsigned i=0;i<3;i++){call(i);work(i);}assert(finished==3 && phases==2 && work_finished==3);}
'''


def syntax(buildtree):
    config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_FUXI'
    if not config.exists():
        config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
    hooks = (SRC / 'regtrace-hooks.diff').read_text()
    with tempfile.TemporaryDirectory(prefix='fuxi-regtrace-syntax-') as tmp:
        tmp = Path(tmp)
        for n, local in (('exports/osfmk/kern/fuxi_regtrace.h', 'fuxi_regtrace.h'),
                         ('iokit/Kernel/fuxi_regtrace_iokit.h', 'fuxi_regtrace_iokit.h'),
                         ('bsd/dev/fuxi_regtrace.c', 'fuxi_regtrace.c')):
            p = tmp / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes((SRC / local).read_bytes())
        for n in ('bsd/kern/sys_work_interval.c', 'iokit/Kernel/IOUserClient.cpp'):
            p = tmp / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes((buildtree / 'xnu' / n).read_bytes())
            chunk = next(c for c in re.split(r'(?=^--- a/)', hooks, flags=re.M) if c.startswith('--- a/'+n+'\n'))
            subprocess.run(['git', 'apply', '-'], input=chunk, text=True, cwd=tmp, check=True)
        for comp, n in (('bsd', 'bsd/dev/fuxi_regtrace.c'), ('bsd', 'bsd/kern/sys_work_interval.c'), ('iokit', 'iokit/Kernel/IOUserClient.cpp')):
            cwd = config / comp / 'DEVELOPMENT'
            old = re.search(r'^SOURCE_DIR=(.+)/xnu$', (cwd / 'Makefile').read_text(), re.M)[1]
            raw = (cwd / '.CFLAGS').read_text().replace('\0', '')
            raw = re.sub(r'(-DCONFIG_NMBCLUSTERS=\(\(1024 \* 512\) / MCLBYTES\))', lambda m: shlex.quote(m[1]), raw)
            if comp == 'iokit':
                raw += ' ' + (cwd / '.CXXFLAGS').read_text().replace('\0', '').split(' ', 1)[1]
            flags, skip = [], False
            for f in shlex.split(raw)[1:]:
                if skip:
                    skip = False
                    continue
                if f == '-arch':
                    skip = True
                    continue
                f = f.replace(old, str(buildtree))
                f = re.sub(r'/Applications/Xcode[^ ]*?\.sdk/+System/Library/Frameworks/Kernel\.framework/Versions/A/PrivateHeaders/platform', str(buildtree / 'sdk-kernel-platform/platform'), f)
                flags.append(f)
            subprocess.run(['clang++' if comp == 'iokit' else 'clang', '--target=arm64-apple-macos14', '-fsyntax-only', '-DFUXI', '-Werror',
                            '-D__builtin_xnu_type_signature(...)=""', '-D__builtin_xnu_type_summary(...)=0', '-D__builtin_xnu_types_compatible(...)=1',
                            '-I'+str(tmp / 'exports/osfmk'), '-I'+str(buildtree / 'xnu' / Path(n).parent), *flags,
                            '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-invalid-offsetof',
                            '-Wno-duplicate-decl-specifier', str(tmp / n)], cwd=cwd, check=True)
            print('PASS: ARM64 full-file syntax: '+n)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu-buildtree', type=Path)
    args = ap.parse_args()
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-regtrace.py'), '--check'], check=True)
    source = re.sub(r'^#include <[^>]+>\n', '', (SRC / 'fuxi_regtrace.c').read_text(), flags=re.M)
    hooks = (SRC / 'regtrace-hooks.diff').read_text()
    assert hooks.count('+\tFRT_SCOPE(') == 6 and hooks.count('+\t\tFRT_SIZE(len);') == 4
    assert '+\tfuxi_regtrace.h \\' in hooks and 'XNU_ONLY_EXPORTS' in hooks
    assert 'cleanup(fuxi_regtrace_work_end)' in hooks and '+\tfuxi_regtrace_start();' in hooks
    assert not re.search(r'\b(?:kalloc|IOMalloc|malloc|calloc)\s*\(', source)
    with tempfile.TemporaryDirectory(prefix='fuxi-regtrace-test-') as tmp:
        tmp = Path(tmp)
        (tmp / 'kern').mkdir()
        (tmp / 'kern/fuxi_regtrace.h').write_bytes((SRC / 'fuxi_regtrace.h').read_bytes())
        (tmp / 'trace.c').write_text(MOCK + '#include <kern/fuxi_regtrace.h>\n' + source + TEST)
        (tmp / 'scope.cpp').write_text(SCOPE)
        for file, compiler, std in (('trace.c', 'clang', 'gnu11'), ('scope.cpp', 'clang++', 'gnu++14')):
            subprocess.run([compiler, '-std='+std, '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-fsanitize=address,undefined', '-DFUXI', '-I'+str(tmp), '-I'+str(SRC), str(tmp / file), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)
        print('PASS: C++ RAII and syscall cleanup cover every early return; disabled scopes record nothing')
    if args.xnu_buildtree:
        syntax(args.xnu_buildtree.resolve())


if __name__ == '__main__':
    main()
