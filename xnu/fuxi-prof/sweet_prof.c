/* Opt-in single-process profiler; osfmk owns saved state and aggregation. */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern task_t proc_task(proc_t);
extern int hz;
extern void thread_terminate_self(void);
extern void sweet_prof_task(task_t task,int pid);
void sweet_prof_start(void);
struct sp_target { task_t task; int pid; };
static const char *sp_target_name="backboardd";
static int sp_find(proc_t p,void *arg) {
 struct sp_target *target=arg;char name[32];proc_name(proc_pid(p),name,sizeof(name));
 if(!target->task && !strcmp(name,sp_target_name)) {
  task_t task=proc_task(p);
  if(task!=TASK_NULL){task_reference(task);target->task=task;target->pid=proc_pid(p);}
 }
 return PROC_RETURNED;
}
static unsigned sp_delay=60;
static void sp_worker(__unused void *arg,__unused wait_result_t wr) {
 static int event;
 (void)tsleep(&event,PZERO,"sweetprof",(int)sp_delay*hz);
 if(strcmp(sp_target_name,"backboardd"))printf("PROF target %s\n",sp_target_name);
 struct sp_target target={0};proc_iterate(PROC_ALLPROCLIST,sp_find,&target,NULL,NULL);
 if(target.task){sweet_prof_task(target.task,target.pid);task_deallocate(target.task);}
 else printf("PROF %s unavailable\n",sp_target_name);
 thread_terminate_self();
}
void sweet_prof_start(void) {
 unsigned enabled=0;thread_t thread;
 /* sweetprof=1: start after 60 s; sweetprof=N (2..3600): start after N s. 10000+N samples SpringBoard
    instead; its lines keep the "PROF backboardd" prefix so the existing parsers still match. */
 if(!PE_parse_boot_argn("sweetprof",&enabled,sizeof(enabled)) || !enabled)return;
 if(enabled>=10000 && enabled<=13600){sp_target_name="SpringBoard";enabled-=10000;if(!enabled)enabled=1;}
 if(enabled>3600)return;
 if(enabled>1)sp_delay=enabled;
 if(kernel_thread_start(sp_worker,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
#else
void sweet_prof_start(void);
void sweet_prof_start(void) {}
#endif
