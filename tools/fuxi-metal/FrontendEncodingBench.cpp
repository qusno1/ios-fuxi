/* Linux CPU model of Frontend.mm's CA recording sequence; no ObjC/driver/GPU claim. */
#define FUXI_MTL_HOST_TEST
#include <TestBackend.hpp>
#ifndef FUXI_MTL_ENCODING_BASELINE
#include <FrontendEncoding.hpp>
#endif
#include <CAState.hpp>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <pthread.h>
using namespace fuxi;
struct Resource {enum Kind {buffer,texture,sampler,pipeline} kind;void *object;const void *wrapped=nullptr;Buffer *backing=nullptr;};
struct Fixture {
 Backend backend;std::array<uint8_t,65536> ring{};uint16_t quad[6]={0,1,2,2,1,3};
 std::array<Pipeline,4> pipelines;Texture target,samples[4];Sampler sampler;
 Resource vertex{Resource::buffer,nullptr,ring.data()},index{Resource::buffer,nullptr,quad},fragment{Resource::buffer,nullptr};
 Resource targetResource{Resource::texture,&target},sampleResources[4],samplerResource{Resource::sampler,&sampler},pipelineResources[4];
 std::string passText,pipelineText;
 Fixture(){
  vertex.object=backend.allocate(ring.size());index.object=backend.allocate(sizeof(quad));fragment.object=backend.allocate(256);
  target.width=1080;target.height=2400;Backend::modelDraw=[](const Operation &op){if(!op.pipeline || op.vertices!=6)std::abort();};
  for(unsigned i=0;i<4;++i){sampleResources[i]={Resource::texture,&samples[i]};pipelineResources[i]={Resource::pipeline,&pipelines[i]};
   for(auto &stage:pipelines[i].shadowWindows)for(auto &w:stage)w.kind=2;
   pipelines[i].options.strides[0]=32;pipelines[i].options.attributes[0]={31,0,0};}
  for(unsigned i=0;i<8;++i)passText+="color:texture=BGRA8Unorm width=1080 height=2400 level=0 slice=0 plane=0 load=clear store=store options=0 resolve=nil/0/0/0 clear=0,0,0,1;";
  for(unsigned i=0;i<8;++i)pipelineText+="attribute:float4 bufferIndex=0 offset=0 stride=32 stepFunction=perVertex stepRate=1;";
 }
 ~Fixture(){Backend::modelDraw={};backend.destroy((Buffer *)vertex.object);backend.destroy((Buffer *)index.object);backend.destroy((Buffer *)fragment.object);}
};
struct Command {
 struct Use {Resource *resource;ShadowRanges ranges;Use(Resource *r):resource(r),ranges(((Buffer *)r->object)->length){}};
 Fixture &fixture;std::vector<Operation> ops;std::unordered_set<void *> resourceSet;std::vector<Resource *> resources;
 std::unordered_map<Buffer *,Use> shadowUses;std::vector<std::unique_ptr<Resource>> inlineBuffers;ShadowSnapshot shadows;ShadowPool pool;
#ifndef FUXI_MTL_ENCODING_BASELINE
 CommandResourceCache recent;
#endif
 explicit Command(Fixture &f):fixture(f){}
 ~Command(){for(auto &r:inlineBuffers)fixture.backend.destroy((Buffer *)r->object);}
 __attribute__((noinline)) void retainCommandResource(Resource *r,bool bounded=false,ShadowFullReason reason=ShadowFullReason::other){
#ifndef FUXI_MTL_ENCODING_BASELINE
  if(recent.seen(r,bounded,reason))return;
#endif
  Buffer *b=r->kind==Resource::buffer?(Buffer *)r->object:r->kind==Resource::texture?r->backing:nullptr;
  if(b && r->wrapped){auto &use=shadowUses.try_emplace(b,r).first->second;if(!bounded)use.ranges.full(reason);}
  if(resourceSet.insert(r).second)resources.push_back(r);
 }
 __attribute__((noinline)) void setBytes(BufferBind &binding,const void *bytes,size_t n){
  auto r=std::make_unique<Resource>(Resource{Resource::buffer,fixture.backend.allocate(n)});memcpy(((Buffer *)r->object)->contents,bytes,n);
  retainCommandResource(r.get(),true);binding={(Buffer *)r->object,0,true};inlineBuffers.push_back(std::move(r));
 }
 __attribute__((noinline)) void prepareCommandShadows(){
  std::unordered_map<Buffer *,ShadowRanges> indices;uint64_t budget=0;
  for(const auto &op:ops)if(op.pipeline && op.indices){auto it=shadowUses.find(op.indices);if(it==shadowUses.end() || it->second.ranges.isFull())continue;
   if(!shadowIndexBytes(op.indices->length,op.indexOffset,op.vertices,op.indexWidth)){it->second.ranges.full(ShadowFullReason::bounds);continue;}
   if(op.vertices>ShadowIndexCache::drawLimit || op.vertices>ShadowIndexCache::frameLimit-budget)continue;
   if(std::any_of(ops.begin(),ops.end(),[&](const auto &write){return write.dst==op.indices;}))continue;
   budget+=op.vertices;indices.try_emplace(op.indices,op.indices->length).first->second.add(op.indexOffset,uint64_t(op.vertices)*op.indexWidth);
  }
  for(auto &entry:indices){auto &use=shadowUses.at(entry.first);shadows.add(entry.first->contents,use.resource->wrapped,entry.second);}shadows.capture(pool);
  ShadowIndexCache cache;uint64_t scanned=0;
  for(const auto &op:ops){ShadowIndexRange range;bool inspected=false;
   for(unsigned stage=0;stage<2;++stage)for(unsigned slot=0;slot<resourceSlots;++slot){auto b=op.buffers[stage][slot];if(!b.buffer || b.bytes)continue;auto it=shadowUses.find(b.buffer);if(it==shadowUses.end())continue;
    auto &ranges=it->second.ranges;ranges.binding(op.pipeline->shadowWindows[stage][slot],b.offset);
    if(!stage)for(auto a:op.pipeline->options.attributes)if(a.format && a.buffer==slot){
     if(!op.indices)ranges.vertex(b.offset,op.first,op.vertices,false,op.pipeline->options.strides[slot],a.offset,a.format);
     else {if(!inspected){auto bytes=shadows.view(op.indices->contents,op.indexOffset,uint64_t(op.vertices)*op.indexWidth);range=cache.scan(op.indices,bytes,op.indexOffset,op.vertices,op.indexWidth,primitiveRestart(op.topology),scanned);inspected=true;}
      ranges.vertexSpan(b.offset,range,op.pipeline->options.strides[slot],a.offset,a.format);}
    }
   }
  }
 }
 __attribute__((noinline)) void commit(){
  prepareCommandShadows();for(auto &entry:shadowUses){auto &use=entry.second;shadows.refresh(entry.first->contents,use.resource->wrapped,use.ranges);}shadows.release(pool);
#ifdef FUXI_MTL_ENCODING_BASELINE
  fixture.backend.submit(std::vector<Operation>(ops.begin(),ops.end()));
#else
  fixture.backend.submit(ops);
#endif
 }
};
struct Encoder {
 Command &command;Operation state{};Resource *pipeline=nullptr,*samples[resourceSlots]{},*samplers[resourceSlots]{};BufferBind buffers[2][resourceSlots]{};bool ended=false,drawn=false;uint32_t load,store;
 Encoder(Command &c,unsigned pass):command(c),load(pass%3),store(pass%4!=0){state.encoderID=pass+1;state.areaWidth=1080;state.areaHeight=2400;state.viewport={0,2400,1080,-2400,0,1};state.scissor={{0,0},{1080,2400}};}
 void active(){if(ended)std::abort();}
 __attribute__((noinline)) void setRenderPipelineState(Resource *p){active();if(p->kind!=Resource::pipeline)std::abort();if(p==pipeline)return;state.diagnostic=(command.fixture.passText+" "+command.fixture.pipelineText).c_str();pipeline=p;}
 void setBuffer(Resource *r,size_t offset,unsigned stage,unsigned slot){active();auto *b=(Buffer *)r->object;if(slot>=resourceSlots || r->kind!=Resource::buffer || offset>=b->length)std::abort();command.retainCommandResource(r,true);buffers[stage][slot]={b,offset,false};}
 void setVertexBufferOffset(size_t offset){active();auto &b=buffers[0][0];if(!bindingOffset(b.buffer!=nullptr,b.bytes,b.buffer?b.buffer->length:0,offset))std::abort();b.offset=offset;}
 void setFragmentTexture(Resource *r,unsigned slot){active();if(slot>=resourceSlots || r->kind!=Resource::texture)std::abort();samples[slot]=r;}
 void setFragmentSamplerState(Resource *r){active();if(r->kind!=Resource::sampler)std::abort();samplers[0]=r;}
 __attribute__((noinline)) void record(bool indexed){
  active();
  Operation op=state;
  op.renderDraw=true;op.texture=&command.fixture.target;op.pipeline=(Pipeline *)pipeline->object;op.vertices=6;op.load=drawn?1:load;op.store=1;
  for(unsigned i=0;i<resourceSlots;++i){if(samples[i]){op.samples[i]=(Texture *)samples[i]->object;op.sampleCount[i]=1;command.retainCommandResource(samples[i]);}if(samplers[i]){op.samplers[i]=(Sampler *)samplers[i]->object;command.retainCommandResource(samplers[i]);}}
  memcpy(op.buffers,buffers,sizeof(buffers));if(indexed){auto &r=command.fixture.index;op.indices=(Buffer *)r.object;command.retainCommandResource(&r,true);command.shadowUses.at(op.indices).ranges.add(0,12);}
  command.retainCommandResource(&command.fixture.targetResource);command.retainCommandResource(pipeline);
  command.ops.push_back(std::move(op));
  drawn=true;
 }
 void endEncoding(){active();command.ops.back().store=store;ended=true;}
};
static volatile uint64_t checksum=0;
__attribute__((noinline)) static void frame(Fixture &fixture){
 Command command(fixture);uint8_t bytes[256]{};unsigned draw=0;
 for(unsigned pass=0;pass<31;++pass){Encoder encoder(command,pass);unsigned count=pass<25?5:4;
  for(unsigned i=0;i<count;++i,++draw){encoder.setRenderPipelineState(&fixture.pipelineResources[(draw/2)%4]);encoder.setBuffer(&fixture.vertex,draw*128,0,0);encoder.setVertexBufferOffset(draw*128+32);
   command.setBytes(encoder.buffers[0][1],bytes,16*(1+draw%16));encoder.setBuffer(&fixture.fragment,0,1,0);
   encoder.setFragmentTexture(&fixture.sampleResources[(draw/3)%4],0);if(i%2)encoder.setFragmentTexture(&fixture.sampleResources[(draw/5+1)%4],1);encoder.setFragmentSamplerState(&fixture.samplerResource);
   if(draw%4==0){if(!scissorFits(0,0,1080,2400,1080,2400) || !viewportFits(0,0,1080,2400,0,1,1080,2400))std::abort();encoder.state.scissor={{0,0},{1080,2400}};encoder.state.viewport={0,2400,1080,-2400,0,1};}
   encoder.record(draw%2);
  }encoder.endEncoding();
 }if(draw!=149 || command.ops.size()!=149)std::abort();checksum+=command.ops.size()+command.resources.size();command.commit();
}
#if defined(__linux__) && defined(__x86_64__)
extern "C" char __executable_start,etext;
static uintptr_t pcs[131072],callers[131072],stackLow=0,stackHigh=0;static volatile sig_atomic_t samples=0;
static void samplePC(int,siginfo_t *,void *context){
 auto *u=(ucontext_t *)context;auto n=samples;if(n>=131072)return;
 uintptr_t pc=u->uc_mcontext.gregs[REG_RIP],fp=u->uc_mcontext.gregs[REG_RBP];pcs[n]=pc;
 if(pc<uintptr_t(&__executable_start) || pc>=uintptr_t(&etext))for(unsigned depth=0;depth<24;++depth){
  if(fp<stackLow || fp>stackHigh-2*sizeof(uintptr_t) || fp%alignof(uintptr_t))break;
  auto *frame=(const uintptr_t *)fp;auto ret=frame[1];if(ret>=uintptr_t(&__executable_start) && ret<uintptr_t(&etext)){callers[n]=ret-1;break;}
  auto next=frame[0];if(next<=fp)break;fp=next;
 }
 samples=n+1;
}
static void profile(bool start){struct itimerval timer{};if(start){pthread_attr_t attr;void *base=nullptr;size_t bytes=0;pthread_getattr_np(pthread_self(),&attr);pthread_attr_getstack(&attr,&base,&bytes);pthread_attr_destroy(&attr);stackLow=uintptr_t(base);stackHigh=stackLow+bytes;struct sigaction action{};action.sa_sigaction=samplePC;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);sigaction(SIGPROF,&action,nullptr);timer.it_interval.tv_usec=1000;timer.it_value=timer.it_interval;}setitimer(ITIMER_PROF,&timer,nullptr);}
#endif
int main(int argc,char **argv){
 unsigned frames=argc>1?std::strtoul(argv[1],nullptr,10):20000;bool sampling=argc>2 && !strcmp(argv[2],"--profile");if(!frames)return 2;
 Fixture fixture;for(unsigned i=0;i<200;++i)frame(fixture);
#if defined(__linux__) && defined(__x86_64__)
 if(sampling)profile(true);
#else
 if(sampling){fputs("sampling requires Linux x86_64\n",stderr);return 2;}
#endif
 auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<frames;++i)frame(fixture);
 auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
#if defined(__linux__) && defined(__x86_64__)
 if(sampling){profile(false);std::unordered_map<uintptr_t,unsigned> counts;for(sig_atomic_t i=0;i<samples;++i)++counts[callers[i]?callers[i]:pcs[i]];
  for(auto [pc,count]:counts){Dl_info info{};dladdr((void *)pc,&info);printf("PC %lx %u %s\n",(unsigned long)pc,count,info.dli_sname?info.dli_sname:info.dli_fname?info.dli_fname:"?");}}
#endif
 printf("frames=%u draws/frame=149 encoders/frame=31 sizeof(Operation)=%zu ns/draw=%.1f ns/frame=%.1f checksum=%llu\n",frames,sizeof(Operation),ns/frames/149,ns/frames,(unsigned long long)checksum);
}
