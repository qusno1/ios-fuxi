/* Pixel A/B controls on the production backend and authored GLSL. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 setenv("FUXI_MTL_SHADOW_HASH","0",1); // counts every refresh copy; the unchanged-backing skip is tested separately
 using namespace fuxi;Backend b;bool merge=systemTranslationPolicy().merge;bool cross=merge && (systemTranslationPolicy().perf&2);
 auto *src=b.allocateTexture(80,5,16,16),*other=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16,3);
 auto *bytes=b.allocate(1024),*read=b.allocate(1024);auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 auto put=[&](Texture *t,uint8_t value,uint32_t level=0){memset(bytes->contents,value,1024);if(t==src || t==other)for(unsigned i=3;i<1024;i+=4)((uint8_t *)bytes->contents)[i]=t==src?16:32;Operation op{};op.src=bytes;op.texture=t;op.upload=true;op.dstLevel=level;b.submit({op});};
 put(src,91);put(other,37);
 Operation draw{};draw.encoderID=1;draw.pipeline=p;draw.texture=dst;draw.samples[0]=src;draw.sampleCount[0]=1;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.clear[0]=.2f;draw.clear[1]=.4f;draw.clear[2]=.6f;draw.clear[3]=1;
 void *proxy=dlopen(FUXI_MTL_ICD,RTLD_NOW|RTLD_LOCAL);assert(proxy);
 auto counter=[&](const char *name){auto fn=reinterpret_cast<uint64_t (*)()>(dlsym(proxy,name));assert(fn);return fn();};
 auto run=[&](const std::vector<Operation> &ops,unsigned passes,unsigned merged){auto beforePass=counter("render_merge_passes"),beforeDraw=counter("render_merge_draws");auto beforeImages=counter("render_merge_image_barriers"),beforeSubmits=counter("render_merge_submits");FrameSample s;{FrameTraceScope scope(&s);b.submit(ops);}assert(s.queueSubmits==1 && counter("render_merge_submits")-beforeSubmits==1);if(s.draws){assert(s.pipe && s.record && s.desc && !s.pipeCPU && !s.recordCPU && !s.descCPU);assert(bool(s.backendCPU)==systemTranslationPolicy().frameCPU);}assert(counter("render_merge_image_barriers")-beforeImages==s.imageBarriers);assert(s.passes==(merge?passes:s.draws+std::count_if(ops.begin(),ops.end(),[](const Operation &o){return o.clearOnly && (o.areaWidth || o.areaHeight);})));assert(counter("render_merge_passes")-beforePass==s.passes && counter("render_merge_draws")-beforeDraw==s.draws);assert(s.mergedDraws==(merge?merged:0));return s;};
 auto pixels=[&](const char *label,uint32_t level=0){Operation get{};get.texture=dst;get.dst=read;get.srcLevel=level;b.submit({get});printf("PIXELS %s ",label);for(size_t i=0;i<dst->levelWidth(level)*dst->levelHeight(level)*4;++i)printf("%02x",((uint8_t *)read->contents)[i]);puts("");};
 // Exact metadata reuse keeps all 120 draws and resource identities. The
 // independent proxy confirms redundant binds are absent inside a merged run.
 void *bridge=dlopen(FUXI_MTL_TRANSLATOR,RTLD_NOW|RTLD_LOCAL);assert(bridge);
 auto plans=reinterpret_cast<uint64_t (*)()>(dlsym(bridge,"render_merge_plan_calls"));assert(plans);
 std::vector<Operation> repeated(120,draw);for(unsigned i=0;i<120;++i)repeated[i].load=i?1:2;
 auto beforePlans=plans(),beforePipelines=counter("render_merge_pipeline_binds"),beforeSets=counter("render_merge_set_binds");
 auto beforeViewports=counter("render_merge_viewport_sets"),beforeScissors=counter("render_merge_scissor_sets");
 auto beforeFormats=counter("render_merge_format_queries");
 run(repeated,1,119);assert(plans()-beforePlans==1 && counter("render_merge_format_queries")==beforeFormats);
 auto expected=merge?1u:120u;
 assert(counter("render_merge_pipeline_binds")-beforePipelines==expected);
 assert(counter("render_merge_viewport_sets")-beforeViewports==expected && counter("render_merge_scissor_sets")-beforeScissors==expected);
 assert(counter("render_merge_set_binds")-beforeSets==(merge && systemTranslationPolicy().submitFast?1u:120u));
 pixels("repeated-plan-and-state");
 // Only the new shape misses; the previous shape survives across submissions.
 repeated[60].first=1;repeated[60].vertices=3;beforePlans=plans();run(repeated,1,119);assert(plans()-beforePlans==1);
 pixels("changed-plan-input");dlclose(bridge);
 if(b.supportsOrderedColorAccess()){
  auto *fetch=b.compilePipeline(nullptr,"control",nullptr,"fetch");assert(fetch->fetchMask==1);
  auto f=draw;f.pipeline=fetch;run({f},1,0);pixels("fetch-clear");
  // Fetch sees the loadOp clear on the first draw, including alpha. Verify an
  // independent expected value as well as byte-identical A/B output.
  for(unsigned i=0;i<1024;++i){float clear=draw.clear[(i%4)==0?2:(i%4)==2?0:i%4];float sample=(i%4)==3?16:91;
   assert(std::abs(int(((uint8_t *)read->contents)[i])-int(std::lround(clear*255*.75+sample*.25)))<=1);}
  std::vector<Operation> mixed(19,draw);for(unsigned i=0;i<mixed.size();++i){auto &op=mixed[i];op.load=i?1:2;if(i%3!=1)op.pipeline=fetch;op.samples[0]=i%2?other:src;op.scissor.extent.width=16-i%5;}
  auto s=run(mixed,1,18);assert(s.mergeSplits[size_t(MergeSplit::fetch)]==0);pixels("fetch-normal-interleaved");
  // A fetch must use the current attachment view after switching target mips.
  for(auto &op:mixed){op.dstLevel=1;op.viewport={0,8,8,-8,0,1};op.scissor.extent={8,8};}run(mixed,1,18);pixels("fetch-mip-view",1);
  std::function<void()> fetchFirst,fetchSecond;b.submit(mixed,{},&fetchFirst);b.submit(mixed,{},&fetchSecond);fetchFirst();fetchSecond();pixels("fetch-two-flights",1);
  b.destroy(fetch);
 }else puts("SKIP: lavapipe lacks enabled VK_EXT_rasterization_order_attachment_access; ordered fetch pixel controls");
 // The proxy masks linear sampling support so this uses the real production
 // optimal-shadow fallback regardless of the host's preferred linear layout.
 auto *backing=b.allocate(4096);auto *shadow=b.linearTexture(backing,80,1,16,16,0,64);assert(shadow->shadow);
 auto *backing2=b.allocate(4096);auto *shadow2=b.linearTexture(backing2,80,1,16,16,0,64);assert(shadow2->shadow);
 std::vector<Operation> shadowDraws(11,draw);for(unsigned i=0;i<shadowDraws.size();++i){auto &op=shadowDraws[i];op.load=i?1:2;op.samples[0]=i%2?shadow2:shadow;}
 auto shadowLayout=shadow->layout;auto shadowDefined=shadow->defined;bool shadowFailed=false;b.failBeforeQueue=true;
 try{b.submit(shadowDraws);}catch(...){shadowFailed=true;}assert(shadowFailed && !b.poisoned && shadow->layout==shadowLayout && shadow->defined==shadowDefined);
 std::vector<uint8_t> prior;
 for(unsigned frame=0;frame<3;++frame){memset(backing->contents,37+frame*61,4096);memset(backing2->contents,63+frame*43,4096);
  auto before=counter("render_merge_copies");auto s=run(shadowDraws,1,10);assert(s.mergeSplits[size_t(MergeSplit::shadow)]==0);
  assert(counter("render_merge_copies")-before==(merge?2u:11u));pixels(("shadow-cpu-frame-"+std::to_string(frame)).c_str());
  std::vector<uint8_t> now((uint8_t *)read->contents,(uint8_t *)read->contents+1024);if(frame)assert(now!=prior);prior=now;
 }
 // Explicit backing-buffer writes stay outside passes. The refresh after the
 // fill must observe it; run-local deduplication must never persist past a run.
 auto sh=shadowDraws[0],shLoad=sh;shLoad.load=1;Operation fill{};fill.dst=backing;fill.size=4096;fill.value=199;
 auto beforeCopies=counter("render_merge_copies");run({sh,fill,shLoad},2,0);assert(counter("render_merge_copies")-beforeCopies==2);pixels("shadow-after-fill");
 // Attachment writes can also change a shadow's backing memory via a linear
 // image alias. Such refreshes depend on earlier draws and must remain split.
 auto *alias=b.linearTexture(backing,80,4,16,16,0,64);assert(!alias->shadow);
 auto aliasDraw=sh;aliasDraw.texture=alias;auto aliasLoad=aliasDraw;aliasLoad.load=1;
 auto split=run({aliasDraw,aliasLoad},2,0);if(merge)assert(split.mergeSplits[size_t(MergeSplit::shadow)]==1);
 auto aliasNormal=aliasLoad;aliasNormal.samples[0]=src;run({aliasDraw,aliasNormal},1,1);
 split=run({aliasDraw,aliasNormal,aliasLoad},2,1);if(merge)assert(split.mergeSplits[size_t(MergeSplit::shadow)]==1);
 Operation aliasRead{};aliasRead.sourceTexture=alias;aliasRead.texture=dst;b.submit({aliasRead});pixels("shadow-attachment-producer");
 // Change CPU bytes between submissions while both tickets stay outstanding.
 // Independent frame backing avoids racing a live GPU read of shared bytes.
 auto *flightTarget=b.allocateTexture(80,5,16,16);auto flight0=shadowDraws,flight1=shadowDraws;
 for(auto &op:flight0)op.samples[0]=shadow;
 for(auto &op:flight1){op.samples[0]=shadow2;op.texture=flightTarget;}
 std::function<void()> shadowFirst,shadowSecond;FrameSample fs0,fs1;
 memset(backing->contents,71,4096);{FrameTraceScope scope(&fs0);b.submit(flight0,{},&shadowFirst);}
 memset(backing2->contents,163,4096);{FrameTraceScope scope(&fs1);b.submit(flight1,{},&shadowSecond);}
 assert(fs0.passes==(merge?1u:11u) && fs1.passes==fs0.passes);shadowFirst();shadowSecond();pixels("shadow-updated-two-flights-0");
 std::vector<uint8_t> flightPixels((uint8_t *)read->contents,(uint8_t *)read->contents+1024);
 Operation flightCopy{};flightCopy.sourceTexture=flightTarget;flightCopy.texture=dst;b.submit({flightCopy});pixels("shadow-updated-two-flights-1");assert(memcmp(flightPixels.data(),read->contents,1024));b.destroy(flightTarget);
 b.destroy(alias);b.destroy(shadow);b.destroy(shadow2);b.destroy(backing);b.destroy(backing2);
 // Distinct bindings, viewports and scissors remain per draw. Clear is applied
 // once; blending makes ordering, repeated clears and missing draws observable.
 std::vector<Operation> many(23,draw);for(unsigned i=0;i<many.size();++i){auto &op=many[i];op.load=i?1:2;op.samples[0]=i%2?other:src;op.scissor.extent.width=16-i%5;op.viewport.width=16-float(i%3);}
 run(many,1,22);assert(counter("render_merge_load")==uint64_t(merge?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD));pixels("blend-clear");assert(dst->isDefined(0));
 for(unsigned i=0;i<many.size();++i)many[i].encoderID=i+1;
 auto s=run(many,cross?1:23,cross?22:0);assert(s.crossEncoderMerges==(cross?22u:0u));pixels("many-encoders");
 if(systemTranslationPolicy().perf&1)assert(s.readBarriersSkipped>0);
 for(auto &op:many)op.encoderID=1;
 // A load first draw must preserve earlier pixels.
 for(auto &op:many)op.load=1;run(many,1,22);pixels("blend-load");
 // The final discard is observable through defined tracking, never through
 // discarded pixels (whose values Vulkan deliberately leaves unspecified).
 many.back().store=0;run(many,1,22);assert(!dst->isDefined(0));assert(counter("render_merge_store")==VK_ATTACHMENT_STORE_OP_DONT_CARE);
 bool refused=false;try{b.submit({many.back()});}catch(const std::runtime_error &e){refused=std::string(e.what()).find("load undefined")!=std::string::npos;}assert(refused && !b.poisoned);
 run({draw},1,0);pixels("after-discard-clear");
 // Crossing a known encoder requires the extra opt-in, preserving STORE/LOAD.
 auto a=draw,c=draw;c.load=1;c.encoderID=2;s=run({a,c},cross?1:2,cross?1:0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::encoder)]==unsigned(!cross));assert(s.crossEncoderMerges==unsigned(cross));pixels("encoders");
 c.encoderID=0;s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::encoder)]==1);
 c=draw;c.load=1;c.areaWidth=8;c.areaHeight=8;c.scissor.extent={8,8};s=run({a,c},(systemTranslationPolicy().perf&4)?1:2,(systemTranslationPolicy().perf&4)?1:0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::area)]==unsigned(!(systemTranslationPolicy().perf&4)));pixels("area-split");
 // Partial render areas must preserve uploaded pixels outside their rectangle.
 put(dst,173);many.assign(9,c);for(unsigned i=0;i<many.size();++i)many[i].load=i?1:2;run(many,1,8);pixels("partial");
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)if(x>=8 || y>=8)for(unsigned q=0;q<4;++q)assert(((uint8_t *)read->contents)[(y*16+x)*4+q]==173);
 // Empty partial clears and transfer operations remain outside merged passes.
 Operation empty=draw;empty.pipeline=nullptr;empty.clearOnly=true;empty.areaWidth=8;empty.areaHeight=8;c=draw;c.load=1;s=run({draw,empty,c},3,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("empty-partial");
 Operation upload{};upload.texture=other;upload.upload=true;upload.src=bytes;c.samples[0]=other;s=run({draw,upload,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("upload");
 Operation copy{};copy.texture=other;copy.sourceTexture=src;s=run({draw,copy,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("image-copy");
 Operation mips{};mips.texture=dst;mips.generateMips=true;mips.mipCount=3;s=run({draw,mips,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("blit-mips",2);
 // A preceding draw produces a mip sampled later in the same encoder. This
 // must split even with a full GENERAL view and proven disjoint mip accesses.
 put(dst,83,0);put(dst,67,1);put(dst,51,2);a=draw;a.dstLevel=1;a.viewport={0,8,8,-8,0,1};a.scissor.extent={8,8};
 c=draw;c.load=1;c.samples[0]=dst;c.sampleBase[0]=1;c.sampleCount[0]=1;s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("mip-producer");
 c.sampleBase[0]=0;c.sampleCount[0]=3;c.dstLevel=1;c.viewport={0,8,8,-8,0,1};c.scissor.extent={8,8};c.load=1;a=draw;
 s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("general-view",1);
 if(systemTranslationPolicy().allowFeedback){
  // True same-level feedback retains the existing opt-in's undefined written
  // texels. Only compare an untouched mip, plus split and gate survival.
  c.dstLevel=0;c.viewport=draw.viewport;c.scissor=draw.scissor;s=run({draw,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("feedback-untouched",2);
 }
 // Internal clear/discard actions cannot disappear even for a malformed host
 // producer that reuses an encoder identity.
 c=draw;s=run({draw,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::actions)]==1);pixels("action-split");
 // Storage can write buffers consumed by a later draw. Preserve the old
 // inter-op barrier by isolating both draws; one fragment invocation per draw.
 auto *storage=b.compilePipeline(nullptr,"control",nullptr,"storage");auto *state=b.allocate(16);memset(state->contents,0,16);
 a=draw;a.pipeline=storage;a.buffers[1][1]={state,0,false};a.scissor.extent={1,1};c=a;c.load=1;
 s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::storage)]==1);assert(*(uint32_t *)state->contents==2);pixels("storage-barrier");b.destroy(storage);b.destroy(state);
 // Hoisted state rolls back on recording failure, including defined bits.
 many.assign(5,draw);for(unsigned i=0;i<many.size();++i)many[i].load=i?1:2;auto layouts=dst->levelLayouts;auto defined=dst->definedLevels;auto layout=dst->layout;
 b.failBeforeQueue=true;refused=false;try{b.submit(many);}catch(...){refused=true;}assert(refused && !b.poisoned && dst->definedLevels==defined && dst->levelLayouts==layouts && dst->layout==layout);run(many,1,4);
 // Use both lanes and retire with the recording thread's trace scope gone.
 std::function<void()> first,second;b.submit(many,{},&first);b.submit(many,{},&second);first();second();pixels("two-flights");
 b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(src);b.destroy(other);b.destroy(dst);b.destroy(bytes);b.destroy(read);
 puts("PASS: render merge pixels, fetch/normal blending, CPU shadows, transfer dependencies, actions, mip hazards, rollback and two flights");
}
