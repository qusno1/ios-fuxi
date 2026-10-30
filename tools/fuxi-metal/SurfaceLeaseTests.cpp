#include "SurfaceLease.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <future>
#include <atomic>
#include <memory>
static std::map<uintptr_t,unsigned> held;
static unsigned locks,unlocks;
static uintptr_t fail;
static unsigned readLocks,writeLocks,readUnlocks;
static int lock(uintptr_t p,bool write){++locks;if(p==fail)return 1;++held[p];++(write?writeLocks:readLocks);return 0;}
static void unlock(uintptr_t p,bool write){assert(held[p]);--held[p];++unlocks;if(!write)++readUnlocks;}
int main() {
 assert(!fuxi::gpuScanoutEnabled(nullptr));
 assert(fuxi::gpuScanoutEnabled("other=1\tfuxigpuscanout=1\nquiet"));
 for(const char *args:{"", "fuxigpuscanout=0", "fuxigpuscanout=2", "xfuxigpuscanout=1", "fuxigpuscanout=1x",
     "fuxigpuscanout=1 fuxigpuscanout=1", "fuxigpuscanout=1 fuxigpuscanout=0"})assert(!fuxi::gpuScanoutEnabled(args));
 bool poisoned=false;
 {fuxi::SurfaceLease lease(false,lock,unlock,&poisoned);lease.add(1);assert(!locks);}
 {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(0);lease.add(1);lease.add(1);lease.add(2);
  assert(locks==2 && !unlocks && held[1]==1 && held[2]==1);}
 assert(unlocks==2 && !held[1] && !held[2]);
 fail=2;
 try {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(1);lease.add(2);assert(false);}
 catch(const std::runtime_error &){}
 assert(unlocks==3 && !held[1] && !held[2]);fail=0;
 {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);lease.add(1);poisoned=true;}
 assert(held[1]==1 && unlocks==3); // unknown DMA cannot release producer ownership
 held[1]=0;poisoned=false;
 try {fuxi::SurfaceLease lease(true,lock,unlock,&poisoned);for(uintptr_t i=1;i<=257;i++)lease.add(i);assert(false);}
 catch(const std::runtime_error &){}
 for(auto pair:held)assert(!pair.second);
 // Commit reservations count concurrent producers without waiting for the prior writer.
 {fuxi::SurfaceLease first(true,lock,unlock,&poisoned),next(true,lock,unlock,&poisoned);
  first.add(900);next.add(900);assert(held[900]==2);}
 assert(!held[900]);
 {fuxi::SurfaceLease failed(true,lock,unlock,&poisoned);failed.add(901);failed.quarantine();}
 assert(held[901]==1);held[901]=0;
 std::atomic<bool> poison{false};
 auto first=std::make_unique<fuxi::SurfaceLease>(true,lock,unlock,&poison,true);first->add(1000);
 std::promise<void> started;auto second=std::async(std::launch::async,[&]{started.set_value();fuxi::SurfaceLease next(true,lock,unlock,&poison,true);next.add(1000);});
 started.get_future().wait();assert(second.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 first.reset();second.get();assert(!held[1000]);
 first=std::make_unique<fuxi::SurfaceLease>(true,lock,unlock,&poison,true);first->add(1001);
 std::promise<void> waiting;auto refused=std::async(std::launch::async,[&]{waiting.set_value();bool failed=false;try{fuxi::SurfaceLease next(true,lock,unlock,&poison,true);next.add(1001);}catch(const std::runtime_error&){failed=true;}assert(failed);});
 waiting.get_future().wait();assert(refused.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 poison=true;first.reset();refused.get();assert(held[1001]);
 // Sampled-only surfaces share with each other, wait for a writer, and block the next writer.

 {poison=false;writeLocks=readLocks=readUnlocks=0;auto r1=std::make_unique<fuxi::SurfaceLease>(true,lock,unlock,&poison,true);r1->add(2000,false);
  auto r2=std::make_unique<fuxi::SurfaceLease>(true,lock,unlock,&poison,true);r2->add(2000,false);assert(held[2000]==2 && readLocks==2 && !writeLocks && r1->stats.count[0]==1 && !r1->stats.count[1]);
  std::promise<void> up;auto writer=std::async(std::launch::async,[&]{up.set_value();fuxi::SurfaceLease w(true,lock,unlock,&poison,true);w.add(2000,true);assert(w.stats.count[1]==1);});
  up.get_future().wait();assert(writer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
  r1.reset();assert(writer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);r2.reset();writer.get();assert(!held[2000] && readUnlocks==2);
  auto w=std::make_unique<fuxi::SurfaceLease>(true,lock,unlock,&poison,true);w->add(2001,true);
  std::promise<void> up2;auto reader=std::async(std::launch::async,[&]{up2.set_value();fuxi::SurfaceLease r(true,lock,unlock,&poison,true);r.add(2001,false);});
  up2.get_future().wait();assert(reader.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);w.reset();reader.get();assert(!held[2001]);
  // A reader upgraded to a writer ends with one write hold.
  {fuxi::SurfaceLease u(true,lock,unlock,&poison,true);u.add(2002,false);u.add(2002,true);u.add(2002,false);assert(held[2002]==1 && u.stats.count[1]==1);}
  assert(!held[2002]);
 }
 puts("PASS: sampled-only leases share, order against writers and upgrade");
 puts("PASS: exclusive surface reuse waits for completion; poisoned waiter wakes without scanout handoff");
 puts("PASS: scanout opt-in parsing, per-submit deduplication, completion/rollback release, unknown-DMA retention, bounded leases");
}
