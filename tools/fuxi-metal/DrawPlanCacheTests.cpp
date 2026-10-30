/* Shape rebasing, exact fallback, bounded replacement and deterministic pipeline address reuse; no GPU. */
#define FUXI_MTL_HOST_TEST
#include "DrawPlanCache.hpp"
#include <cassert>
#include <cstdio>
#include <new>
struct Pipeline {unsigned tag;};
int main(){
 using namespace fuxi;DrawPlanCache<Pipeline> cache;Pipeline a{1},b{2},variant{3};unsigned calls=0;
 auto uncached=[&](Pipeline *p,const DrawInput &input,Pipeline *&resolved,DrawOutput &out){
  ++calls;resolved=p==&a?&variant:p;memset(&out,int(p->tag+input.count),sizeof(out));out.count=input.count;out.first=input.start;out.indexOffset=input.indexOffset;
 };
 auto draw=[&](Pipeline *p,const DrawInput &input,Pipeline *&resolved,DrawOutput &out){if(!cache.find(p,input,resolved,out)){uncached(p,input,resolved,out);cache.insert(p,input,resolved,out);}};
 DrawInput inputs[2]{};inputs[0].count=3;inputs[1].count=6;inputs[1].buffers[1][15]={256,16,1};
 for(unsigned i=0;i<4;++i){auto *p=i%2?&b:&a;auto &input=inputs[i%2];Pipeline *resolved=nullptr,*expected=nullptr;DrawOutput out{},reference{};
  draw(p,input,resolved,out);assert(calls==(i?2u:1u));unsigned saved=calls;uncached(p,input,expected,reference);calls=saved;assert(resolved==expected && !memcmp(&out,&reference,sizeof(out)));
 }
 // The same requested pipeline can interleave complete inputs too.
 Pipeline *resolved=nullptr;DrawOutput out{};draw(&a,inputs[1],resolved,out);assert(calls==3);draw(&a,inputs[0],resolved,out);draw(&a,inputs[1],resolved,out);assert(calls==3);
 cache.invalidate(&variant);assert(!cache.find(&a,inputs[0],resolved,out) && !cache.find(&a,inputs[1],resolved,out));assert(cache.find(&b,inputs[1],resolved,out));
 cache.invalidate(&b);assert(!cache.find(&b,inputs[1],resolved,out));
 alignas(Pipeline) unsigned char storage[sizeof(Pipeline)];auto *p=new(storage) Pipeline{4};
 draw(p,inputs[0],resolved,out);assert(calls==4);cache.invalidate(p);p->~Pipeline();p=new(storage) Pipeline{9};
 assert(!cache.find(p,inputs[0],resolved,out));draw(p,inputs[0],resolved,out);assert(calls==5 && resolved==p);DrawOutput reference{};Pipeline *expected=nullptr;uncached(p,inputs[0],expected,reference);assert(!memcmp(&out,&reference,sizeof(out)));cache.invalidate(p);p->~Pipeline();
 static DrawPlanCache<Pipeline> bounded;DrawInput input{};DrawOutput value{};
 for(unsigned i=0;i<=DrawPlanCache<Pipeline>::capacity;++i){input.count=i;value.count=i;bounded.insert(&a,input,&variant,value);}
 input.count=0;assert(!bounded.find(&a,input,resolved,out));
 for(unsigned i=1;i<=DrawPlanCache<Pipeline>::capacity;++i){input.count=i;assert(bounded.find(&a,input,resolved,out) && resolved==&variant && out.count==i);}
 DrawRebase rules{};rules.enabled=1;rules.buffers[0]={1,1,3,0,0,16,256,65536};rules.buffers[1]={1,0,1,1,0,0,16,5000};
 rules.pushCount=3;rules.pushes[0]={0,0,1,0};rules.pushes[1]={0,8,2,19};rules.pushes[2]={1,0,3,0};
 DrawInput seed{};seed.count=3;seed.buffers[1][3]={4096,260,0};seed.buffers[0][1]={4096,16,0};seed.buffers[0][0]={4096,0,0};
 DrawOutput prototype{};prototype.count=3;prototype.descriptorCount=2;prototype.descriptors[0].offset=256;prototype.descriptors[0].range=32;prototype.descriptors[1].offset=16;prototype.descriptors[1].range=4080;
 prototype.vertexCount=1;prototype.vertexSlots[0]=0;prototype.pushCount=2;prototype.pushes[0].size=16;prototype.pushes[1].size=8;
 cache.insert(&a,seed,&variant,prototype,rules);auto changed=seed;changed.count=9;changed.start=2;changed.buffers[1][3]={5000,772,0};changed.buffers[0][1]={5000,32,0};changed.buffers[0][0]={5000,256,0};
 assert(cache.find(&a,changed,resolved,out) && resolved==&variant && out.count==9 && out.first==2 && out.vertexOffsets[0]==256);
 assert(out.descriptors[0].offset==768 && out.descriptors[0].range==32 && out.descriptors[1].offset==32 && out.descriptors[1].range==4976);
 uint64_t guest;memcpy(&guest,out.pushes[0].bytes+8,8);uint32_t base;memcpy(&base,out.pushes[1].bytes,4);uint32_t residual;memcpy(&residual,out.pushes[0].bytes,4);assert(guest==772 && base==2 && residual==4);
 auto miss=[&](DrawInput bad){assert(!cache.find(&a,bad,resolved,out));};
 auto bad=changed;bad.buffers[1][3].offset=769;miss(bad);bad=changed;bad.buffers[1][3].offset=5000;miss(bad);bad=changed;bad.buffers[1][3].offset=4992;miss(bad);
 bad=changed;bad.count=0;miss(bad);bad=changed;bad.count=65537;miss(bad);bad=changed;bad.start=65535;miss(bad);
 bad=changed;bad.buffers[0][15]={5000,0,1};miss(bad);bad=changed;bad.buffers[0][1].length=1u<<25;miss(bad);
 bad=changed;bad.buffers[0][0].length=16384;miss(bad);bad=changed;bad.buffers[0][1].length=6000;miss(bad);bad=changed;bad.formats[15]=80;miss(bad);bad=changed;bad.lodMin[15]=-0.0f;miss(bad);
 changed.indexed=1;changed.start=0;changed.indexLength=100;changed.indexOffset=4;prototype.indexed=1;cache.insert(&b,changed,&b,prototype,rules);
 changed.count=7;changed.indexOffset=8;assert(cache.find(&b,changed,resolved,out) && out.indexed==1 && out.count==7 && out.first==0 && out.indexOffset==8);
 bad=changed;bad.indexOffset=9;assert(!cache.find(&b,bad,resolved,out));bad=changed;bad.indexLength=20;assert(!cache.find(&b,bad,resolved,out));
 cache.invalidate(&variant);assert(!cache.find(&a,seed,resolved,out));
 puts("PASS: affine descriptors/vertices/draw bases, validation fallback, A/B hits, complete-input interleaving, byte-identical plans, requested/resolved invalidation, address reuse and bounded eviction");
}
