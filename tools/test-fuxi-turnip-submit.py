#!/usr/bin/env python3
"""Compile actual patched Darwin KMD wait/submit functions against an ABI model.

Supply the external pinned UniXNU patch; no Mesa source is vendored.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "NativeAdrenoABI.h"
using VkResult=int;using kern_return_t=int;
constexpr int VK_SUCCESS=0,VK_TIMEOUT=2,VK_ERROR_DEVICE_LOST=-4,KERN_SUCCESS=0,kIOReturnTimeout=1,kIOReturnBusy=2;
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define p_atomic_set(p,v) (*(p)=(v))
#define p_atomic_read(p) (*(p))
#define mesa_logi(...) ((void)0)
struct vk_device {int unused;};struct tu_device {struct vk_device vk;};
struct tu_queue {struct tu_device *device;int fence;};
struct vk_sync {int unused;};
enum darwin_sync_state {DARWIN_SYNC_UNSIGNALED,DARWIN_SYNC_SIGNALED,DARWIN_SYNC_FENCE};
struct vk_darwin_sync {struct vk_sync vk;enum darwin_sync_state state;uint32_t fence;};
struct vk_sync_wait {struct vk_sync *sync;};struct vk_sync_signal {struct vk_sync *sync;};
enum vk_sync_wait_flags {VK_SYNC_WAIT_PENDING=1,VK_SYNC_WAIT_ANY=2};
struct tu_u_trace_submission_data {unsigned gpu_ts_offset;};
struct util_dynarray {unsigned count;NativeAdrenoAbiSubmitBuffer data[130];};
struct tu_darwin_queue_submit {struct util_dynarray commands;};
#define util_dynarray_num_elements(p,t) ((p)->count)
#define util_dynarray_element(p,t,i) ((p)->data+(i))
static struct {pthread_mutex_t lock;unsigned inflight_count;uint32_t inflight[8];} dw={PTHREAD_MUTEX_INITIALIZER,0,{}};
static uint64_t clock_ns=1000000000,slice_seen,reply_fence=7;
static unsigned submits,polls,collections;static int response=kIOReturnTimeout,collect_result=VK_SUCCESS;
static VkResult darwin_collect_retired(tu_device *) {collections++;return collect_result;}
static uint64_t os_time_get_nano() {return clock_ns;}
static int vk_device_set_lost(vk_device *,const char *,...) {return VK_ERROR_DEVICE_LOST;}
static VkResult dw_status_to_vk(tu_device *,int rc,const char *) {return rc==kIOReturnTimeout?VK_TIMEOUT:VK_ERROR_DEVICE_LOST;}
static kern_return_t dw_call(unsigned method,const void *in,size_t bytes,void *out,size_t outbytes) {
 if(method==NativeAdrenoMethodWaitFence) {
  assert(bytes==sizeof(NativeAdrenoAbiWaitFence) && !out && !outbytes);
  const auto *r=static_cast<const NativeAdrenoAbiWaitFence *>(in);slice_seen=r->timeout_ns;clock_ns+=r->timeout_ns;++polls;return response;
 }
 assert(method==NativeAdrenoMethodSubmit && bytes==sizeof(NativeAdrenoAbiSubmit) && outbytes==sizeof(NativeAdrenoAbiSubmitReply));
 const auto *r=static_cast<const NativeAdrenoAbiSubmit *>(in);assert(r->buffer_count<=128 && !r->queue && !r->wait_fence);
 ++submits;static_cast<NativeAdrenoAbiSubmitReply *>(out)->fence=reply_fence;return KERN_SUCCESS;
}
static void darwin_note_fence(uint32_t f) {assert(f==reply_fence);}
static VkResult darwin_sync_wait(tu_device *,vk_darwin_sync *s,uint64_t) {
 return s->state==DARWIN_SYNC_SIGNALED?VK_SUCCESS:s->state==DARWIN_SYNC_FENCE?VK_ERROR_DEVICE_LOST:VK_TIMEOUT;
}
'''
TEST=r'''
int main() {
 tu_device dev={};tu_queue queue={&dev,0};
 assert(dw_wait_fence_locked_free(&dev,1,clock_ns+500000)==VK_TIMEOUT && slice_seen==500000 && polls==1);
 assert(dw_wait_fence_locked_free(&dev,1,0)==VK_TIMEOUT && slice_seen==0 && polls==2);
 response=KERN_SUCCESS;assert(dw_wait_fence_locked_free(&dev,1,0)==VK_SUCCESS);
 assert(collections==1);
 collect_result=VK_ERROR_DEVICE_LOST;
 assert(dw_wait_fence_locked_free(&dev,1,0)==VK_ERROR_DEVICE_LOST && collections==2);
 collect_result=VK_SUCCESS;
 response=99;assert(dw_wait_fence_locked_free(&dev,1,0)==VK_ERROR_DEVICE_LOST);
 response=KERN_SUCCESS;clock_ns=1000000000;
 assert(darwin_queue_wait_fence(&queue,1,UINT64_MAX-1)==VK_SUCCESS && slice_seen==1000000 && polls==6);
 // Saturating the relative deadline must not wrap to an immediate zero poll.
 clock_ns=1000000000;
 vk_darwin_sync syncs[2]={{{},DARWIN_SYNC_UNSIGNALED,0},{{},DARWIN_SYNC_FENCE,1}};
 vk_sync_wait waits[2]={{&syncs[0].vk},{&syncs[1].vk}};
 assert(vk_darwin_sync_wait_many(&dev.vk,2,waits,VK_SYNC_WAIT_ANY,0)==VK_ERROR_DEVICE_LOST);
 syncs[1].state=DARWIN_SYNC_SIGNALED;
 assert(vk_darwin_sync_wait_many(&dev.vk,2,waits,VK_SYNC_WAIT_ANY,0)==VK_SUCCESS);
 syncs[1].state=DARWIN_SYNC_UNSIGNALED;
 assert(vk_darwin_sync_wait_many(&dev.vk,2,waits,VK_SYNC_WAIT_ANY,0)==VK_TIMEOUT);
 assert(vk_darwin_sync_wait_many(&dev.vk,2,waits,VK_SYNC_WAIT_PENDING,0)==VK_SUCCESS);
 tu_darwin_queue_submit submit={};submit.commands.count=129;
 vk_darwin_sync signal={};vk_sync_signal signals[]={{&signal.vk}};
 assert(darwin_queue_submit(&queue,&submit,nullptr,0,signals,1,nullptr)==VK_ERROR_DEVICE_LOST && !submits);
 submit.commands.count=128;
 assert(darwin_queue_submit(&queue,&submit,nullptr,0,signals,1,nullptr)==VK_SUCCESS && submits==1 && queue.fence==7);
 assert(signal.state==DARWIN_SYNC_FENCE && signal.fence==7);
 submit.commands.count=0;
 assert(darwin_queue_submit(&queue,&submit,nullptr,0,signals,1,nullptr)==VK_SUCCESS && submits==1 && signal.fence==7);
 puts("PASS: actual KMD bounded/poll/overflow wait, wait-any loss propagation, atomic IB limit before first submit, fence signal/empty submit");
}
'''


def function(source,name):
    start=source.index(name+'(')
    start=source.rfind('static ',0,start)
    body=source.index('{',start);depth=1;end=body+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--upstream-patch',type=Path,required=True);args=ap.parse_args()
    raw=args.upstream_patch.read_text()
    block=raw.split('+++ b/src/freedreno/vulkan/tu_knl_darwin.cc\n')[1].split('\ndiff --git ')[0]
    original=''.join(line[1:] for line in block.splitlines(True) if line.startswith('+'))
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);source=d/'src/freedreno/vulkan/tu_knl_darwin.cc';source.parent.mkdir(parents=True);source.write_text(original)
        subprocess.run(['patch','-s','-p1','--fuzz=0','-i',str(ROOT/'tools/patches/mesa-fuxi-smoke.patch')],cwd=d,check=True)
        text=source.read_text()
        names=('dw_wait_fence_raw','dw_wait_fence_locked_free','darwin_queue_wait_fence','vk_darwin_sync_wait_many','darwin_submit_chunk','darwin_queue_submit')
        model=d/'kmd.cpp';model.write_text(PREFIX+'\n'.join(function(text,n) for n in names)+TEST)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'xnu/fuxi-adreno'),str(model),'-o',str(d/'kmd')],check=True)
        subprocess.run([str(d/'kmd')],check=True,timeout=5)


if __name__=='__main__':main()
