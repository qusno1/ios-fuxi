/* Commit-owned packed snapshots; unknown access preserves the full-copy path. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "VertexProfile.hpp"
namespace fuxi {
struct ShadowWindow {uint32_t kind=0,reserved=0;int64_t start=0;uint64_t size=0;}; // 0 whole, 1 proven argument window, 2 no argument access
static_assert(sizeof(ShadowWindow)==24,"shadow window ABI");
enum class ShadowFullReason : size_t {indexed,wholeWindow,useResource,blit,bounds,other,count};
inline constexpr const char *shadowFullReasonNames[]={"indexed","whole_window","use_resource","blit","bounds","other"};
struct ShadowInterval {uint64_t offset,size;};
struct ShadowIndexRange {uint32_t first=0,last=0;bool valid=false,empty=true;};
inline bool shadowIndexBytes(uint64_t length,uint64_t offset,uint32_t count,uint32_t width){return (width==2 || width==4) && offset%width==0 && offset<=length && uint64_t(count)*width<=length-offset;}
inline ShadowIndexRange shadowVertexIndices(ShadowIndexRange r,int64_t base){
 if(!r.valid || r.empty)return r;
 if(base<0){uint64_t n=uint64_t(-(base+1))+1;if(n>r.first)return {};r.first-=uint32_t(n);r.last-=uint32_t(n);}
 else {if(uint64_t(base)>UINT32_MAX-r.last)return {};r.first+=uint32_t(base);r.last+=uint32_t(base);}return r;
}
// Only command-owned frozen bytes enter this cache; its lifetime is one commit.
class ShadowIndexCache {
 struct Entry {const void *identity;uint64_t offset;uint32_t count,width;bool restart;ShadowIndexRange range;};
 std::array<Entry,64> entries{};size_t used=0,next=0;
public:
 static constexpr uint32_t drawLimit=65536,frameLimit=262144;
 ShadowIndexRange scan(const void *identity,const void *bytes,uint64_t offset,uint32_t count,uint32_t width,bool restart,uint64_t &scanned){
  if(!bytes)return {};for(size_t i=0;i<used;++i){auto &e=entries[i];if(e.identity==identity && e.offset==offset && e.count==count && e.width==width && e.restart==restart)return e.range;}
  if((width!=2 && width!=4) || count>drawLimit || scanned>frameLimit || count>frameLimit-scanned)return {};
  ShadowIndexRange r;r.valid=true;r.first=UINT32_MAX;uint32_t sentinel=width==2?UINT16_MAX:UINT32_MAX;
  for(uint32_t i=0;i<count;++i){uint32_t index=0;memcpy(&index,(const uint8_t *)bytes+uint64_t(i)*width,width);if(restart && index==sentinel)continue;r.empty=false;r.first=std::min(r.first,index);r.last=std::max(r.last,index);}
  scanned+=count;entries[next]={identity,offset,count,width,restart,r};next=(next+1)%entries.size();used=std::min(used+1,entries.size());return r;
 }
};
class ShadowRanges {
 uint64_t length;bool whole=false;uint32_t reasons=0;std::vector<ShadowInterval> intervals;
public:
 explicit ShadowRanges(uint64_t n):length(n){}
 void full(ShadowFullReason reason=ShadowFullReason::other){whole=true;reasons|=1u<<size_t(reason);intervals.clear();}
 bool fullReason(ShadowFullReason reason) const{return reasons&(1u<<size_t(reason));}
 void add(uint64_t offset,uint64_t size){
  if(offset>length || size>length-offset){full(ShadowFullReason::bounds);return;}if(whole || !size)return;
  uint64_t end=offset+size;auto first=intervals.begin();while(first!=intervals.end() && first->offset+first->size<offset)++first;
  auto last=first;while(last!=intervals.end() && last->offset<=end){offset=std::min(offset,last->offset);end=std::max(end,last->offset+last->size);++last;}
  if(first==last)intervals.insert(first,{offset,end-offset});else {first->offset=offset;first->size=end-offset;intervals.erase(first+1,last);}
 }
 void texture(uint64_t offset,uint64_t row,uint64_t width,uint64_t height,uint64_t pixel){
  if(!width || !height || !pixel || width>UINT64_MAX/pixel){full(ShadowFullReason::bounds);return;}uint64_t tight=width*pixel;
  if(row<tight || row%pixel || offset%pixel || offset>length || tight>length-offset || height-1>(length-offset-tight)/row){full(ShadowFullReason::bounds);return;}
  add(offset,(height-1)*row+tight);
 }
 void binding(const ShadowWindow &w,uint64_t offset){
  if(w.kind==2)return;if(w.kind!=1){full(w.kind==0?ShadowFullReason::wholeWindow:ShadowFullReason::other);return;}
  if(w.start<0){uint64_t delta=uint64_t(-(w.start+1))+1;if(delta>offset){full(ShadowFullReason::bounds);return;}offset-=delta;}
  else {if(uint64_t(w.start)>length || offset>length-uint64_t(w.start)){full(ShadowFullReason::bounds);return;}offset+=uint64_t(w.start);}
  add(offset,w.size);
 }
 void vertexSpan(uint64_t offset,ShadowIndexRange r,uint32_t stride,uint32_t attribute,uint32_t format){
  if(!r.valid){full(ShadowFullReason::indexed);return;}if(r.empty)return;
  if(r.first>r.last || !vertexFetchFits(length,offset,r.last,stride,attribute,format)){full(ShadowFullReason::bounds);return;}
  add(offset+uint64_t(r.first)*stride+attribute,uint64_t(r.last-r.first)*stride+vertexFormatInfo(format).bytes);
 }
 void vertex(uint64_t offset,uint32_t first,uint32_t count,bool indexed,uint32_t stride,uint32_t attribute,uint32_t format){
  if(indexed){full(ShadowFullReason::indexed);return;}if(!count || first>UINT32_MAX-(count-1)){full(ShadowFullReason::bounds);return;}
  vertexSpan(offset,{first,first+count-1,true,false},stride,attribute,format);
 }
 template<class F> void each(F &&f) const {if(whole){if(length)f(ShadowInterval{0,length});}else for(auto r:intervals)f(r);}
 uint64_t bytes() const {uint64_t n=0;each([&](auto r){n+=r.size;});return n;}
 bool isFull() const{return whole;}
};
struct ShadowStorage {std::unique_ptr<uint8_t[]> data;size_t capacity=0;};
class ShadowPool {
 std::mutex mutex;std::array<ShadowStorage,2> cached;
public:
 ShadowStorage take(size_t bytes){
  {std::lock_guard<std::mutex> lock(mutex);for(auto &s:cached)if(s.data && s.capacity>=bytes)return std::move(s);}
  return {std::unique_ptr<uint8_t[]>(new uint8_t[bytes]),bytes};
 }
 void put(ShadowStorage storage){
  if(!storage.data)return;std::lock_guard<std::mutex> lock(mutex);
  for(auto &s:cached)if(!s.data){s=std::move(storage);return;}
  auto &s=cached[cached[0].capacity<cached[1].capacity?0:1];if(storage.capacity>s.capacity)s=std::move(storage);
 }
};
struct ShadowSegment {void *destination;const void *source;uint64_t offset,size,packed;};
class ShadowSnapshot {
 ShadowStorage storage;std::vector<ShadowSegment> segments;size_t size=0,captured=0;
 template<class F> void pieces(void *destination,uint64_t offset,uint64_t bytes,F &&f) const {
  uint64_t end=offset+bytes;while(offset<end){const ShadowSegment *hit=nullptr;uint64_t next=end;
   for(const auto &r:segments)if(r.destination==destination){if(r.offset<=offset && offset-r.offset<r.size){hit=&r;break;}if(r.offset>offset)next=std::min(next,r.offset);}
   uint64_t stop=hit?std::min(end,hit->offset+hit->size):next;f(offset,stop-offset,hit);offset=stop;
  }
 }
public:
 void add(void *destination,const void *source,const ShadowRanges &ranges){
  std::vector<ShadowSegment> added;ranges.each([&](auto r){pieces(destination,r.offset,r.size,[&](uint64_t offset,uint64_t bytes,const ShadowSegment *hit){if(!hit){added.push_back({destination,source,offset,bytes,size});size+=bytes;}});});segments.insert(segments.end(),added.begin(),added.end());
 }
 size_t bytes() const{return size;}
 void capture(ShadowPool &pool){
  if(size==captured)return;if(!storage.data || storage.capacity<size){auto next=pool.take(size);if(captured)memcpy(next.data.get(),storage.data.get(),captured);pool.put(std::move(storage));storage=std::move(next);}
  for(auto r:segments)if(r.packed>=captured)memcpy(storage.data.get()+r.packed,(const uint8_t *)r.source+r.offset,r.size);captured=size;
 }
 const void *view(void *destination,uint64_t offset,uint64_t bytes) const {
  for(auto r:segments)if(r.destination==destination && offset>=r.offset && offset-r.offset<=r.size && bytes<=r.size-(offset-r.offset) && r.packed+r.size<=captured)return storage.data.get()+r.packed+offset-r.offset;return nullptr;
 }
 uint64_t refresh(void *destination,const void *source,const ShadowRanges &ranges) const {
  uint64_t copied=0;ranges.each([&](auto r){pieces(destination,r.offset,r.size,[&](uint64_t offset,uint64_t bytes,const ShadowSegment *hit){const void *from=hit?storage.data.get()+hit->packed+offset-hit->offset:(const uint8_t *)source+offset;memcpy((uint8_t *)destination+offset,from,bytes);copied+=bytes;});});return copied;
 }
 template<class F> void eachWrite(F &&f) const {for(auto &r:segments)f(r.destination,r.offset,r.size);}
 // `resolve` maps a segment's stable destination key to the storage currently backing it.
 template<class R> size_t apply(ShadowPool &pool,R &&resolve){for(auto r:segments)memcpy((uint8_t *)resolve(r.destination)+r.offset,storage.data.get()+r.packed,r.size);size_t copied=size;release(pool);return copied;}
 size_t apply(ShadowPool &pool){return apply(pool,[](void *d){return d;});}
 void release(ShadowPool &pool){pool.put(std::move(storage));segments.clear();size=captured=0;}
};
}
