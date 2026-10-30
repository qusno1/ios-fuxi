/* Real Vulkan deferred retirement, independent slots, pins and fault quarantine. */
#include "Backend.hpp"
#include <cassert>
#include <future>
int main(int argc,char **argv){
 using namespace fuxi;
 if(argc>1 && !strcmp(argv[1],"host")){
 Backend b;auto *a=b.allocate(4096),*c=b.allocate(4096);std::function<void()> x,y;
 b.submit({{nullptr,a,0,0,4096,63}},{},&x);std::promise<void> started;
 auto next=std::async(std::launch::async,[&]{started.set_value();b.submit({{a,c,0,0,4096,0}},{},&y);});
 started.get_future().wait();assert(next.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);x();next.get();y();
 assert(b.flushCalls>=3 && b.invalidateCalls>=3);for(unsigned i=0;i<4096;++i)assert(((uint8_t *)c->contents)[i]==63);
 b.destroy(a);b.destroy(c);puts("PASS: shared noncoherent allocation drains before flush/record/invalidate");return 0;}
 if(argc>1){Backend lost;auto *a=lost.allocate(4096),*b=lost.allocate(4096);std::function<void()> x,y;
 lost.submit({{nullptr,a,0,0,4096,1}},{},&x);lost.submit({{nullptr,b,0,0,4096,2}},{},&y);auto held=liveMemoryObjects.load();
 for(auto *ticket:{&x,&y}){bool failed=false;try{(*ticket)();}catch(const std::runtime_error&){failed=true;}assert(failed && lost.poisoned);}
 lost.destroy(a);lost.destroy(b);lost.close();assert(liveMemoryObjects==held);puts("PASS: injected in-flight fence loss/timeout, both tickets fail, DMA quarantine");return 0;}
 Backend b;auto baseline=liveMemoryObjects.load();
 auto *first=b.allocate(4096),*second=b.allocate(4096);
 std::function<void()> retireFirst,retireSecond;unsigned scheduled=0;
 FrameSample a,c;
 {FrameTraceScope scope(&a);b.submit({{nullptr,first,0,0,4096,31}},[&]{++scheduled;},&retireFirst);}
 {FrameTraceScope scope(&c);b.submit({{first,second,0,0,4096,0}},[&]{++scheduled;},&retireSecond);}
 assert(scheduled==2 && retireFirst && retireSecond && !a.gpuWait && !c.gpuWait);
 assert(b.reuseCounts.commandPools==2 && b.reuseCounts.fences==2);
 // Third submission cannot reset either live slot. This is a synchronization
 // assertion, not a GPU speed threshold (neither ticket has been retired).
 std::promise<void> started;auto third=std::async(std::launch::async,[&]{started.set_value();b.submit({{nullptr,first,0,0,4,7}});});
 started.get_future().wait();assert(third.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 {FrameTraceScope scope(&a);retireFirst();}third.get();
 {FrameTraceScope scope(&c);retireSecond();}
 assert(a.gpuWait && c.gpuWait && b.reuseCounts.commandPools==2);
 for(unsigned i=0;i<4096;++i)assert(((uint8_t *)second->contents)[i]==31);
 // External destruction cannot reclaim a buffer while a ticket owns it.
 auto *pinned=b.allocate(4096);std::function<void()> retire;
 b.submit({{nullptr,pinned,0,0,4096,42}},{},&retire);auto held=liveMemoryObjects.load();b.destroy(pinned);assert(liveMemoryObjects==held);retire();assert(liveMemoryObjects==held-1);
 b.destroy(first);b.destroy(second);b.close();assert(liveMemoryObjects==baseline);
 // A poison arriving after two successful QueueSubmits must fail both tickets,
 // free admission slots, and leave DMA allocations quarantined through close.
 Backend lost;first=lost.allocate(4096);second=lost.allocate(4096);
 lost.submit({{nullptr,first,0,0,4096,1}},{},&retireFirst);lost.submit({{nullptr,second,0,0,4096,2}},{},&retireSecond);
 held=liveMemoryObjects.load();try{lost.loseWhileInFlightForTest();assert(false);}catch(const std::runtime_error&){}assert(lost.poisoned);
 for(auto *ticket:{&retireFirst,&retireSecond}){bool failed=false;try{(*ticket)();}catch(const std::runtime_error&){failed=true;}assert(failed);}
 lost.destroy(first);lost.destroy(second);lost.close();assert(liveMemoryObjects==held);
 puts("PASS: deferred wait, two independent lanes, bounded third submit, GPU ordering/pixels, resource pins, poison-in-flight quarantine");
}
