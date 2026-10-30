/* Sub-allocation of small device-local image memory from large blocks; no Vulkan or platform dependencies. Caller serializes and frees a range only after the GPU is done with it. */
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace fuxi {
inline std::atomic<uint64_t> bufferPoolHits{0},bufferPoolMisses{0},bufferPoolTrims{0},bufferPoolBytes{0};
inline std::atomic<uint64_t> slabHits{0},slabGrows{0},slabGrowCold{0},slabGrowFrag{0},slabGrowFull{0},slabGrowRefused{0},slabFrees{0},slabTrims{0},slabBytes{0},slabFreeBytes{0};
template<class Handle> class SlabMemory {
public:
 struct Block {Handle memory{};uint64_t offset{},size{};uint32_t slab{};};
 enum Result {ok,grow,ineligible};
 static constexpr uint64_t slabSize=16ull<<20,maxBlock=4ull<<20,idleNanos=30ull*1000*1000*1000;
 static constexpr size_t maxSlabs=8;
private:
 struct Range {uint64_t offset,size;};
 struct Slab {Handle memory{};uint32_t type{};bool live=false;uint64_t used=0,idle=0;std::vector<Range> free;};
 std::array<Slab,maxSlabs> slabs{};
 static uint64_t up(uint64_t n,uint64_t a){return (n+a-1)&~(a-1);}
 template<class Free> void dropSlab(Slab &s,Free &free){free(s.memory,slabSize);slabBytes-=slabSize;slabFreeBytes-=slabSize;s=Slab{};++slabTrims;}
public:
 uint64_t bytes() const{uint64_t n=0;for(auto &s:slabs)if(s.live)n+=slabSize;return n;}
 uint64_t emptyBytes() const{uint64_t n=0;for(auto &s:slabs)if(s.live && !s.used)n+=slabSize;return n;}
 bool canGrow() const{for(auto &s:slabs)if(!s.live)return true;return false;}
 // Best fit (least leftover) over slabs whose type is permitted; `align` must be a power of two.
 Result alloc(uint64_t need,uint64_t align,uint32_t typeBits,Block &out){
  if(!need || need>maxBlock || !align || (align&(align-1)) || align>maxBlock)return ineligible;
  uint64_t size=up(need,align),waste=UINT64_MAX,matching=0,freeTotal=0;Slab *bs=nullptr;size_t bi=0;uint64_t bstart=0;
  for(auto &s:slabs){if(!s.live || s.type>=32 || !(typeBits&(1u<<s.type)))continue;++matching;
   for(size_t i=0;i<s.free.size();++i){auto &r=s.free[i];freeTotal+=r.size;uint64_t start=up(r.offset,align);
    if(start+size<=r.offset+r.size){uint64_t left=r.offset+r.size-start-size;if(left<waste){waste=left;bs=&s;bi=i;bstart=start;}}}}
  if(!bs){++slabGrows;if(!matching)++slabGrowCold;else if(freeTotal>=size)++slabGrowFrag;else ++slabGrowFull;return grow;}
  auto r=bs->free[bi];uint64_t end=bstart+size;
  if(bstart>r.offset){bs->free[bi].size=bstart-r.offset;if(end<r.offset+r.size)bs->free.insert(bs->free.begin()+bi+1,Range{end,r.offset+r.size-end});}
  else if(end<r.offset+r.size)bs->free[bi]={end,r.offset+r.size-end};
  else bs->free.erase(bs->free.begin()+bi);
  bs->used+=size;slabFreeBytes-=size;++slabHits;out={bs->memory,bstart,size,uint32_t(bs-slabs.data())};return ok;
 }
 // Registers a fresh slab (all free); false when the slab count is capped.
 bool add(Handle memory,uint32_t type,uint64_t now){
  for(auto &s:slabs)if(!s.live){s.memory=memory;s.type=type;s.live=true;s.used=0;s.idle=now;s.free.assign(1,Range{0,slabSize});slabBytes+=slabSize;slabFreeBytes+=slabSize;return true;}
  ++slabGrowRefused;return false;
 }
 void release(const Block &b,uint64_t now){
  if(b.slab>=slabs.size())return;auto &s=slabs[b.slab];if(!s.live || s.memory!=b.memory || b.size>s.used)return;
  auto at=std::upper_bound(s.free.begin(),s.free.end(),b.offset,[](uint64_t o,const Range &r){return o<r.offset;});
  at=s.free.insert(at,Range{b.offset,b.size});size_t i=at-s.free.begin();
  if(i+1<s.free.size() && s.free[i].offset+s.free[i].size==s.free[i+1].offset){s.free[i].size+=s.free[i+1].size;s.free.erase(s.free.begin()+i+1);}
  if(i && s.free[i-1].offset+s.free[i-1].size==s.free[i].offset){s.free[i-1].size+=s.free[i].size;s.free.erase(s.free.begin()+i);}
  s.used-=b.size;slabFreeBytes+=b.size;++slabFrees;if(!s.used)s.idle=now;
 }
 // Frees empty slabs idle for idleNanos, always keeping the newest empty slab per type warm; `all` frees every empty slab.
 template<class Free> void trim(uint64_t now,bool all,Free free){
  for(auto &s:slabs){if(!s.live || s.used)continue;
   if(!all){bool newest=true;for(auto &o:slabs)if(&o!=&s && o.live && !o.used && o.type==s.type && (o.idle>s.idle || (o.idle==s.idle && &o>&s)))newest=false;
    if(newest || now<s.idle || now-s.idle<idleNanos)continue;}
   dropSlab(s,free);}
 }
 // Frees empty slabs (longest idle first) until at least `need` bytes are released.
 template<class Free> void reclaim(uint64_t need,Free free){
  while(need){Slab *oldest=nullptr;for(auto &s:slabs)if(s.live && !s.used && (!oldest || s.idle<oldest->idle))oldest=&s;
   if(!oldest)return;dropSlab(*oldest,free);need=slabSize>=need?0:need-slabSize;}
 }
};
}
