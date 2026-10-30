/* Real Vulkan alias pixels, scratch allocation counts, retry and quarantine. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
static fuxi::Operation alias(fuxi::Texture *t,unsigned width,unsigned height){
 fuxi::Operation op{};op.texture=t;op.sourceTexture=t;op.width=width;op.height=height;return op;
}
int main(){
 using namespace fuxi;
 assert(systemTranslationPolicy().frameTrace);bool reuse=systemTranslationPolicy().pipelined;
 Backend backend;backend.buffersPooled=false;auto baseline=backend.resourceBytes();auto objects=liveMemoryObjects.load();
 auto *t=backend.allocateTexture(80,5,1024,512);auto *up=backend.allocate(1024*512*4),*out=backend.allocate(1024*512*4);
 for(size_t i=0;i<up->length;++i)((uint8_t *)up->contents)[i]=uint8_t(i*13%251);
 Operation upload{};upload.texture=t;upload.src=up;upload.upload=true;
 Operation read{};read.texture=t;read.dst=out;
 backend.submit({upload});auto charged=backend.resourceBytes();
 auto before=frameCounts.allocations;uint64_t prepare=0;
 for(unsigned i=0;i<12;++i){FrameSample s;
  // Change the source between fences, and overlap source/destination rows.
  // Reusing stale snapshot bytes or copying in-place must fail readback.
  for(size_t j=0;j<up->length;++j)((uint8_t *)up->contents)[j]=uint8_t((j*13+i*17)%251);
  std::vector<uint8_t> expected((uint8_t *)up->contents,(uint8_t *)up->contents+up->length);
  for(unsigned y=0;y<128;++y)memcpy(expected.data()+(y+1)*4096+(i+1)*4,(uint8_t *)up->contents+y*4096+i*4,128*4);
  auto overlapping=alias(t,128,128);overlapping.srcX=i;overlapping.dstX=i+1;overlapping.dstY=1;
  {FrameTraceScope scope(&s);backend.submit({upload,overlapping,read});}
  assert(!memcmp(out->contents,expected.data(),expected.size()));
  assert(!s.draws && !s.passes && s.backendSubmit && s.gpuWait);
  assert(bool(s.backendCPU)==systemTranslationPolicy().frameCPU);
  assert(s.record && s.kernelIO && s.queueSubmit && !s.recordCPU && !s.kernelIOCPU && !s.queueSubmitCPU);prepare+=s.backendSubmit;
  if(backend.supportsTimestamps())assert(s.gpuExec);
  assert(backend.resourceBytes()==charged+(reuse?65536:0));
 }
 assert(frameCounts.allocations-before==(reuse?1:12));backend.submit({upload});
 // Growing reclaims the old quiescent allocation first; identical full snapshots
 // preserve every pixel, including the >1 MiB transient fallback.
 backend.submit({alias(t,256,256)});
 assert(backend.resourceBytes()==charged+(reuse?262144:0));
 auto largeBefore=frameCounts.allocations;backend.submit({alias(t,1024,512)});
 assert(frameCounts.allocations==largeBefore+1 && backend.resourceBytes()==charged+(reuse?262144:0));
 backend.failBeforeQueue=true;bool failed=false;
 try{backend.submit({alias(t,128,128)});}catch(const std::runtime_error&){failed=true;}
 assert(failed && !backend.poisoned);
 unsigned scheduled=0;backend.submit({alias(t,128,128),read},[&]{++scheduled;});
 assert(scheduled==1 && !memcmp(out->contents,up->contents,up->length));
 // A clear still reports an actual Vulkan render pass; empty submits don't wait.
 Operation clear{};clear.texture=t;clear.clearOnly=true;clear.clear[3]=1;clear.areaWidth=512;
 FrameSample render;{FrameTraceScope scope(&render);backend.submit({clear});}assert(render.passes==1 && !render.draws);
 FrameSample empty;{FrameTraceScope scope(&empty);backend.submit({});}assert(!empty.gpuWait && !empty.gpuExec && !empty.passes);
 // Async retirement must not extend submit-thread CPU time through the fence.
 FrameSample deferred;std::function<void()> retire;
 {FrameTraceScope scope(&deferred);backend.submit({upload,read},{},&retire);}
 auto submittedCPU=deferred.backendCPU;assert(retire && bool(submittedCPU)==systemTranslationPolicy().frameCPU && !deferred.gpuWait);
 std::thread retireWorker([&]{FrameTraceScope scope(&deferred);retire();});retireWorker.join();
 assert(deferred.backendCPU==submittedCPU && deferred.gpuWait);
 backend.destroy(t);backend.destroy(up);backend.destroy(out);backend.trimMemory();
 assert(backend.resourceBytes()==baseline+(reuse?262144:0));backend.close();
 assert(liveMemoryObjects==objects); // retained scratch is reclaimed on clean close
 {
  Backend lost;auto *texture=lost.allocateTexture(80,5,16,16);auto *buffer=lost.allocate(1024);
  upload.texture=texture;upload.src=buffer;lost.submit({upload});
  lost.failQueueAttempt=true;failed=false;
  try{lost.submit({alias(texture,16,16)});}catch(const std::runtime_error&){failed=true;}
  assert(failed && lost.poisoned);failed=false;
  try{lost.submit({alias(texture,16,16)});}catch(const std::runtime_error&){failed=true;}
  assert(failed);lost.close(); // intentionally retain unknown DMA ownership
 }
 printf("PASS: pipelined=%u alias allocations=%u/12 prepare_ns=%llu pixels/growth/cap/retry/render/empty/close/quarantine\n",unsigned(reuse),reuse?1:12,(unsigned long long)prepare);
}
