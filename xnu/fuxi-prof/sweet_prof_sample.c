/* Saved user stacks are approximate; no target suspension or logging per sweep. */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <arm/thread.h>
#include <arm64/proc_reg.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>
#include <string.h>
#include "sweet_prof_logic.h"
void sweet_prof_task(task_t task,int pid);
static void sp_sleep(uint64_t deadline,uint64_t leeway) {
 static int event;
 assert_wait_deadline_with_leeway(&event,THREAD_UNINT,TIMEOUT_URGENCY_SYS_NORMAL|TIMEOUT_URGENCY_LEEWAY,deadline,leeway);
 thread_block(THREAD_CONTINUE_NULL);
}
static void sp_sweep(task_t task,struct sp_table *p) {
 thread_t thread;vm_map_t map=get_task_map(task);
 task_lock(task);
 queue_iterate(&task->threads,thread,thread_t,task_threads) {
  struct sp_sample s={0};arm_saved_state_t *ss=find_user_regs(thread);
  if(!ss || !is_saved_state64(ss)){p->skipped++;continue;}
  spl_t level=splsched();thread_lock(thread);
  int state=thread->state;
  if(state&TH_TERMINATE){thread_unlock(thread);splx(level);p->skipped++;continue;}
  s.tid=thread->thread_id;s.event=thread->wait_event;s.wr=thread->wait_result;
  s.state=machine_thread_on_core(thread)?0:(state&TH_WAIT)?1:(state&TH_RUN)?2:3;
  thread_unlock(thread);splx(level);
  s.pc=get_saved_state_pc(ss);s.lr=get_saved_state_lr(ss);s.x16=(int32_t)get_saved_state_reg(ss,16);
  uint64_t fp=get_saved_state_fp(ss),base=fp;
  for(unsigned n=0;n<8 && fp && !(fp&7) && fp>=base && fp-base<1024*1024;n++) {
   uint64_t frame[2];
   if(vm_map_read_user(map,fp,frame,sizeof(frame))!=KERN_SUCCESS)break;
   s.bt[n]=frame[1];if(frame[0]<=fp)break;fp=frame[0];
  }
  unsigned t=sp_thread_index(p,s.tid);
  if(t==SP_THREADS)continue;
  if(!p->threads[t].name[0] && get_bsdthread_info(thread))thread_get_thread_name(thread,p->threads[t].name);
  sp_add(p,t,&s);
 }
 task_unlock(task);p->sweeps++;
}
/* Diagnostic: the calling thread's user frames plus the shared cache slide, one console line. */
void sweet_prof_user_bt(const char *tag);
void sweet_prof_user_bt(const char *tag) {
 thread_t thread=current_thread();task_t task=current_task();arm_saved_state_t *ss=find_user_regs(thread);
 if(!ss || !is_saved_state64(ss))return;
 uint64_t slide=0,bt[12]={0};vm_shared_region_t sr=vm_shared_region_get(task);
 if(sr){slide=sr->sr_slide;vm_shared_region_deallocate(sr);}
 vm_map_t map=get_task_map(task);uint64_t fp=get_saved_state_fp(ss),base=fp;unsigned n=0;
 for(;n<12 && fp && !(fp&7) && fp>=base && fp-base<1024*1024;n++) {
  uint64_t frame[2];
  if(vm_map_read_user(map,fp,frame,sizeof(frame))!=KERN_SUCCESS)break;
  bt[n]=frame[1];if(frame[0]<=fp){n++;break;}fp=frame[0];
 }
 printf("%s slide 0x%llx pc 0x%llx lr 0x%llx bt",tag,(unsigned long long)slide,(unsigned long long)get_saved_state_pc(ss),(unsigned long long)get_saved_state_lr(ss));
 for(unsigned i=0;i<n;i++)printf(" 0x%llx",(unsigned long long)bt[i]);
 printf("\n");
}
void sweet_prof_task(task_t task,int pid) {
 uint64_t deadline,period,leeway,duration;
 struct sp_table *p=kalloc_data(sizeof(*p),Z_WAITOK|Z_ZERO);
 if(!p){printf("PROF allocation failed\n");return;}
 uint64_t slide=0;vm_shared_region_t sr=vm_shared_region_get(task);
 if(sr){slide=sr->sr_slide;vm_shared_region_deallocate(sr);}
 task_lock(task);uint64_t mh=task->mach_header_vm_address;task_unlock(task);
 nanoseconds_to_absolutetime(2*NSEC_PER_MSEC,&period);nanoseconds_to_absolutetime(100*NSEC_PER_USEC,&leeway);
 nanoseconds_to_absolutetime(60ULL*NSEC_PER_SEC,&duration);
 uint64_t start=mach_absolute_time(),end=start+duration;deadline=start;
 while(mach_absolute_time()<end) {
  sp_sweep(task,p);
  uint64_t now=mach_absolute_time();if(now>=end)break;deadline+=period;
  if(deadline<=now){p->late++;deadline=now+period;}
  if(deadline>end)deadline=end;
  sp_sleep(deadline,leeway);
 }
 printf("PROF backboardd[%d] slide 0x%llx mh 0x%llx\n",pid,(unsigned long long)slide,(unsigned long long)mh);
 sp_print(p);kfree_data(p,sizeof(*p));
}
#endif
