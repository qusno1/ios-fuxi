#pragma once
// GPU-SCANOUT ownership for imported IOSurfaces. Original Fuxi implementation.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <functional>
namespace fuxi {
inline bool gpuScanoutEnabled(const char *args) {
 if(!args)return false;
 unsigned keys=0;bool enabled=false;
 for(const char *p=args;*p;) {
  while(*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')++p;
  const char *begin=p;while(*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n')++p;
  size_t n=size_t(p-begin);
  if(n>=15 && !strncmp(begin,"fuxigpuscanout=",15)){++keys;enabled=n==16 && begin[15]=='1';}
 }
 return keys==1 && enabled;
}
class SurfaceLease {
 using Lock=int (*)(uintptr_t,bool);
 using Unlock=void (*)(uintptr_t,bool);
 struct Held {uintptr_t surface;bool write;};
 bool enabled_,exclusive_,quarantined_=false;Lock lock_;Unlock unlock_;std::function<bool()> poisoned_;
 inline static std::mutex mutex_;inline static std::condition_variable wake_;inline static std::vector<uintptr_t> writers_,readers_;
 std::vector<Held> held_;
 static uint64_t since(std::chrono::steady_clock::time_point t){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t).count());}
 // Caller holds mutex_. A sampled-only reader shares with readers; a writer is alone.
 static void forget(uintptr_t surface,bool write){auto &v=write?writers_:readers_;auto it=std::find(v.begin(),v.end(),surface);if(it!=v.end())v.erase(it);}
public:
 // Per-lease cost split: exclusion wait vs kernel lock, writers vs sampled-only readers.
 struct Stats {uint64_t waitNs=0,lockNs[2]={0,0},count[2]={0,0};} stats;
 template<class Poison> SurfaceLease(bool enabled,Lock lock,Unlock unlock,const Poison *poisoned,bool exclusive=false)
  :enabled_(enabled),exclusive_(exclusive),lock_(lock),unlock_(unlock),poisoned_([poisoned]{return poisoned && bool(*poisoned);}) {}
 SurfaceLease(const SurfaceLease &)=delete;
 SurfaceLease &operator=(const SurfaceLease &)=delete;
 // write=false is for surfaces the GPU only samples: a read-only kernel lock never waits for scanout.
 void add(uintptr_t surface,bool write=true) {
  if(!enabled_ || !surface)return;
  auto found=std::find_if(held_.begin(),held_.end(),[&](const Held &h){return h.surface==surface;});
  if(found!=held_.end()){
   if(found->write || !write)return;
   // Upgrade a reader: release the shared hold, then take it again as a writer.
   Held old=*found;held_.erase(found);
   if(exclusive_){std::lock_guard<std::mutex> g(mutex_);forget(surface,false);wake_.notify_all();}
   if(!poisoned_())unlock_(old.surface,false);
  }
  if(held_.size()==256)throw std::runtime_error("scanout IOSurface lease limit");
  held_.push_back({surface,write});
  if(exclusive_){auto t=std::chrono::steady_clock::now();std::unique_lock<std::mutex> guard(mutex_);
   wake_.wait(guard,[&]{return poisoned_() || (std::find(writers_.begin(),writers_.end(),surface)==writers_.end() && (!write || std::find(readers_.begin(),readers_.end(),surface)==readers_.end()));});
   stats.waitNs+=since(t);
   if(poisoned_()){held_.pop_back();throw std::runtime_error("scanout device poisoned");}
   try{(write?writers_:readers_).push_back(surface);}catch(...){held_.pop_back();throw;}}
  // The kernel lock can sleep behind a scanout copy; never hold mutex_ across it, or retirement cannot release other leases.
  auto t=std::chrono::steady_clock::now();int failed=lock_(surface,write);stats.lockNs[write]+=since(t);++stats.count[write];
  if(failed){if(exclusive_){std::lock_guard<std::mutex> g(mutex_);forget(surface,write);wake_.notify_all();}held_.pop_back();throw std::runtime_error("scanout IOSurface lock failed");}
 }
 void quarantine(){quarantined_=true;}
 ~SurfaceLease() {
  if(quarantined_)return;
  // An uncertain submission keeps both the producer lock and the allocation
  // alive. The kernel quarantines DMA; no framebuffer reader may reuse it.
  if(!exclusive_){if(!poisoned_())for(auto it=held_.rbegin();it!=held_.rend();++it)unlock_(it->surface,it->write);return;}
  if(poisoned_()){std::lock_guard<std::mutex> guard(mutex_);wake_.notify_all();return;}
  for(auto it=held_.rbegin();it!=held_.rend();++it)unlock_(it->surface,it->write);
  std::lock_guard<std::mutex> guard(mutex_);
  for(auto &h:held_)forget(h.surface,h.write);
  wake_.notify_all();
 }
};
}
