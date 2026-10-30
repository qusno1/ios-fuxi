/* Host controls use the same tracker, packer and pool as the frontend. */
#include "ShadowRanges.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>
using namespace fuxi;
int main(){
 // Every commit freezes and copies its ranges, including unchanged bytes.
 {
  ShadowPool pool;std::vector<uint8_t> cpu(12288,19),gpu(12288,3);ShadowRanges r(cpu.size());r.add(256,9000);ShadowSnapshot a,b,c;
  auto capture=[&](ShadowSnapshot &s){s.add(gpu.data(),cpu.data(),r);s.capture(pool);assert(s.bytes()==9000);};
  capture(a);capture(b);assert(a.apply(pool)==9000 && b.apply(pool)==9000);
  cpu[5000]=29;capture(a);cpu[5000]=39;capture(b);cpu[5000]=29;capture(c);cpu[5000]=99;
  assert(a.apply(pool)==9000 && gpu[5000]==29);assert(b.apply(pool)==9000 && gpu[5000]==39);assert(c.apply(pool)==9000 && gpu[5000]==29);
  assert(gpu[255]==3 && gpu[9256]==3);
  // Intervening GPU writes are overwritten by the frozen version too.
  cpu[5000]=29;capture(a);gpu[5000]=0;assert(a.apply(pool)==9000 && gpu[5000]==29);
 }
 ShadowRanges merged(1024);merged.add(80,20);merged.add(20,20);merged.add(40,40);merged.add(15,5);merged.add(90,5);assert(merged.bytes()==85);
 unsigned intervals=0;merged.each([&](auto r){assert(r.offset==15 && r.size==85);++intervals;});assert(intervals==1);
 merged.add(1024,0);assert(merged.bytes()==85);merged.add(UINT64_MAX,2);assert(merged.isFull() && merged.bytes()==1024);
 // A byte-mask oracle checks arbitrary insertion order, overlap and adjacency.
 ShadowRanges random(1024);std::array<bool,1024> mask{};uint32_t seed=123;
 for(unsigned i=0;i<300;++i){seed=1664525*seed+1013904223;unsigned offset=seed%1024;seed=1664525*seed+1013904223;unsigned size=seed%33;size=std::min(size,1024-offset);random.add(offset,size);for(unsigned j=offset;j<offset+size;++j)mask[j]=true;
  std::array<bool,1024> covered{};random.each([&](auto r){for(uint64_t j=r.offset;j<r.offset+r.size;++j)covered[j]=true;});assert(covered==mask);
 }
 ShadowRanges texture(4096);texture.texture(256,32,7,5,2);assert(texture.bytes()==142);texture.each([](auto r){assert(r.offset==256 && r.size==142);});
 texture.texture(256,32,7,5,2);assert(texture.bytes()==142);texture.texture(384,32,7,2,2);assert(texture.bytes()==174);
 texture.add(2000,16);assert(texture.bytes()==190);texture.full(ShadowFullReason::useResource);texture.texture(256,32,7,5,2);assert(texture.bytes()==4096 && texture.fullReason(ShadowFullReason::useResource));
 for(auto d:{std::array<uint64_t,5>{0,16,0,5,1},{0,16,7,0,1},{0,16,7,5,0},{0,16,UINT64_MAX,5,2},{UINT64_MAX,16,7,5,1},{0,UINT64_MAX,7,5,1},{0,16,7,UINT64_MAX,1},{0,4,7,5,1},{1,16,7,5,2},{0,15,7,5,2}}){ShadowRanges r(4096);r.texture(d[0],d[1],d[2],d[3],d[4]);assert(r.isFull() && r.fullReason(ShadowFullReason::bounds));}
 ShadowRanges tight(142);tight.texture(0,32,7,5,2);assert(!tight.isFull() && tight.bytes()==142);ShadowRanges shortTexture(141);shortTexture.texture(0,32,7,5,2);assert(shortTexture.isFull());
 ShadowRanges negative(1024);negative.binding({1,0,-8,24},128);assert(negative.bytes()==24);negative.each([](auto r){assert(r.offset==120);});
 negative.binding({1,0,std::numeric_limits<int64_t>::min(),1},0);assert(negative.isFull());
 for(auto w:{ShadowWindow{},ShadowWindow{3,0,0,4},ShadowWindow{1,0,1000,32},ShadowWindow{1,0,0,UINT64_MAX}}){ShadowRanges r(1024);r.binding(w,8);assert(r.isFull());}
 ShadowRanges unused(1024);unused.binding({2,0,0,0},0);assert(!unused.bytes());
 ShadowRanges vertices(4096);vertices.vertex(256,2,6,false,32,8,29);assert(vertices.bytes()==168);vertices.each([](auto r){assert(r.offset==328);});
 vertices.vertex(0,0,3,true,32,0,29);assert(vertices.isFull());
 ShadowRanges badVertex(1024);badVertex.vertex(0,UINT32_MAX,3,false,32,0,29);assert(badVertex.isFull());
 // Scans read frozen bytes, include restart only for strips, and never cross commits.
 for(uint32_t width:{2u,4u}){
  std::array<uint8_t,64> bytes{};uint32_t values[]={3,1,2,width==2?UINT16_MAX:UINT32_MAX,4,4};for(unsigned i=0;i<6;++i)memcpy(bytes.data()+8+i*width,&values[i],width);
  assert(shadowIndexBytes(bytes.size(),8,6,width) && !shadowIndexBytes(bytes.size(),9,6,width) && !shadowIndexBytes(16,8,6,width) && !shadowIndexBytes(UINT64_MAX,UINT64_MAX-1,6,width));
  ShadowIndexCache cache;uint64_t scanned=0;auto r=cache.scan(bytes.data(),bytes.data()+8,8,6,width,true,scanned);assert(r.valid && !r.empty && r.first==1 && r.last==4 && scanned==6);
  ShadowRanges bounded(4096);bounded.vertexSpan(256,r,32,8,29);assert(bounded.bytes()==104);bounded.each([](auto i){assert(i.offset==296);});
  auto base=shadowVertexIndices(r,5);assert(base.first==6 && base.last==9);ShadowRanges shifted(4096);shifted.vertexSpan(256,base,32,8,29);shifted.each([](auto i){assert(i.offset==456 && i.size==104);});
  base=shadowVertexIndices(r,-1);assert(base.valid && base.first==0 && base.last==3);assert(!shadowVertexIndices(r,-2).valid && !shadowVertexIndices(r,INT64_MIN).valid && !shadowVertexIndices(r,UINT32_MAX).valid);
  assert(cache.scan(bytes.data(),bytes.data()+8,8,6,width,true,scanned).last==4 && scanned==6);
  auto list=cache.scan(bytes.data(),bytes.data()+8,8,6,width,false,scanned);assert(list.last==values[3] && scanned==12);ShadowRanges out(4096);out.vertexSpan(0,list,32,0,31);assert(out.isFull() && out.fullReason(ShadowFullReason::bounds));
  auto changed=cache.scan(bytes.data(),bytes.data()+8,8,3,width,false,scanned);assert(changed.first==1 && changed.last==3 && scanned==15);
  for(unsigned i=0;i<6;++i)memcpy(bytes.data()+8+i*width,&values[3],width);ShadowIndexCache next;auto empty=next.scan(bytes.data(),bytes.data()+8,8,6,width,true,scanned);assert(empty.valid && empty.empty);ShadowRanges none(1024);none.vertexSpan(0,empty,32,0,31);assert(!none.bytes());
  auto sentinel=next.scan(bytes.data(),bytes.data()+8,8,6,width,false,scanned);assert(!sentinel.empty && sentinel.last==values[3]);
  assert(!next.scan(bytes.data(),bytes.data()+8,0,ShadowIndexCache::drawLimit+1,width,true,scanned).valid);
  scanned=ShadowIndexCache::frameLimit;assert(!next.scan(bytes.data(),bytes.data()+8,16,3,width,true,scanned).valid);
  ShadowRanges unstable(4096);unstable.vertexSpan(0,next.scan(bytes.data(),nullptr,0,3,width,true,scanned),32,0,31);assert(unstable.isFull() && unstable.fullReason(ShadowFullReason::indexed));
 }
 ShadowRanges reasons(1024);reasons.binding({},0);reasons.full(ShadowFullReason::useResource);reasons.full(ShadowFullReason::blit);reasons.full(ShadowFullReason::other);reasons.vertex(0,0,3,true,32,0,31);reasons.vertex(0,UINT32_MAX,3,false,32,0,31);
 for(size_t i=0;i<size_t(ShadowFullReason::count);++i)assert(reasons.fullReason(ShadowFullReason(i)));
 ShadowPool pool;auto storage=pool.take(1024);auto *address=storage.data.get();pool.put(std::move(storage));storage=pool.take(512);assert(storage.data.get()==address);pool.put(std::move(storage));
 constexpr size_t ringBytes=4*1024*1024,draws=130,needed=draws*256;
 std::vector<uint8_t> cpu(ringBytes),gpu(ringBytes,0);ShadowSnapshot first,second;
 auto ranges=[&](unsigned frame){ShadowRanges r(ringBytes);for(unsigned i=0;i<draws;++i){uint64_t offset=65536+frame*needed+i*256;r.vertex(offset,0,6,false,32,0,31);r.vertex(offset,0,6,false,32,16,31);r.binding({1,0,0,64},offset+192);}assert(r.bytes()==needed);return r;};
 // CPU writes both inside and outside the proven window after capture.
 for(unsigned frame=0;frame<3;++frame){std::fill(cpu.begin(),cpu.end(),uint8_t(19+frame));auto r=ranges(frame);first.add(gpu.data(),cpu.data(),r);first.capture(pool);assert(first.bytes()==needed);std::fill(cpu.begin(),cpu.end(),77);
  std::fill(gpu.begin(),gpu.end(),3);assert(first.apply(pool)==needed);for(size_t i=0;i<ringBytes;++i)assert(gpu[i]==(i>=65536+frame*needed && i<65536+(frame+1)*needed?19+frame:3));
 }
 // Pending snapshots never share storage, even when the same ring is reused.
 auto r=ranges(0);std::fill(cpu.begin(),cpu.end(),11);first.add(gpu.data(),cpu.data(),r);first.capture(pool);std::fill(cpu.begin(),cpu.end(),22);second.add(gpu.data(),cpu.data(),r);second.capture(pool);std::fill(cpu.begin(),cpu.end(),33);
 assert(first.apply(pool)==needed && gpu[65536]==11);assert(second.apply(pool)==needed && gpu[65536]==22);
 ShadowRanges full(ringBytes);full.binding({},123);first.add(gpu.data(),cpu.data(),full);first.capture(pool);cpu.back()=99;assert(first.apply(pool)==ringBytes && gpu.back()==33);
 std::array<uint8_t,128> otherCPU{},otherGPU{};otherCPU.fill(42);ShadowRanges other(128);other.add(48,16);std::fill(cpu.begin(),cpu.end(),14);
 first.add(gpu.data(),cpu.data(),r);first.add(otherGPU.data(),otherCPU.data(),other);first.capture(pool);otherCPU.fill(99);assert(first.apply(pool)==needed+16 && gpu[65536]==14);
 for(unsigned i=0;i<128;++i)assert(otherGPU[i]==(i>=48 && i<64?42:0));
 // Return/take from separate commit and submit threads uses the pool lock.
 std::thread worker([&]{for(unsigned i=0;i<100;++i){auto s=pool.take(2048);assert(s.data && s.capacity>=2048);pool.put(std::move(s));}});
 for(unsigned i=0;i<100;++i){auto s=pool.take(1024);assert(s.data && s.capacity>=1024);pool.put(std::move(s));}worker.join();
 // Indexed CA-like quads share the ring with their indices and fragment constants.
 constexpr size_t indexedNeeded=draws*(128+64+12);ShadowRanges indexRanges(ringBytes),indexedRanges(ringBytes);std::vector<bool> touched(ringBytes);
 for(unsigned i=0;i<draws;++i){uint64_t offset=65536+i*256;uint16_t quad[]={0,1,2,2,1,3};memcpy(cpu.data()+offset+192,quad,sizeof(quad));indexRanges.add(offset+192,sizeof(quad));}
 first.add(gpu.data(),cpu.data(),indexRanges);first.capture(pool);ShadowIndexCache cache;uint64_t scanned=0;
 for(unsigned i=0;i<draws;++i){uint64_t offset=65536+i*256;auto indices=cache.scan(gpu.data(),first.view(gpu.data(),offset+192,12),offset+192,6,2,false,scanned);assert(indices.valid && indices.last==3);
  indexedRanges.vertexSpan(offset,indices,32,0,31);indexedRanges.vertexSpan(offset,indices,32,16,31);indexedRanges.binding({1,0,0,64},offset+128);indexedRanges.add(offset+192,12);
 }
 assert(scanned==draws*6 && indexedRanges.bytes()==indexedNeeded);std::fill(cpu.begin(),cpu.end(),55);first.add(gpu.data(),cpu.data(),indexedRanges);first.capture(pool);assert(first.bytes()==indexedNeeded);
 std::fill(cpu.begin(),cpu.end(),77);std::fill(gpu.begin(),gpu.end(),3);assert(first.apply(pool)==indexedNeeded);
 for(unsigned i=0;i<draws;++i){uint64_t offset=65536+i*256;for(unsigned j=0;j<192;++j)assert(gpu[offset+j]==55);uint16_t quad[]={0,1,2,2,1,3};assert(!memcmp(gpu.data()+offset+192,quad,12));for(unsigned j=0;j<204;++j)touched[offset+j]=true;}
 for(size_t i=0;i<ringBytes;++i)if(!touched[i])assert(gpu[i]==3);
 // Direct synchronous refresh also uses the frozen indices, including full fallback.
 std::array<uint16_t,8> indexCPU{0,1,2,2,1,3,7,8},indexGPU{};ShadowRanges ir(sizeof(indexCPU));ir.add(0,12);first.add(indexGPU.data(),indexCPU.data(),ir);first.capture(pool);indexCPU.fill(99);ir.full();assert(first.refresh(indexGPU.data(),indexCPU.data(),ir)==sizeof(indexCPU));first.release(pool);assert(indexGPU[0]==0 && indexGPU[5]==3 && indexGPU[6]==99);
 std::array<uint8_t,128> stagedCPU{},stagedGPU{};stagedCPU.fill(11);ShadowRanges frozen(128);frozen.add(16,8);frozen.add(64,8);first.add(stagedGPU.data(),stagedCPU.data(),frozen);first.capture(pool);assert(!first.view(stagedGPU.data(),16,9));stagedCPU.fill(22);ShadowRanges expanded(128);expanded.full();first.add(stagedGPU.data(),stagedCPU.data(),expanded);first.capture(pool);assert(first.bytes()==128);stagedCPU.fill(33);assert(first.apply(pool)==128);
 for(unsigned i=0;i<128;++i)assert(stagedGPU[i]==((i>=16 && i<24) || (i>=64 && i<72)?11:22));
 printf("PASS: indexed u16/u32, base vertex, restart/list sentinels, bounds/reasons, frozen scans/cache/budgets; indexed CA-like ring bytes/frame before=%zu after=%zu (snapshot and backend copy each)\n",ringBytes,indexedNeeded);
 printf("PASS: shadow ranges, immutable commits, full fallback, two flights, pool reuse; CA-like ring bytes/frame before=%zu after=%zu (snapshot and backend copy each)\n",ringBytes,needed);
}
