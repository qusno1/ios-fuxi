/* Full/ranged snapshots through production Vulkan submit/retirement and pixels. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;Backend backend;ShadowPool pool;constexpr size_t length=4*1024*1024;
 auto *indices=backend.allocate(128);auto *buffer=backend.allocate(length),*read=backend.allocate(16*16*4);auto *target=backend.allocateTexture(80,4,16,16);CompileOptions options;options.strides[0]=8;options.attributes[0]={29,0,0};auto *pipeline=backend.compilePipeline(nullptr,"control",nullptr,"control",options);
 bool old=getenv("SHADOW_OLD_ADAPTER");assert(pipeline->shadowWindows[1][0].kind==(old?0u:1u));
 std::vector<uint8_t> cpu(length),reference,actual;uint64_t copied=0;
 auto run=[&](bool full,unsigned frame,unsigned indexWidth){
  size_t offset=65536+frame*256;float color[]={.25f,.5f,.75f,1};std::fill(cpu.begin(),cpu.end(),0);memcpy(cpu.data()+offset,color,sizeof(color));float positions[][2]={{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};if(indexWidth){positions[3][0]=positions[3][1]=1;}memcpy(cpu.data()+offset+64,positions,(indexWidth?4:6)*8);memset(buffer->contents,3,length);
  std::array<uint8_t,128> indexCPU{};uint32_t quad[]={0,1,2,2,1,3};for(unsigned i=0;i<6;++i)memcpy(indexCPU.data()+16+i*indexWidth,&quad[i],indexWidth);
  ShadowRanges indexRanges(128);if(indexWidth)indexRanges.add(16,6*indexWidth);
  ShadowRanges ranges(length);ranges.binding(full?ShadowWindow{}:pipeline->shadowWindows[1][0],offset);ShadowSnapshot snapshot;snapshot.add(indices->contents,indexCPU.data(),indexRanges);snapshot.capture(pool);if(indexWidth){ShadowIndexCache cache;uint64_t scanned=0;auto r=cache.scan(indices,snapshot.view(indices->contents,16,6*indexWidth),16,6,indexWidth,false,scanned);assert(r.valid && r.first==0 && r.last==3 && scanned==6);ranges.vertexSpan(offset+64,r,8,0,29);}
  if(!indexWidth)ranges.vertex(offset+64,0,6,false,8,0,29);snapshot.add(buffer->contents,cpu.data(),ranges);snapshot.capture(pool);
  assert(snapshot.bytes()==(full || old?length:indexWidth?48:64)+6*indexWidth);std::fill(cpu.begin(),cpu.end(),255);indexCPU.fill(255);
  Operation draw{};draw.texture=target;draw.pipeline=pipeline;draw.vertices=6;draw.indices=indexWidth?indices:nullptr;draw.indexWidth=indexWidth?indexWidth:2;draw.indexOffset=16;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.buffers[1][0]={buffer,offset,false};draw.buffers[0][0]={buffer,offset+64,false};
  std::function<void()> retire;backend.submit({draw},{},systemTranslationPolicy().overlap?&retire:nullptr,[&]{copied+=snapshot.apply(pool);});if(retire)retire();
  assert(((uint8_t *)buffer->contents)[0]==(full || old?0:3));assert(((uint8_t *)buffer->contents)[offset]==((uint8_t *)color)[0]);
  Operation get{};get.texture=target;get.dst=read;backend.submit({get});return std::vector<uint8_t>((uint8_t *)read->contents,(uint8_t *)read->contents+16*16*4);
 };
 for(unsigned indexWidth:{0u,2u,4u})for(unsigned frame=0;frame<3;++frame){reference=run(true,frame,indexWidth);actual=run(false,frame,indexWidth);assert(actual==reference);for(size_t i=0;i<actual.size();i+=4){assert(actual[i]==191 && actual[i+1]==128 && actual[i+2]==64 && actual[i+3]==255);}}
 assert(copied==3*(length+(old?length:64))+6*(length+(old?length:48))+216);backend.destroy(indices);backend.destroy(pipeline);backend.destroy(target);backend.destroy(read);backend.destroy(buffer);
 printf("PASS: lavapipe nonindexed/u16/u32 full/ranged pixel equality, post-commit writes, prepare/retirement (%s adapter, %s submit)\n",old?"old/full":"window",systemTranslationPolicy().overlap?"overlap":"synchronous");
}
