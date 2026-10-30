/* Pinned external planner differential stream; original AIR on lavapipe. */
#include "Backend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <random>
int main(int argc,char **argv){
 assert(argc==2);using namespace fuxi;Backend b;auto &tr=Translator::get();assert(tr.draw_plan);
 std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(file),{}};char why[1024]{};
 void *lib=tr.open(air.data(),air.size(),why,sizeof(why));assert(lib);CompileOptions options{};
 options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *a=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 options.blend=1;options.dstRGB=options.dstAlpha=5;auto *other=b.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 DrawPlanCache<Pipeline> cache;std::mt19937 random(0xCA2026);uint64_t hits=0,misses=0,accepted=0,rejected=0,shapePlans=0,clipped=0;bool residual=false;
 auto compare=[&](Pipeline *p,const DrawInput &in){
  DrawOutput full{},out{};memset(&full,0xa5,sizeof(full));memset(&out,0xa5,sizeof(out));DrawRebase rules{};bool want=tr.draw(p->translation,&in,&full,why,sizeof(why));Pipeline *resolved=nullptr;
  bool hit=cache.find(p,in,resolved,out),got=hit;if(hit)++hits;else {++misses;got=tr.planDraw(p->translation,&in,&out,&rules,why,sizeof(why));if(got){cache.insert(p,in,p,out,rules);shapePlans+=rules.enabled;for(auto r:rules.buffers)residual|=r.active && in.buffers[r.stage][r.slot].offset%r.alignment;}}
  if(want!=got || memcmp(&full,&out,sizeof(out))){fprintf(stderr,"differential failure draw=%llu hit=%u want=%u got=%u: %s\n",(unsigned long long)(hits+misses),hit,want,got,why);abort();}
  if(got){assert(!hit || resolved==p);++accepted;}else ++rejected;
 };
 // Warm both products with normal buffers and inline copies before the mixed stream.
 for(unsigned n=0;n<24000;++n){
  DrawInput in{};in.count=1+random()%120;in.start=random()%48;in.indexed=(n/2)%2;in.strip=(n/4)%2;if(in.indexed)in.start=0;
  in.indexLength=32768+random()%4096;in.indexOffset=(random()%4096)*2;
  in.formats[0]=80;in.filters[0]=1;in.lodMax[0]=1000;
  in.buffers[0][0]={65536+random()%4096,(random()%128)*256,0};
  bool bytes=(n/8)%2;in.buffers[0][2]=bytes?BoundBuffer{16+random()%64,0,1}:BoundBuffer{65536+random()%4096,(random()%128)*256,0};
  in.buffers[1][1]=bytes?BoundBuffer{16+random()%64,0,1}:BoundBuffer{65536+random()%4096,(random()%128)*256,0};
  auto *p=n%2?a:other;compare(p,in);
  if(n%7==0){auto bad=in;switch((n/7)%18){
   case 0:bad.count=0;break;case 1:bad.count=65537;break;case 2:bad.start=65536;break;
   case 3:bad.indexed=1;bad.start=1;break;case 4:bad.indexed=1;bad.start=0;bad.indexOffset|=1;break;
   case 5:bad.indexed=1;bad.start=0;bad.indexOffset=bad.indexLength;break;case 6:bad.indexed=1;bad.start=0;bad.indexOffset=UINT64_MAX-1;break;
   case 7:bad.buffers[0][0].offset=bad.buffers[0][0].length;break;case 8:bad.buffers[0][0].length=16777217;break;
   case 9:bad.buffers[0][2]={4097,0,1};break;case 10:bad.buffers[1][1]={64,1,1};break;
   case 11:bad.buffers[0][0].offset+=1;break;case 12:bad.buffers[0][2]={65536,257,0};break;
   case 13:bad.buffers[1][1]={65536,258,0};break;case 14:bad.buffers[1][1]={65536,65535,0};++clipped;break;
   case 15:bad.buffers[0][15]={64,64,0};break;case 16:bad.buffers[0][2]={};break;case 17:bad.buffers[0][2]={1,0,1};++clipped;break;
  }compare(p,bad);}
 }
 DrawInput pulledInput{};pulledInput.count=4;pulledInput.formats[0]=80;pulledInput.filters[0]=1;pulledInput.lodMax[0]=1000;pulledInput.buffers[0][0]={65536,1,0};pulledInput.buffers[0][2]={65536,256,0};pulledInput.buffers[1][1]={65536,512,0};
 Translation variant{};assert(tr.variant(a->translation,&pulledInput,&variant,why,sizeof(why)));auto *pulled=b.createPipeline(variant,a->options);
 for(unsigned n=0;n<2000;++n){auto in=pulledInput;in.count=1+random()%120;in.start=random()%48;in.buffers[0][0].offset=(random()%128)*256+1;in.buffers[0][2].offset=(random()%128)*256;in.buffers[1][1].offset=(random()%128)*256;compare(pulled,in);}
 // Exact residues, padding/class boundaries, absent-slot garbage and every ABI shape field.
 for(uint64_t length:{1,15,16,17,255,256,257,4095,4096,4097,65535,65536,65537,16777216})for(unsigned offset:{0,1,2,3,4,15,16,17,255,256,257}){
  DrawInput in{};in.count=4;in.formats[0]=80;in.filters[0]=1;in.lodMax[0]=1000;in.buffers[0][0]={65536,256,0};in.buffers[0][2]={length,offset,0};in.buffers[1][1]={length,offset,0};compare(a,in);
 }
 DrawInput seed{};seed.count=4;seed.formats[0]=80;seed.filters[0]=1;seed.lodMax[0]=1000;seed.buffers[0][0]={65536,0,0};seed.buffers[0][2]={16,0,1};seed.buffers[1][1]={16,0,1};compare(a,seed);
 for(unsigned slot=0;slot<resourceSlots;++slot){auto in=seed;in.formats[slot]=81;compare(a,in);in=seed;in.filters[slot]=31;compare(a,in);in=seed;in.lodMin[slot]=-0.0f;compare(a,in);in=seed;in.lodMax[slot]=12;compare(a,in);
  for(unsigned stage=0;stage<2;++stage){in=seed;in.buffers[stage][slot]={64,4,0};compare(a,in);in=seed;in.buffers[stage][slot]={64,0,1};compare(a,in);in=seed;in.buffers[stage][slot]={0,UINT64_MAX,7};compare(a,in);}
 }

 assert(hits>20000 && rejected>1500 && shapePlans && clipped && residual);cache.invalidate(a);Pipeline *resolved=nullptr;DrawOutput out{};assert(!cache.find(a,seed,resolved,out));
 printf("PASS: external differential draws=%llu accepted=%llu rejected=%llu hits=%llu misses=%llu hit_rate=%.2f%% shape_plans=%llu; byte-identical outputs and acceptance\n",(unsigned long long)(hits+misses),(unsigned long long)accepted,(unsigned long long)rejected,(unsigned long long)hits,(unsigned long long)misses,100.0*hits/(hits+misses),(unsigned long long)shapePlans);
 b.destroy(pulled);b.destroy(a);b.destroy(other);b.close();tr.close(lib);
}
