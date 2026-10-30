/* Authored MRT pixels through production Backend + dynamically loaded bridge. */
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
static void reject(const std::function<void()> &f){bool failed=false;try{f();}catch(const std::runtime_error &){failed=true;}assert(failed);}
static float half(uint16_t h){return std::ldexp(float(h&1023)+((h&0x7c00)?1024.f:0.f),int((h>>10)&31)-25)*(h&0x8000?-1:1);}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
 auto &tr=fuxi::Translator::get();char error[512]{};void *lib=tr.open(air.data(),air.size(),error,sizeof(error));assert(lib);
 fuxi::Backend b;auto *vertices=b.allocate(12*sizeof(float)),*read=b.allocate(4096);float triangle[]={-1,-1,3,-1,-1,3,-1,-1,3,-1,-1,3};memcpy(vertices->contents,triangle,sizeof(triangle));
 auto pixels=[&](fuxi::Texture *t,unsigned level){fuxi::Operation op{};op.texture=t;op.srcLevel=level;op.dst=read;memset(read->contents,0xa5,read->length);b.submit({op});};
 auto check=[&](fuxi::Texture *t,unsigned level,const std::array<float,4> &expected){pixels(t,level);unsigned channels=t->format==10?1:t->format==30?2:4;
  for(unsigned p=0;p<t->levelWidth(level)*t->levelHeight(level);++p)for(unsigned c=0;c<channels;++c){unsigned sw=t->format==80 && c<3?2-c:c;float value=t->format==115?half(((uint16_t *)read->contents)[4*p+c]):((uint8_t *)read->contents)[p*channels+sw]/255.f;
   if(std::abs(value-expected[c])>.006f){fprintf(stderr,"format=%u level=%u pixel=%u channel=%u got=%g expected=%g\n",t->format,level,p,c,value,expected[c]);abort();}}
 };
 for(unsigned count:{2u,4u,8u}){
  fuxi::CompileOptions options{};options.colorCount=count;options.attributes[0]={29,0,0};options.strides[0]=8;
  for(unsigned i=1;i<count;++i){auto &c=options.colors[i-1];c.format=i%4==1?70:i%4==2?115:30;c.blend=1;c.srcRGB=4;c.dstRGB=5;c.srcAlpha=1;c.dstAlpha=0;c.mask=i%4==3?8:15;}
  auto *pipeline=b.compilePipeline(lib,"mrt_vertex",lib,("mrt"+std::to_string(count)).c_str(),options);
  auto *same=b.compilePipeline(lib,"mrt_vertex",lib,("mrt"+std::to_string(count)).c_str(),options);assert(same==pipeline);b.destroy(same);
  auto changed=options;changed.colors[0].mask=8;auto *other=b.compilePipeline(lib,"mrt_vertex",lib,("mrt"+std::to_string(count)).c_str(),changed);assert(other!=pipeline);b.destroy(other);
  fuxi::Operation draw{};draw.colorCount=count;draw.colorMask=(1u<<count)-1;draw.pipeline=pipeline;draw.buffers[0][0]={vertices,0,false};draw.viewport={0,8,8,-8,0,1};draw.scissor={{0,0},{8,8}};
  std::vector<fuxi::Texture *> targets;
  for(unsigned i=0;i<count;++i){auto *t=b.allocateTexture(options.color(i).format,5,16,16,2);targets.push_back(t);fuxi::ColorTarget c{t,1,2,1,{.125f,.25f,.375f,.5f}};draw.setColor(i,c);}
  b.submit({draw});auto frames=b.cachedFrames();b.submit({draw});assert(b.cachedFrames()==frames);
  auto expected=[&](unsigned i){std::array<float,4> src=i%2?std::array<float,4>{.75f,.25f,.5f,.25f}:std::array<float,4>{.25f,.5f,.75f,.5f};
   if(i){auto alpha=src[3];for(unsigned c=0;c<3;++c)src[c]=src[c]*alpha+std::array<float,3>{.125f,.25f,.375f}[c]*(1-alpha);if(i%4==3)src[1]=.25f;}return src;};
  for(unsigned i=0;i<count;++i){assert(!targets[i]->isDefined(0) && targets[i]->isDefined(1));check(targets[i],1,expected(i));}
  // Same-encoder MRT merging preserves every attachment's independent blend
  // state and load/store actions, across 2/4/8 mixed-format mip targets.
  auto mergePair=[&](fuxi::Operation first,fuxi::Pipeline *nextPipeline=nullptr){first.encoderID=77;auto second=first;if(nextPipeline)second.pipeline=nextPipeline;
   for(unsigned i=0;i<count;++i){auto c=second.color(i);c.load=1;second.setColor(i,c);}
   b.submit({first});b.submit({second});std::vector<std::vector<uint8_t>> reference;
   for(auto *t:targets){pixels(t,1);auto n=t->levelWidth(1)*t->levelHeight(1)*fuxi::Backend::texelBytes(t->format);reference.emplace_back((uint8_t *)read->contents,(uint8_t *)read->contents+n);}
   fuxi::FrameSample sample;{fuxi::FrameTraceScope scope(&sample);b.submit({first,second});}
   bool merged=fuxi::systemTranslationPolicy().merge;assert(sample.passes==(merged?1u:2u) && sample.mergedDraws==unsigned(merged));
   assert(sample.mergeSplits[size_t(fuxi::MergeSplit::fetch)]==0);
   for(unsigned i=0;i<count;++i){pixels(targets[i],1);assert(!memcmp(read->contents,reference[i].data(),reference[i].size()));}
  };
  mergePair(draw);other=b.compilePipeline(lib,"mrt_vertex",lib,("mrt"+std::to_string(count)).c_str(),changed);mergePair(draw,other);b.destroy(other);
  auto first=draw;first.encoderID=77;auto last=first;for(unsigned i=0;i<count;++i){auto c=last.color(i);c.load=1;c.store=i%2;last.setColor(i,c);}
  fuxi::FrameSample discardSample;{fuxi::FrameTraceScope scope(&discardSample);b.submit({first,last});}assert(discardSample.passes==(fuxi::systemTranslationPolicy().merge?1u:2u));
  for(unsigned i=0;i<count;++i)assert(targets[i]->isDefined(1)==bool(i%2));b.submit({draw});
  // Mixed Load/Clear/DontCare and Store/DontCare; a full-coverage draw makes
  // DontCare-load output defined, and a discarded level refuses readback.
  auto mixed=draw;for(unsigned i=0;i<count;++i){auto c=mixed.color(i);c.load=i%3;c.store=i%2;mixed.setColor(i,c);}
  b.submit({mixed});for(unsigned i=0;i<count;++i){assert(targets[i]->isDefined(1)==bool(i%2));if(!(i%2))reject([&]{pixels(targets[i],1);});}
  b.submit({draw});
  // A partial empty pass clears only the top-left rectangle in every target.
  auto partial=draw;partial.pipeline=nullptr;partial.clearOnly=true;partial.areaWidth=3;partial.areaHeight=5;
  b.submit({partial});for(unsigned i=0;i<count;++i){pixels(targets[i],1);auto before=expected(i);unsigned channels=targets[i]->format==30?2:4;
   for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)for(unsigned c=0;c<channels;++c){auto want=x<3 && y<5?std::array<float,4>{.125f,.25f,.375f,.5f}[c]:before[c];unsigned sw=targets[i]->format==80 && c<3?2-c:c;auto pixel=y*8+x;float got=targets[i]->format==115?half(((uint16_t *)read->contents)[4*pixel+c]):((uint8_t *)read->contents)[pixel*channels+sw]/255.f;assert(std::abs(got-want)<.006f);}}
  b.submit({draw});
  // All attachment states roll back together before a queue attempt.
  auto bad=draw;bad.colors[0].level=0;reject([&]{b.submit({bad});});bad=draw;bad.colors[0].texture=targets[0];reject([&]{b.submit({bad});});
  auto layout=targets.back()->layout;b.failBeforeQueue=true;auto discarded=draw;for(unsigned i=0;i<count;++i){auto c=discarded.color(i);c.store=0;discarded.setColor(i,c);}reject([&]{b.submit({discarded});});for(auto *t:targets)assert(t->isDefined(1));assert(targets.back()->layout==layout && !b.poisoned);
  // Four independent ordered fetch inputs see each prior write in a single draw.
  if(count==4 && b.supportsOrderedColorAccess()){
   auto fetchOptions=options;fetchOptions.blend=0;for(auto &c:fetchOptions.colors)c.blend=0;
   auto *fetch=b.compilePipeline(lib,"mrt_vertex",lib,"fetch4",fetchOptions);assert(fetch->fetchMask==15);
   auto op=draw;op.pipeline=fetch;op.vertices=6;b.submit({op});
   for(unsigned i=0;i<count;++i){std::array<float,4> value=i%2?std::array<float,4>{1.f,.4375f,.84375f,.5f}:std::array<float,4>{.40625f,.8125f,1.21875f,.875f};if(i==0)value[2]=1;if(i==3)value[1]=.25f;check(targets[i],1,value);}mergePair(op);mergePair(op,pipeline);mergePair(draw,fetch);b.destroy(fetch);
  }
  for(auto *t:targets)b.destroy(t);b.destroy(pipeline);
 }
 // Sparse slots, including an absent color 0; Vulkan attachments are compact
 // but shader locations and input indices retain the Metal slot numbers.
 fuxi::CompileOptions sparse{};sparse.format=0;sparse.colorCount=8;sparse.colors[0].format=70;sparse.colors[6].format=115;sparse.attributes[0]={29,0,0};sparse.strides[0]=8;
 auto *sp=b.compilePipeline(lib,"mrt_vertex",lib,"sparse",sparse);auto *a=b.allocateTexture(70,5,8,8),*z=b.allocateTexture(115,5,8,8);
 fuxi::Operation op{};op.texture=a;op.colorCount=8;op.colorMask=130;op.colors[0].texture=a;op.colors[6].texture=z;op.pipeline=sp;op.buffers[0][0]={vertices,0,false};op.viewport={0,8,8,-8,0,1};op.scissor={{0,0},{8,8}};b.submit({op});check(a,0,{.75f,.25f,.5f,.25f});check(z,0,{.75f,.25f,.5f,.25f});
 op.encoderID=78;auto next=op;for(unsigned i:{1u,7u}){auto c=next.color(i);c.load=1;next.setColor(i,c);}
 fuxi::FrameSample sparseSample;{fuxi::FrameTraceScope scope(&sparseSample);b.submit({op,next});}assert(sparseSample.passes==(fuxi::systemTranslationPolicy().merge?1u:2u));check(a,0,{.75f,.25f,.5f,.25f});check(z,0,{.75f,.25f,.5f,.25f});
 b.destroy(a);b.destroy(z);b.destroy(sp);b.destroy(vertices);b.destroy(read);tr.close(lib);
 printf("M9l ordered_fetch=%u\n",unsigned(b.supportsOrderedColorAccess()));
 puts("PASS: M9l 2/4/8 MRT mixed formats/blend/masks, sparse slots 1+7, mip targets, independent actions, readbacks, alias/size bounds, rollback, cache identity/reuse, ordered four-input fetch when enabled");
}
