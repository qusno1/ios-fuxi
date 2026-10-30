/* CA-shaped frame: 150 draws in 30 passes over 6 offscreen targets, one unique sampled texture per draw (a descriptor set per draw), alternating viewports. Host CPU only. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;
 Backend b;auto *data=b.allocate(16384);memset(data->contents,91,16384);
 std::vector<Texture *> pool,targets;for(unsigned i=0;i<100;++i){auto *t=b.allocateTexture(80,5,16,16);Operation u{};u.texture=t;u.src=data;u.upload=true;b.submit({u});pool.push_back(t);}
 for(unsigned i=0;i<6;++i){auto *t=b.allocateTexture(80,5,64,64);Operation u{};u.texture=t;u.src=data;u.upload=true;b.submit({u});targets.push_back(t);}
 auto *p=b.compilePipeline(nullptr,"control",nullptr,"control",CompileOptions{});auto *sam=b.allocateSampler(false,false,1);
 std::vector<Operation> ops;
 for(unsigned pass=0;pass<30;++pass)for(unsigned k=0;k<5;++k){
  Operation d{};d.encoderID=pass+1;d.texture=targets[pass%6];d.pipeline=p;d.samplers[0]=sam;d.vertices=6;d.sampleCount[0]=1;
  d.samples[0]=(k==0 && pass>=6)?targets[(pass+5)%6]:pool[(pass*5+k)%100];
  d.viewport={float(k),float(16+k),16,-16,0,1};d.scissor={{int32_t(k),0},{16,16}};d.load=k?1:2;ops.push_back(d);}
 for(unsigned i=0;i<5;++i)b.submit(ops);
 FrameTraceWindow window;auto sb=frameCounts.descriptors;
 for(unsigned i=0;i<100;++i){FrameSample s;{FrameTraceScope scope(&s);b.submit(ops);}window.add(s);}
 auto sets=frameCounts.descriptors-sb;printf("CA_BENCH view_creates=%llu frame_creates=%llu\n",(unsigned long long)Backend::submitViewCreates.load(),(unsigned long long)Backend::submitFrameCreates.load());
 printf("CA_BENCH draws=%zu median_submit_ms=%.3f p90=%.3f passes/frame=%.1f image_barriers/frame=%.1f sets/frame=%.1f\n",ops.size(),window.percentile(&FrameSample::backendSubmit,50)/1e6,window.percentile(&FrameSample::backendSubmit,90)/1e6,double(window.percentile(&FrameSample::passes,50)),double(window.percentile(&FrameSample::imageBarriers,50)),sets/100.0);
 if(getenv("CA_BENCH_REPORT"))window.report(stdout);
 for(auto *t:pool)b.destroy(t);for(auto *t:targets)b.destroy(t);b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(data);b.close();
}
