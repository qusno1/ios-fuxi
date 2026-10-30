/* Production Vulkan transfers only: no translator, Apple inputs or device. */
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <cstdio>
template<class F> static void refuses(F body){bool failed=false;try{body();}catch(const std::runtime_error&){failed=true;}assert(failed);}
static fuxi::Operation copy(fuxi::Texture *s,fuxi::Texture *d,uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,uint32_t w,uint32_t h,uint32_t sl=0,uint32_t dl=0){
 fuxi::Operation o{};o.sourceTexture=s;o.texture=d;o.srcX=sx;o.srcY=sy;o.dstX=dx;o.dstY=dy;o.width=w;o.height=h;o.srcLevel=sl;o.dstLevel=dl;return o;
}
int main(){
 fuxi::Backend b;auto baseline=b.resourceBytes();
 // CA linear textures: 1x1 BGRA in a 64-byte row is 4 bytes; zero means "not given".
 assert(fuxi::linearImageFits(4,64,1,1,80) && !fuxi::linearImageFits(3,64,1,1,80) && fuxi::linearImageFits(0,64,1,1,80));
 assert(fuxi::linearImageFits(68,64,1,2,80) && !fuxi::linearImageFits(67,64,1,2,80));
 assert(b.pixelFormat(43)==VK_FORMAT_A1R5G5B5_UNORM_PACK16 && b.texelBytes(43)==2);
 for(auto f:{23u,25u,55u,105u}){
  VkFormat expected=f==23?VK_FORMAT_R16_UINT:f==25?VK_FORMAT_R16_SFLOAT:f==55?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R32G32_SFLOAT;
  assert(b.pixelFormat(f)==expected && fuxi::textureFormatAccepted(f));
  auto *t=b.allocateTexture(f,1,16,1,5,0);assert(t->textureType==0 && t->height==1 && t->levelWidth(4)==1);
  auto *up=b.allocate(256),*out=b.allocate(256);auto pixel=b.texelBytes(f);
  for(size_t i=0;i<256;++i)((uint8_t *)up->contents)[i]=uint8_t(i*7);
  fuxi::Operation upload{};upload.src=up;upload.texture=t;upload.upload=true;upload.width=16;upload.height=1;
  fuxi::Operation read{};read.texture=t;read.dst=out;read.width=16;read.height=1;
  b.submit({upload,copy(t,t,0,0,2,0,12,1),read});
  assert(!memcmp((uint8_t *)out->contents+2*pixel,up->contents,12*pixel));
  b.submit({copy(t,t,0,0,0,0,8,1,0,1)});assert(t->isDefined(1));
  read.srcLevel=1;read.width=8;b.submit({read});assert(!memcmp(out->contents,up->contents,2*pixel));
  if(!t->canGenerate){fuxi::Operation mip{};mip.texture=t;mip.generateMips=true;mip.mipCount=2;refuses([&]{b.submit({mip});});assert(!b.poisoned);}
  b.destroy(up);b.destroy(out);b.destroy(t);
 }
 for(auto f:{550u,554u,552u,0u}){assert(!fuxi::textureFormatAccepted(f));refuses([&]{b.allocateTexture(f,5,8,8);});}
 refuses([&]{b.allocateTexture(55,1,16,2,1,0);});refuses([&]{b.allocateTexture(55,5,16,1,1,0);});
 assert(!fuxi::textureShapeAccepted(1,1,1) && !fuxi::textureShapeAccepted(0,2,1));
 auto *t=b.allocateTexture(80,5,8,6,3),*other=b.allocateTexture(80,5,8,6,3);
 auto *up=b.allocate(512),*out=b.allocate(512);auto *bytes=(uint8_t *)up->contents;auto *result=(uint8_t *)out->contents;
 for(size_t i=0;i<512;++i)bytes[i]=uint8_t((i*13)%251);
 fuxi::Operation upload{};upload.src=up;upload.texture=t;upload.upload=true;upload.from=16;upload.rowBytes=40;upload.width=8;upload.height=6;
 fuxi::Operation read{};read.texture=t;read.dst=out;read.to=16;read.rowBytes=40;read.width=8;read.height=6;
 std::vector<uint8_t> expected(192),snapshot;
 auto reset=[&]{b.submit({upload});for(unsigned y=0;y<6;++y)memcpy(expected.data()+y*32,bytes+16+y*40,32);};
 auto check=[&]{memset(result,0xa7,512);b.submit({read});for(unsigned y=0;y<6;++y){assert(!memcmp(result+16+y*40,expected.data()+y*32,32));for(unsigned x=32;x<40;++x)assert(result[16+y*40+x]==0xa7);}assert(result[15]==0xa7 && result[256]==0xa7);};
 // Snapshot semantics must hold across rows, in both overlap directions and
 // for disjoint or identical regions. All operations share one submission.
 for(auto region: {std::array<unsigned,6>{0,0,1,1,6,4},{1,1,0,0,6,4},{0,0,4,4,3,2},{0,0,0,0,8,6}}){
  reset();auto sx=region[0],sy=region[1],dx=region[2],dy=region[3],w=region[4],h=region[5];snapshot=expected;
  for(unsigned y=0;y<h;++y)memcpy(expected.data()+((dy+y)*8+dx)*4,snapshot.data()+((sy+y)*8+sx)*4,w*4);
  auto charged=b.resourceBytes();b.submit({copy(t,t,sx,sy,dx,dy,w,h)});assert(b.resourceBytes()==charged);check();
 }
 reset();b.submit({copy(t,other,0,0,0,0,8,6),copy(other,t,2,1,0,0,4,3,0,1)});
 assert(t->isDefined(1));read.srcLevel=1;read.width=4;read.height=3;read.rowBytes=24;read.to=4;b.submit({read});
 for(unsigned y=0;y<3;++y)assert(!memcmp(result+4+y*24,bytes+16+(y+1)*40+8,16));
 // Invalid requests and pre-submit failures leave data/layout/budget reusable.
 b.trimMemory();auto charged=b.resourceBytes();auto bad=copy(t,t,7,0,0,0,2,1);refuses([&]{b.submit({bad});});
 bad=copy(t,t,0,0,0,0,1,1,3,0);refuses([&]{b.submit({bad});});
 auto *different=b.allocateTexture(70,5,8,6);refuses([&]{b.submit({copy(t,different,0,0,0,0,8,6)});});b.destroy(different);
 auto *undefined=b.allocateTexture(80,5,8,6);refuses([&]{b.submit({copy(undefined,t,0,0,0,0,8,6)});});b.destroy(undefined);b.trimMemory();
 b.failBeforeQueue=true;refuses([&]{b.submit({copy(t,t,0,0,1,1,4,3)});});assert(!b.poisoned && b.resourceBytes()==charged);
 // Mip generation, fill and the existing transfer commands compose in order.
 fuxi::Operation fill{};fill.dst=up;fill.to=16;fill.size=256;fill.value=0x5c;
 fuxi::Operation mip{};mip.texture=t;mip.generateMips=true;mip.mipCount=3;
 b.submit({fill,upload,mip});assert(t->isDefined(2));
 // BGR5A1 attachment clear verifies the packed bit order, beyond a byte copy.
 auto *packed=b.allocateTexture(43,5,8,2);fuxi::Operation clear{};clear.texture=packed;clear.clearOnly=true;clear.clear[0]=1;clear.clear[3]=1;
 fuxi::Operation packedRead{};packedRead.texture=packed;packedRead.dst=out;
 b.submit({clear,packedRead});for(unsigned i=0;i<16;++i)assert(((uint16_t *)out->contents)[i]==0xfc00);
 b.destroy(packed);
 // Distinct images imported from the same host allocation also need a snapshot.
 if(b.supportsHostImport()){
  void *host=nullptr;constexpr size_t length=65536;assert(!posix_memalign(&host,length,length));memset(host,0,length);
  auto *a=b.importTexture(host,length,64,16,8),*alias=b.importTexture(host,length,64,16,8);
  for(unsigned i=0;i<512;++i)((uint8_t *)host)[i]=uint8_t(i%251);
  std::vector<uint8_t> saved((uint8_t *)host,(uint8_t *)host+512);
  b.submit({copy(a,alias,0,0,1,1,14,6)});
  for(unsigned y=0;y<6;++y)assert(!memcmp((uint8_t *)host+(y+1)*64+4,saved.data()+y*64,56));
  b.destroy(a);b.destroy(alias);free(host);
 }
 b.destroy(t);b.destroy(other);b.destroy(up);b.destroy(out);b.trimMemory();assert(b.resourceBytes()==baseline);
 puts("PASS: M9n formats/packed channels, native 1D/mips, overlapping/disjoint/self/import aliases, padded buffer transfers, fill/generation, bounds/format/undefined refusal, rollback/budget, tight linear image size");
}
