#!/usr/bin/env python3
"""Execute actual profiler C with mocked tasks/time; optionally syntax-check XNU TU."""
import argparse
import contextlib
import io
from unittest import mock
import importlib.util
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#define FUXI 1
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define TASK_NULL NULL
#define PROC_ALLPROCLIST 1
#define PROC_RETURNED 0
#define KERN_SUCCESS 0
#define TH_TERMINATE 8
#define TH_WAIT 1
#define TH_RUN 2
#define THREAD_UNINT 1
#define TIMEOUT_URGENCY_SYS_NORMAL 0
#define TIMEOUT_URGENCY_LEEWAY 32
#define THREAD_CONTINUE_NULL NULL
#define Z_WAITOK 1
#define Z_ZERO 2
#define NSEC_PER_SEC 1000000000u
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_USEC 1000u
struct regs { uint64_t pc,lr,fp,x16;int valid; };
typedef struct regs arm_saved_state_t;
struct thread { struct thread *next;struct regs *ss;uint64_t thread_id,wait_event;int state,wait_result,oncore; };
typedef struct thread *thread_t;
struct task {thread_t threads;uint64_t mach_header_vm_address;};
typedef struct task *task_t;
typedef void *vm_map_t;
typedef int spl_t,wait_result_t,proc_t;
struct region {uint64_t sr_slide;};
typedef struct region *vm_shared_region_t;
static struct regs regs={0x180001000,0x180002000,0x1000,(uint64_t)-31,1};
static struct thread th={NULL,&regs,42,0x123,TH_WAIT,7,0};
static struct task task={&th,0x100000000};
static struct region region={0x200000};
static unsigned locks,schedlock,high,reads,refs,allocs,frees,starts,threadrefs,output,sleeps,blocks;
static unsigned boot_present,boot_value,missing,allocfail,readfail,malformed,cost,non64;
static uint64_t ticks,wake,last_leeway;
static char logs[500][512];
static jmp_buf terminated;
static int model_printf(const char *fmt,...) {
 assert(!locks && !schedlock && output<500);va_list ap;va_start(ap,fmt);
 vsnprintf(logs[output++],512,fmt,ap);va_end(ap);return 0;
}
#define printf model_printf
static task_t proc_task(proc_t p){assert(p==1);return &task;}
static void proc_name(int pid,char *out,size_t size){assert(pid==1);snprintf(out,size,"%s",missing?"other":"backboardd");}
static int proc_pid(proc_t p){return p;}
static void proc_iterate(int which,int (*fn)(proc_t,void *),void *arg,void *filter,void *farg){assert(which==1 && !filter && !farg);fn(1,arg);}
static void task_reference(task_t t){assert(t==&task);refs++;}
static void task_deallocate(task_t t){assert(t==&task && refs);refs--;}
static void task_lock(task_t t){assert(t==&task && !locks);locks++;}
static void task_unlock(task_t t){assert(t==&task && locks && !schedlock);locks--;}
#define queue_iterate(q,t,type,field) for((t)=*(q);(t);(t)=(t)->next)
static vm_map_t get_task_map(task_t t){return t;}
static task_t current_task(void){return &task;}
static thread_t current_thread(void){return task.threads;}
static arm_saved_state_t *find_user_regs(thread_t t){assert(locks);return t->ss;}
static int is_saved_state64(arm_saved_state_t *s){return s->valid && !non64;}
static spl_t splsched(void){assert(!high);high=1;return 0;}
static void splx(spl_t s){assert(!s && high && !schedlock);high=0;}
static void thread_lock(thread_t t){assert(t && high && !schedlock);schedlock=1;}
static void thread_unlock(thread_t t){assert(t && schedlock);schedlock=0;}
static int machine_thread_on_core(thread_t t){assert(schedlock);return t->oncore;}
static uint64_t get_saved_state_pc(arm_saved_state_t *s){assert(!high);return s->pc;}
static uint64_t get_saved_state_lr(arm_saved_state_t *s){return s->lr;}
static uint64_t get_saved_state_fp(arm_saved_state_t *s){return s->fp;}
static uint64_t get_saved_state_reg(arm_saved_state_t *s,int n){assert(n==16);return s->x16;}
static int vm_map_read_user(vm_map_t map,uint64_t fp,void *out,size_t size){
 assert(map==&task && locks && !high && !schedlock && size==16 && !(fp&7));reads++;
 if(readfail && reads==readfail)return 1;
 uint64_t *frame=out;frame[0]=malformed==1?fp:malformed==2?fp-16:malformed==3?fp+1024*1024:fp+16;
 frame[1]=0x180003000+(fp-0x1000);ticks+=cost;return 0;
}
static void *get_bsdthread_info(thread_t t){return t;}
static void thread_get_thread_name(thread_t t,char *name){assert(t==&th);memcpy(name,"CA render server\n",18);}
static uint64_t mach_absolute_time(void){return ticks;}
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *out){*out=ns;}
int hz=100;
#define PZERO 0
static int tsleep(void *event,int priority,const char *name,int timeout){assert(event && !priority && !strcmp(name,"sweetprof") && timeout==60*hz);ticks+=60ULL*NSEC_PER_SEC;sleeps++;blocks++;return 0;}
static int assert_wait_deadline_with_leeway(void *event,int intr,unsigned urgency,uint64_t deadline,uint64_t leeway){
 assert(event && intr==1 && urgency==32 && deadline>ticks && !locks);wake=deadline;last_leeway=leeway;sleeps++;return 0;
}
static int thread_block(void *cont){assert(!cont);ticks=wake;blocks++;return 0;}
static void *kalloc_data(size_t size,int flags){assert(flags==3 && !locks);allocs++;return allocfail?NULL:calloc(1,size);}
static void kfree_data(void *ptr,size_t size){assert(size && ptr && !locks);frees++;free(ptr);}
static vm_shared_region_t vm_shared_region_get(task_t t){assert(t==&task);return &region;}
static void vm_shared_region_deallocate(vm_shared_region_t sr){assert(sr==&region);}
void thread_terminate_self(void){longjmp(terminated,1);}
static int PE_parse_boot_argn(const char *name,void *value,unsigned size){assert(!strcmp(name,"sweetprof") && size==4);*(unsigned *)value=boot_value;return boot_present;}
static int kernel_thread_start(void (*fn)(void *,int),void *arg,thread_t *thread){assert(fn && !arg);starts++;*thread=&th;threadrefs++;return 0;}
static void thread_deallocate(thread_t t){assert(t==&th && threadrefs);threadrefs--;}
'''
TEST = r'''
static void reset(void){assert(!refs && !locks && !high && !schedlock);ticks=0;reads=allocs=frees=starts=threadrefs=output=sleeps=blocks=0;missing=allocfail=readfail=malformed=cost=non64=0;th.ss=&regs;th.state=TH_WAIT;th.oncore=0;regs.fp=0x1000;}
static void worker(void){if(!setjmp(terminated))sp_worker(NULL,0);assert(!refs && !threadrefs && !locks);}
int main(void){
 reset();for(unsigned present=0;present<2;present++)for(unsigned value=0;value<4;value++){
  boot_present=present;boot_value=value;unsigned before=starts;sweet_prof_start();
  assert(starts-before==(present && value>=1));assert(!allocs && !sleeps && !output && !threadrefs);
  assert(sp_delay==(present && value>1?value:60));sp_delay=60;
 }
 boot_present=1;boot_value=3601;{unsigned before=starts;sweet_prof_start();assert(starts==before && sp_delay==60);}
 worker();assert(ticks==120ULL*NSEC_PER_SEC && sleeps==30001 && blocks==sleeps && last_leeway==100000);
 assert(reads==240000 && allocs==1 && frees==1 && output==4);
 assert(strstr(logs[0],"PROF backboardd[1] slide 0x200000 mh 0x100000000"));
 assert(strstr(logs[1],"t42 name=CA_render_server_ total=30000 dropped=0"));
 assert(strstr(logs[2],"n=30000/30000 state=wait x16=-31 wait=0x123 wr=7 mixed=0 pc 0x180001000"));
 assert(strstr(logs[2],"bt 0x180003000 0x180003010 0x180003020 0x180003030"));
 assert(strstr(logs[3],"sweeps=30000 late=0 skipped=0 untracked=0 truncated=0"));
 reset();missing=1;worker();assert(ticks==60ULL*NSEC_PER_SEC && !allocs && output==1);
 reset();allocfail=1;worker();assert(allocs==1 && !frees && output==1 && !reads);
 reset();cost=400000;worker(); // 8 frame reads keep the 3.2 ms sweep costassert(ticks>=120ULL*NSEC_PER_SEC && ticks<120ULL*NSEC_PER_SEC+4*NSEC_PER_MSEC && strstr(logs[output-1],"late=11538"));
 struct sp_table *p=calloc(1,sizeof(*p));assert(p);
 reset();sp_sweep(&task,p);assert(p->threads[0].total==1 && reads==8);
 for(unsigned state=0;state<4;state++){
  th.oncore=state==0;th.state=state==1?TH_WAIT:state==2?TH_RUN:0;sp_sweep(&task,p);
  struct sp_sample s={.tid=42,.pc=regs.pc,.lr=regs.lr,.bt={0x180003000,0x180003010},.state=state};
  unsigned found=0;for(unsigned i=0;i<SP_BUCKETS;i++)if(p->keys[i].count && sp_equal(&p->keys[i].s,&s))found++;assert(found==1);
 }
 free(p);
 for(unsigned mode=0;mode<8;mode++){
  reset();p=calloc(1,sizeof(*p));assert(p);
  if(mode<3)malformed=mode+1;
  if(mode==3)regs.fp=0x1001;
  if(mode==4)readfail=2;
  if(mode==5)th.ss=NULL;
  if(mode==6)non64=1;
  if(mode==7)th.state=TH_TERMINATE;
  sp_sweep(&task,p);
  assert(reads==(mode<3?1:mode==4?2:0));assert(p->skipped==(mode>=5));free(p);
 }
 reset();p=calloc(1,sizeof(*p));assert(p);
 struct sp_sample s={.tid=100,.state=1,.pc=1,.lr=2,.bt={3,4,5,6},.x16=-31};
 unsigned t=sp_thread_index(p,s.tid);sp_add(p,t,&s);s.x16=-49;s.event=20;s.bt[3]++;sp_add(p,t,&s);
 for(unsigned n=1;n<=20;n++){s.pc=n;for(unsigned j=0;j<n;j++)sp_add(p,t,&s);}
 sp_print(p);assert(output==14 && strstr(logs[1],"n=20/212") && strstr(logs[12],"n=9/212"));
 unsigned mixed=0;for(unsigned n=0;n<SP_BUCKETS;n++)mixed+=p->keys[n].mixed;assert(mixed==1);
 /* A completely occupied probe window drops samples without overwriting counts. */
 memset(p,0,sizeof(*p));t=sp_thread_index(p,100);
 for(unsigned i=0;i<SP_BUCKETS;i++){p->keys[i].count=1;p->keys[i].s.tid=999;}
 sp_add(p,t,&s);assert(p->threads[t].total==1 && p->threads[t].dropped==1);
 /* Stable IDs survive enumeration order changes; explicit thread/output bounds. */
 memset(p,0,sizeof(*p));output=0;
 for(unsigned i=0;i<SP_THREADS;i++){
  s.tid=i+1;t=sp_thread_index(p,s.tid);assert(t==i);
  for(unsigned j=0;j<SP_TOP;j++){s.pc=j;sp_add(p,t,&s);}
 }
 for(unsigned i=SP_THREADS;i>0;i--)assert(sp_thread_index(p,i)==i-1);
 assert(sp_thread_index(p,9999)==SP_THREADS && p->untracked==1);
 sp_print(p);assert(output==399 && strstr(logs[398],"truncated=256"));
 unsigned summaries=0;for(unsigned i=0;i<output;i++)if(strstr(logs[i]," total="))summaries++;
 assert(summaries==SP_THREADS);free(p);
#undef printf
 puts("PASS: actual profiler C: boot gate, 60+60s/2ms/leeway, no spin/catch-up/allocation/logging in sweeps, task refs, failure cleanup, states/x16, fault-safe bounded frames, aggregation/mixed/top12/overflow/400-line cap");
}
'''


def syntax_file(buildtree, component, filename):
    config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_FUXI'
    if not config.exists():
        config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
    cwd = config / component / 'DEVELOPMENT'
    old_root = re.search(r'^SOURCE_DIR=(.+)/xnu$', (cwd / 'Makefile').read_text(), re.M)[1]
    raw = (cwd / '.CFLAGS').read_text().replace('\0', '')
    raw = re.sub(r'(-DCONFIG_NMBCLUSTERS=\(\(1024 \* 512\) / MCLBYTES\))', lambda m: shlex.quote(m[1]), raw)
    flags, skip = [], False
    for flag in shlex.split(raw)[1:]:
        if skip:
            skip = False
            continue
        if flag == '-arch':
            skip = True
            continue
        flag = flag.replace(old_root, str(buildtree))
        flag = re.sub(r'-I.*?\.sdk/+System/Library/Frameworks/Kernel\.framework/Versions/A/PrivateHeaders/platform',
                      '-I'+str(buildtree / 'sdk-kernel-platform/platform'), flag)
        flags.append(flag)
    subprocess.run(['clang', '--target=arm64-apple-macos14', '-fsyntax-only', '-mgeneral-regs-only',
                    '-Werror', '-D__builtin_xnu_type_signature(...)=""', '-D__builtin_xnu_type_summary(...)=0',
                    '-D__builtin_xnu_types_compatible(...)=1', *flags, '-DFUXI',
                    str(ROOT / 'xnu/fuxi-prof' / filename)], cwd=cwd, check=True)
    print('PASS: complete '+filename+' syntax against saved XNU headers/flags')


def symbolication():
    spec = importlib.util.spec_from_file_location('sweet_crash', ROOT / 'tools/sweet-crash.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # The existing parser already accepts pc/lr and every bt address.
    text = (ROOT / 'tools/sweet-crash.py').read_text()
    pattern = re.search(r"re.findall\(r'([^']+)'", text)[1]
    for prefix in ('SMP backboardd t0', 'PROF backboardd t42 name=CA n=3/4 state=wait x16=-31 wait=0xfffffff012345678 wr=7 mixed=0'):
        line = prefix + ' pc 0x180001000 lr 0x180002000 bt 0x180003000 0x180004000 0x180005000 0x180006000'
        assert re.findall(pattern, line) == [hex(0x180001000+i*0x1000) for i in range(6)]
    capture = io.StringIO()
    report = ('PROF backboardd[1] slide 0x200000 mh 0x100000000\n'
              'PROF backboardd t42 name=CA n=3/4 state=wait x16=-31 wait=0xfffffff012345678 wr=7 mixed=0 '
              'pc 0x180201000 lr 0x100001000 bt 0x180203000 0x180204000 0x180205000 0x180206000\n')
    with mock.patch.object(module.sys, 'argv', ['sweet-crash.py', 'CACHE', 'ROOT']), \
         mock.patch.object(module.sys, 'stdin', io.StringIO(report)), \
         mock.patch.object(module, 'a2s', return_value='mock_symbol') as resolve, \
         contextlib.redirect_stdout(capture):
        module.main()
    assert 'cache 0x180001000 mock_symbol' in capture.getvalue()
    assert 'main binary 0x100001000' in capture.getvalue()
    assert resolve.call_count == 5
    print('PASS: existing sweet-crash.py parses PROF header/address layout and stays SMP-compatible')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu-buildtree', type=Path)
    args = ap.parse_args()
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-prof.py'), '--check'], check=True)
    source = ''.join((ROOT / 'xnu/fuxi-prof' / name).read_text() for name in ('sweet_prof_sample.c', 'sweet_prof.c'))
    source = re.sub(r'^#include <[^>]+>\n', '', source, flags=re.M)
    with tempfile.TemporaryDirectory(prefix='fuxi-prof-test-') as tmp:
        tmp = Path(tmp)
        (tmp / 'prof.c').write_text(MOCK + source + TEST)
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I'+str(ROOT / 'xnu/fuxi-prof'),
                        str(tmp / 'prof.c'), '-o', str(tmp / 'prof')], check=True)
        subprocess.run([str(tmp / 'prof')], check=True)
    symbolication()
    if args.xnu_buildtree:
        syntax_file(args.xnu_buildtree.resolve(), 'bsd', 'sweet_prof.c')
        syntax_file(args.xnu_buildtree.resolve(), 'osfmk', 'sweet_prof_sample.c')


if __name__ == '__main__':
    main()
