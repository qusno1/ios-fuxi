/* Caller serializes planning and pipeline lifetime changes. No resource ownership. */
#pragma once
#include <array>
#include "Translator.hpp"
namespace fuxi {
struct DrawPlanShape {
 uint8_t buffers[2][resourceSlots]{};uint32_t indexed{},strip{};
 uint32_t formats[resourceSlots]{},filters[resourceSlots]{};float lodMin[resourceSlots]{},lodMax[resourceSlots]{};
 explicit DrawPlanShape(const DrawInput &i){
  indexed=i.indexed;strip=i.strip;memcpy(formats,i.formats,sizeof(formats));memcpy(filters,i.filters,sizeof(filters));memcpy(lodMin,i.lodMin,sizeof(lodMin));memcpy(lodMax,i.lodMax,sizeof(lodMax));
  for(unsigned s=0;s<2;++s)for(unsigned n=0;n<resourceSlots;++n){auto b=i.buffers[s][n];if(!b.length)continue;
   uint64_t padded=(b.length+15)&~uint64_t(15);unsigned bits=padded?64-__builtin_clzll(padded):64;
   buffers[s][n]=b.bytes?128:uint8_t(bits*4+(b.offset%4==0?2:b.offset%2==0?1:0));
  }
 }
 bool operator==(const DrawPlanShape &b)const{return !memcmp(this,&b,sizeof(b));}
};
static_assert(sizeof(DrawPlanShape)==2*resourceSlots+8+16*resourceSlots,"shape key has no padding");
inline bool drawRebaseBounds(const DrawInput &i){
 if(!i.count || i.count>65536 || i.start>65536-i.count || (i.indexed && i.start))return false;
 if(i.indexed && (i.indexOffset%2 || i.indexOffset>i.indexLength || uint64_t(i.count)*2>i.indexLength-i.indexOffset))return false;
 for(auto &stage:i.buffers)for(auto b:stage)if(b.length && (b.offset>=b.length || b.length>16*1024*1024 || (b.bytes && (b.offset || b.length>4096))))return false;
 return true;
}
inline bool rebaseDrawPlan(const DrawInput &old,const DrawInput &i,const DrawOutput &plan,const DrawRebase &rules,DrawOutput &out){
 if(!drawRebaseBounds(i))return false;
 uint64_t offsets[16]{},ranges[16]{};
 for(unsigned n=0;n<plan.descriptorCount;++n){auto r=rules.buffers[n];if(!r.active)continue;
  auto b=i.buffers[r.stage][r.slot],prior=old.buffers[r.stage][r.slot];uint64_t a=r.alignment;
  // Residues and clipped windows are not affine. Let the full planner handle them.
  if(!a || b.offset%a!=prior.offset%a)return false;
  uint64_t end=(b.length+15)&~uint64_t(15);__int128 start=__int128(b.offset)+r.start;
  if(!r.whole && (start<0 || start+r.size>__int128(end)))return false;
  uint64_t lo=r.whole?b.offset/a*a:uint64_t(start)/a*a;
  uint64_t hi=r.whole?end:(uint64_t(start+r.size)+15)&~uint64_t(15);
  if(hi<lo || hi-lo<16 || hi-lo>r.maxRange)return false;offsets[n]=lo;ranges[n]=hi-lo;
 }
 memcpy(&out,&plan,sizeof(out));out.count=i.count;out.first=i.indexed?0:i.start;out.indexOffset=i.indexed?i.indexOffset:0;
 for(unsigned n=0;n<plan.descriptorCount;++n)if(rules.buffers[n].active){out.descriptors[n].offset=offsets[n];out.descriptors[n].range=ranges[n];}
 for(unsigned n=0;n<plan.vertexCount;++n){auto b=i.buffers[0][plan.vertexSlots[n]];out.vertexOffsets[n]=b.bytes?0:b.offset;}
 for(unsigned n=0;n<rules.pushCount;++n){auto r=rules.pushes[n];auto *dst=out.pushes[r.push].bytes+r.offset;
  if(r.kind==1){auto b=rules.buffers[r.source];uint32_t value=uint32_t(i.buffers[b.stage][b.slot].offset-offsets[r.source]);memcpy(dst,&value,4);}
  else if(r.kind==2){auto b=i.buffers[r.source/resourceSlots][r.source%resourceSlots];uint64_t value=b.length?b.offset:0;memcpy(dst,&value,8);}
  else if(r.kind==3){uint32_t value[2]={i.indexed?0:i.start,0};memcpy(dst,value,8);}
 }
 return true;
}
// CA streams more than 32 shapes per frame; 32 entries missed ~30 draws/frame on device.
template<class Pipeline> class DrawPlanCache {
public:
 static constexpr size_t capacity=128;
private:
 struct Entry {Pipeline *requested{},*resolved{};DrawInput input{};DrawOutput output{};DrawRebase rules{};DrawPlanShape shape{DrawInput{}};};
 std::array<Entry,capacity> entries{};size_t next=0;
public:
 bool find(Pipeline *requested,const DrawInput &input,Pipeline *&resolved,DrawOutput &output) const {
  DrawPlanShape shape(input);
  for(auto &e:entries)if(e.requested && e.requested==requested){
   if(e.rules.enabled){if(!(e.shape==shape))continue;if(!rebaseDrawPlan(e.input,input,e.output,e.rules,output)){if(!sameDrawInput(e.input,input))continue;memcpy(&output,&e.output,sizeof(output));}}
   else {if(!sameDrawInput(e.input,input))continue;memcpy(&output,&e.output,sizeof(output));}
   resolved=e.resolved;return true;
  }
  return false;
 }
 void insert(Pipeline *requested,const DrawInput &input,Pipeline *resolved,const DrawOutput &output,const DrawRebase &rules={}) {
  auto &e=entries[next];next=(next+1)%entries.size();e.requested=requested;e.resolved=resolved;memcpy(&e.input,&input,sizeof(input));memcpy(&e.output,&output,sizeof(output));e.rules=rules;e.shape=DrawPlanShape(input);
 }
 void invalidate(Pipeline *p){for(auto &e:entries)if(e.requested==p || e.resolved==p)e.requested=e.resolved=nullptr;}
};
}
