/* Native selectors/ownership against a paused CPU fence model; requires Mac. */
#import <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <cassert>
static std::atomic<unsigned> pendingLocks{0},heldLocks{0};
static std::atomic<bool> failPending{false};
static kern_return_t overlapLock(IOSurfaceRef,IOSurfaceLockOptions options,uint32_t *) {
 if(options==kIOSurfaceLockAvoidSync){if(failPending)return 1;++pendingLocks;}++heldLocks;return 0;
}
static kern_return_t overlapUnlock(IOSurfaceRef,IOSurfaceLockOptions,uint32_t *) {assert(heldLocks);--heldLocks;return 0;}
#define IOSurfaceLock overlapLock
#define IOSurfaceUnlock overlapUnlock
#define FUXI_MTL_HOST_TEST
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
#include <future>
@interface FuxiOverlapBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiOverlapBase
@end
@interface FuxiOverlapDrawable : NSObject { @public unsigned presents; }
@end
@implementation FuxiOverlapDrawable
- (void)present {++presents;}
@end
int main(){@autoreleasepool{
 assert(fuxi::systemTranslationPolicy().overlap);
 Class cls=makeDeviceClass([FuxiOverlapBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiQueue *single=(FuxiQueue *)[device newCommandQueueWithMaxCommandBufferCount:1];
 assert(!dispatch_semaphore_wait(single->_inflight,DISPATCH_TIME_NOW));assert(dispatch_semaphore_wait(single->_inflight,DISPATCH_TIME_NOW));dispatch_semaphore_signal(single->_inflight);[single release];
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];
 static int deliveryKey;dispatch_queue_t delivery=dispatch_queue_create("fuxi.test.completion",DISPATCH_QUEUE_SERIAL);
 dispatch_queue_set_specific(delivery,&deliveryKey,&deliveryKey,nullptr);[queue setCompletionQueue:delivery];dispatch_suspend(queue->_retireQueue);
 __block unsigned callbacks=0,scheduled=0;
 FuxiCommandBuffer *a=[(FuxiCommandBuffer *)[queue commandBuffer] retain],*b=[(FuxiCommandBuffer *)[queue commandBuffer] retain];
 // Enable attribution directly without requiring a backboardd process gate.
 for(auto *command:{a,b}){command->_frameTrace=YES;command->_traceCreated=fuxi::FrameClock::now();command->_traceThread=std::this_thread::get_id();}
 FuxiOverlapDrawable *drawable=[FuxiOverlapDrawable new];
 FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithLength:4096 options:0];
 id<MTLBlitCommandEncoder> encoder=[a blitCommandEncoder];[encoder fillBuffer:(id)buffer range:NSMakeRange(0,4096) value:42];[encoder endEncoding];
 [a addScheduledHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusScheduled);[command waitUntilScheduled];++scheduled;}];
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==0 && dispatch_get_specific(&deliveryKey)==&deliveryKey);[command waitUntilCompleted];++callbacks;}];
 [a presentDrawable:(id)drawable];
 [b addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==1);++callbacks;}];
 // Explicit enqueue order wins over reversed commit order.
 [a enqueue];[b commit];[a commit];[buffer release];
 [a waitUntilScheduled];[b waitUntilScheduled];assert(scheduled==1 && callbacks==0 && !drawable->presents && fuxi::Backend::live==1);
 // Late registration while a fence is pending must join that completion.
 [b addCompletedHandler:^(id<MTLCommandBuffer> command){(void)command;assert(callbacks==2);++callbacks;}];
 FuxiCommandBuffer *third=[(FuxiCommandBuffer *)[queue commandBuffer] retain];third->_frameTrace=YES;third->_traceCreated=fuxi::FrameClock::now();third->_traceThread=std::this_thread::get_id();
 [third addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==3);++callbacks;}];
 std::promise<void> started;auto blocked=std::async(std::launch::async,[&]{@autoreleasepool{started.set_value();[third commit];}});
 started.get_future().wait();assert(blocked.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 dispatch_resume(queue->_retireQueue);blocked.get();[third waitUntilCompleted];[b waitUntilCompleted];[a waitUntilCompleted];
 assert(callbacks==4 && drawable->presents==1 && fuxi::Backend::live==0);
 assert(a->_flightTrace.commitCall && b->_flightTrace.submitQueue && third->_flightTrace.admissionWait);
 assert(a->_flightTrace.frontendCPU && a->_flightTrace.frontendWall && third->_flightTrace.frontendWall>=third->_flightTrace.admissionWait);
 assert(queue->_traceReturn && queue->_traceDelivery && queue->_traceHandlers && queue->_traceWaitCompleted);
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted);++callbacks;}];[a waitUntilCompleted];assert(callbacks==5);
 [a release];[b release];[third release];[drawable release];
 // Snapshot no-copy CPU bytes at commit, even while the submit worker is paused.
 uint8_t bytes[4096];memset(bytes,19,sizeof(bytes));buffer=(FuxiBuffer *)[device newBufferWithBytesNoCopy:bytes length:sizeof(bytes) options:0 deallocator:nil];
 assert(buffer && buffer->_shadow);FuxiBuffer *output=(FuxiBuffer *)[device newBufferWithLength:4096 options:0];
 a=[(FuxiCommandBuffer *)[queue commandBuffer] retain];encoder=[a blitCommandEncoder];[encoder copyFromBuffer:(id)buffer sourceOffset:0 toBuffer:(id)output destinationOffset:0 size:4096];[encoder endEncoding];
 dispatch_suspend(queue->_submitQueue);[a commit];memset(bytes,77,sizeof(bytes));dispatch_resume(queue->_submitQueue);[a waitUntilCompleted];
 for(unsigned i=0;i<4096;++i)assert(((uint8_t *)[output contents])[i]==19);[a release];[buffer release];[output release];
 // Opt-out wait still blocks until real scheduling.
 dispatch_suspend(queue->_submitQueue);a=[(FuxiCommandBuffer *)[queue commandBuffer] retain];
 a->_frameTrace=YES;a->_traceCreated=fuxi::FrameClock::now();a->_traceThread=std::this_thread::get_id();
 std::promise<void> waiting;auto synchronous=std::async(std::launch::async,[&]{@autoreleasepool{waiting.set_value();assert([a commitAndWaitUntilSubmitted]);}});
 waiting.get_future().wait();assert(synchronous.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 dispatch_resume(queue->_submitQueue);synchronous.get();[a waitUntilCompleted];assert(queue->_traceWaitScheduled && a->_scheduled);[a release];
 // Early CA waits reserve counted leases even with the submit queue and one flight paused.
 fuxi::Backend::modelHostImport=true;gpuScanout=true;
 IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@8,(id)kIOSurfaceHeight:@6,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfaceAllocSize:@16384,(id)kIOSurfacePixelFormat:@(0x42475241)});assert(surface);
 auto *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:6 mipmapped:NO];
 descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 FuxiTexture *texture=(FuxiTexture *)[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];assert(texture && !heldLocks);unsigned pendingBase=pendingLocks; // texture creation probes one AvoidSync lock
 FuxiQueue *earlyQueue=(FuxiQueue *)[device newCommandQueueWithMaxCommandBufferCount:1];
 auto earlyCommand=[&]{FuxiCommandBuffer *c=[(FuxiCommandBuffer *)[earlyQueue commandBuffer] retain];c->_earlySubmit=YES;c->_frameTrace=YES;c->_traceCreated=fuxi::FrameClock::now();c->_traceThread=std::this_thread::get_id();[c->_resources addObject:texture];return c;};
 dispatch_suspend(earlyQueue->_submitQueue);dispatch_suspend(earlyQueue->_retireQueue);
 a=earlyCommand();b=earlyCommand();third=earlyCommand();__block unsigned order=0;
 [a addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusScheduled && order==0);++order;}];
 [b addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusScheduled && order==1);++order;}];
 [third addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusScheduled && order==2);++order;}];
 [a enqueue];
 auto returned=std::async(std::launch::async,[&]{@autoreleasepool{assert([b commitAndWaitUntilSubmitted]);assert([a commitAndWaitUntilSubmitted]);assert([third commitAndWaitUntilSubmitted]);}});
 assert(returned.wait_for(std::chrono::seconds(1))==std::future_status::ready);returned.get();
 assert(pendingLocks-pendingBase==3 && heldLocks==3 && !order && !a->_scheduled && !b->_scheduled && !third->_scheduled && !earlyQueue->_depth);
 assert(earlyQueue->_traceWaitScheduled && a->_flightTrace.schedWait);
 __block unsigned lateScheduled=0;[b addScheduledHandler:^(id<MTLCommandBuffer> c){assert(c.status==MTLCommandBufferStatusScheduled && order==2);++lateScheduled;}];assert(!lateScheduled);
 dispatch_resume(earlyQueue->_submitQueue);
 // Worker recording can also remain blocked after the caller has returned.
 std::promise<void> recording,releaseRecording;auto proceed=releaseRecording.get_future();
 // The serial submit queue is drained before installing the next model hook.
 dispatch_resume(earlyQueue->_retireQueue);[third waitUntilCompleted];[b waitUntilCompleted];[a waitUntilCompleted];
 assert(order==3 && lateScheduled==1 && !heldLocks);[a release];[b release];[third release];
 fuxi::Backend::beforeSubmit=[&]{recording.set_value();proceed.wait();};a=earlyCommand();
 assert([a commitAndWaitUntilSubmitted]);recording.get_future().wait();assert(!a->_scheduled && heldLocks==2);
 releaseRecording.set_value();[a waitUntilCompleted];fuxi::Backend::beforeSubmit={};assert(!heldLocks);[a release];
 // Completion bookkeeping cannot hold the next source lease or flight slot.
 dispatch_suspend(earlyQueue->_finishQueue);a=earlyCommand();b=earlyCommand();third=earlyCommand();
 __block unsigned finishedOrder=0;
 [a addCompletedHandler:^(id<MTLCommandBuffer> c){(void)c;assert(finishedOrder++==0);}];
 [b addCompletedHandler:^(id<MTLCommandBuffer> c){(void)c;assert(finishedOrder++==1);}];
 [third addCompletedHandler:^(id<MTLCommandBuffer> c){(void)c;assert(finishedOrder++==2);}];
 for(auto *command:{a,b,third})assert([command commitAndWaitUntilSubmitted]);
 dispatch_sync(earlyQueue->_submitQueue,^{});dispatch_sync(earlyQueue->_retireQueue,^{});
 assert(!heldLocks && !earlyQueue->_depth && !finishedOrder && !a->_finished && !b->_finished && !third->_finished);
 assert(a->_flightTrace.retireQueue && b->_flightTrace.retireQueue && third->_flightTrace.retireQueue);
 dispatch_resume(earlyQueue->_finishQueue);[third waitUntilCompleted];assert(finishedOrder==3);
 [a release];[b release];[third release];
 // A reservation failure rejects commit; a later submission failure keeps its reservation closed.
 failPending=true;a=earlyCommand();assert(![a commitAndWaitUntilSubmitted]);[a waitUntilCompleted];assert(a.error && !heldLocks);[a release];failPending=false;
 fuxi::Backend::beforeSubmit=[]{throw std::runtime_error("modeled recording failure");};a=earlyCommand();
 [a addScheduledHandler:^(id<MTLCommandBuffer> c){(void)c;assert(false);}];
 [a commitAndWaitUntilSubmitted];[a waitUntilCompleted];assert(a.error && !a->_scheduled && heldLocks==1);fuxi::Backend::beforeSubmit={};[a release];
 overlapUnlock(surface,0,nullptr); // test-only recovery of a fail-closed reservation
 [earlyQueue release];[texture release];CFRelease(surface);gpuScanout=false;
 // Device loss at retirement reports Error once, never presents, still unblocks waiters.
 dispatch_suspend(queue->_retireQueue);a=[(FuxiCommandBuffer *)[queue commandBuffer] retain];drawable=[FuxiOverlapDrawable new];
 [a presentDrawable:(id)drawable];__block unsigned errors=0;
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusError && command.error);++errors;}];
 [a commit];[a waitUntilScheduled];owner(device)->backend->poisoned=true;dispatch_resume(queue->_retireQueue);[a waitUntilCompleted];assert(errors==1 && !drawable->presents);
 [a release];[drawable release];[queue release];dispatch_release(delivery);[device release];
 puts("PASS: overlap commit return, explicit enqueue/reversed commits, scheduled/completed/late/self-wait handlers, retained resources, presentation, no-copy snapshot, in-flight poison");
}}
