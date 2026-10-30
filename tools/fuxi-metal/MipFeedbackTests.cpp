/* Real production backend, authored GLSL adapter, software Vulkan; no Apple inputs. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 assert(fuxi::backboardGate.admit("12345678-1234-1234-1234-123456789abc",open(getenv("CONTROL_GATE"),O_RDONLY|O_DIRECTORY|O_CLOEXEC)));
 fuxi::Backend b;b.reuseDrawPlans=false; // Fixture push bytes vary through CONTROL_LOD, outside DrawInput.
 auto *t=b.allocateTexture(80,5,704,96,10);auto *upload=b.allocate(704*96*4),*read=b.allocate(704*96*4);
 memset(upload->contents,91,upload->length);fuxi::Operation put{};put.texture=t;put.src=upload;put.upload=true;b.submit({put});
 auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 fuxi::Operation op{};op.texture=t;op.pipeline=p;op.samples[0]=t;op.samplers[0]=sam;op.vertices=6;op.dstLevel=1;op.areaWidth=352;op.areaHeight=48;op.viewport={0,48,352,-48,0,1};op.scissor={{0,0},{352,48}};op.sampleCount[0]=10;op.load=2;op.store=1;
 setenv("CONTROL_LOD","0",1);b.submit({op});assert(t->isDefined(1) && !t->isDefined(2));
 assert(b.sampleViewUses.size()==1);auto view=b.sampleViewUses[0];assert(view.base==0 && view.count==10 && view.layout==VK_IMAGE_LAYOUT_GENERAL);
 // The entire original descriptor view is GENERAL, with individually tracked
 // mips; the attachment has no implicit transition to COLOR_ATTACHMENT_OPTIMAL.
 assert(b.imageTransitions.size()==11);assert(b.imageTransitions[0].level==0 && b.imageTransitions[0].before==VK_IMAGE_LAYOUT_GENERAL);
 assert(b.imageTransitions[1].level==1 && b.imageTransitions[1].before==VK_IMAGE_LAYOUT_UNDEFINED);
 for(auto transition:b.imageTransitions)assert(transition.texture==t && transition.after==VK_IMAGE_LAYOUT_GENERAL);
 auto pixels=[&](uint32_t l,uint8_t expected){fuxi::Operation get{};get.texture=t;get.srcLevel=l;get.dst=read;b.submit({get});for(size_t i=0;i<t->levelWidth(l)*t->levelHeight(l)*4;++i)if(((uint8_t *)read->contents)[i]!=expected){fprintf(stderr,"pixel level=%u byte=%zu got=%u expected=%u\n",l,i,((uint8_t *)read->contents)[i],expected);abort();}};
 pixels(1,91);
 // True feedback: same view, same sampler, different draw-time LOD. Default
 // rejects before recording/submission; opt-in drops only this operation.
 setenv("CONTROL_LOD","1",1);auto before=b.submits.load();auto skipped=fuxi::skippedSamplingDraws.load();bool rejected=false;
 try{b.submit({op});}catch(const std::runtime_error &e){rejected=true;assert(std::string(e.what()).find("sampling reject attachmentFeedback")!=std::string::npos);}
 bool allow=fuxi::systemTranslationPolicy().allowFeedback,skip=!allow && fuxi::systemTranslationPolicy().skipSampling;
 assert(rejected==!(skip||allow) && !b.poisoned && !fuxi::backboardGate.blocked());
 assert(fuxi::skippedSamplingDraws==skipped+(skip?1:0));if(!skip && !allow)assert(b.submits==before);
 // Feedback opt-in: the draw runs (the written texel's read is undefined) and the device stays live.
 if(allow)assert(fuxi::allowedFeedbackDraws>=1 && b.submits==before+1);
 if(skip){std::vector<fuxi::Operation> refused(20,op);b.submit(refused);assert(fuxi::skippedSamplingDraws==skipped+21 && !fuxi::backboardGate.blocked());}
 if(!allow)pixels(1,skip?0:91);
 setenv("CONTROL_LOD","0",1);b.submit({op});setenv("CONTROL_LOD","1",1);
 // Upsample: original view still starts at zero. LOD 1 reads the preceding
 // downsample result while writing zero; no rebasing of explicit LOD is needed.
 op.dstLevel=0;op.areaWidth=704;op.areaHeight=96;op.viewport={0,96,704,-96,0,1};op.scissor={{0,0},{704,96}};b.submit({op});pixels(0,91);
 // A rebased Metal view, relative LOD zero -> absolute level one, writing zero.
 // Descriptor and attachment layouts are independent at these disjoint levels.
 op.sampleBase[0]=1;op.sampleCount[0]=1;setenv("CONTROL_LOD","0",1);b.submit({op});
 view=b.sampleViewUses[0];assert(view.base==1 && view.count==1 && view.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 assert(t->layout==VK_IMAGE_LAYOUT_GENERAL && t->levelLayouts[1]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 assert(b.imageTransitions.size()==2 && b.imageTransitions[0].level==1 && b.imageTransitions[1].level==0);
 // Recording failure restores each subresource, including mixed layouts.
 auto layouts=t->levelLayouts;auto layout=t->layout;op.sampleBase[0]=0;op.sampleCount[0]=10;op.dstLevel=1;op.areaWidth=352;op.areaHeight=48;op.viewport={0,48,352,-48,0,1};op.scissor={{0,0},{352,48}};
 b.failBeforeQueue=true;rejected=false;try{b.submit({op});}catch(...){rejected=true;}assert(rejected && !b.poisoned && t->layout==layout && t->levelLayouts==layouts);b.submit({op});pixels(1,91);
 // Two views of the same image must agree on the layout of shared mips.
 auto *multi=b.compilePipeline(nullptr,"control",nullptr,"multi");op.pipeline=multi;op.samples[1]=t;op.samplers[1]=sam;op.sampleCount[1]=1;b.submit({op});
 assert(b.sampleViewUses.size()==2 && b.sampleViewUses[0].layout==VK_IMAGE_LAYOUT_GENERAL && b.sampleViewUses[1].count==1 && b.sampleViewUses[1].layout==VK_IMAGE_LAYOUT_GENERAL);pixels(1,91);
 b.destroy(multi);
 b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(t);b.destroy(upload);b.destroy(read);
 puts("PASS: 704x96/10 level-1 downsample, same-level refusal/skip, full-view upsample, rebased views, pixels and per-mip barrier/rollback");
}
