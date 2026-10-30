/* Authored adapter forces runtime variants and a coarse-key collision. */
#include "Backend.hpp"
#include <cassert>
int main(){
 using namespace fuxi;Backend b;
 auto *src=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16);auto *bytes=b.allocate(1024);
 memset(bytes->contents,37,1024);Operation upload{};upload.src=bytes;upload.texture=src;upload.upload=true;b.submit({upload});
 auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 void *bridge=dlopen(FUXI_MTL_TRANSLATOR,RTLD_NOW|RTLD_LOCAL);assert(bridge);
 auto counter=[&](const char *name){auto fn=reinterpret_cast<uint64_t (*)()>(dlsym(bridge,name));assert(fn);return fn();};
 Operation draw{};draw.encoderID=1;draw.texture=dst;draw.pipeline=p;draw.samples[0]=src;draw.sampleCount[0]=1;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};
 std::vector<Operation> ops(120,draw);for(unsigned i=0;i<120;++i){ops[i].vertices=i%2?6:3;ops[i].load=i?1:2;}
 bool trace=systemTranslationPolicy().trace;b.captureDrawPlans=true;
 b.submit(ops);assert(counter("fixture_base_calls")== (trace?120u:1u) && counter("fixture_variant_calls")== (trace?120u:2u) && counter("fixture_variant_creates")==1);
 auto plans=b.drawPlansForTest;
 b.submit(ops);assert(counter("fixture_base_calls")== (trace?240u:1u) && counter("fixture_variant_calls")== (trace?240u:2u) && p->variants.size()==1);
 b.reuseDrawPlans=false;b.submit(ops);assert(counter("fixture_base_calls")== (trace?360u:1u) && counter("fixture_variant_calls")== (trace?360u:122u));
 assert(plans.size()==b.drawPlansForTest.size());for(size_t i=0;i<plans.size();++i)assert(plans[i].first==b.drawPlansForTest[i].first && !memcmp(&plans[i].second,&b.drawPlansForTest[i].second,sizeof(DrawOutput)));
 b.reuseDrawPlans=true;
 // A candidate rejection must retry the baseline, rather than refuse a valid
 // draw or reuse a specialization selected for a different complete input.
 auto fallback=draw;fallback.first=1;fallback.vertices=3;fallback.load=1;
 b.submit({fallback,draw});assert(counter("fixture_base_calls")== (trace?362u:2u) && counter("fixture_variant_calls")== (trace?361u:123u) && counter("fixture_variant_creates")==1);
 if(!trace){
  auto *linear=b.allocateSampler(true,false,1);auto changed=draw;changed.samplers[0]=linear;
  b.submit({changed});assert(p->variants.size()==2 && counter("fixture_variant_creates")==2);
  auto base=counter("fixture_base_calls"),variants=counter("fixture_variant_calls");b.submit({fallback});assert(counter("fixture_base_calls")==base+1 && counter("fixture_variant_calls")==variants+1);
  variants=counter("fixture_variant_calls");b.dropPipelineCacheForTest();b.submit({draw});assert(counter("fixture_variant_calls")==variants+1);b.destroy(linear);
 }
 Operation read{};read.texture=dst;read.dst=bytes;b.submit({read});for(unsigned i=0;i<1024;++i)assert(((uint8_t *)bytes->contents)[i]==37);
 b.destroy(p);b.destroy(sam);b.destroy(src);b.destroy(dst);b.destroy(bytes);b.close();dlclose(bridge);
 puts("PASS: interleaved shapes reuse exact plans; uncached plans match byte-for-byte; coarse-key collision falls back; exact pixels");
}
