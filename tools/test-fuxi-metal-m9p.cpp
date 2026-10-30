/* Production Vulkan 3D sampled textures: allocation, view type, strided uploads, copies and refusals. No translator or device. */
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <cstdio>
template<class F> static void refuses(F body){bool failed=false;try{body();}catch(const std::runtime_error&){failed=true;}assert(failed);}
static fuxi::Operation readOf(fuxi::Texture *t,fuxi::Buffer *dst){fuxi::Operation r{};r.texture=t;r.dst=dst;return r;}
int main(){
 fuxi::Backend b;auto baseline=b.resourceBytes();
 assert(fuxi::textureShapeAccepted(7,16,1,16,1) && fuxi::textureShapeAccepted(7,1,3,4,1) && !fuxi::textureShapeAccepted(7,16,5,16,1) && !fuxi::textureShapeAccepted(7,16,4,16,1) && !fuxi::textureShapeAccepted(7,16,1,16,2) && !fuxi::textureShapeAccepted(7,16,1,0,1));
 assert(!fuxi::textureShapeAccepted(2,16,1,2,1) && fuxi::textureShapeAccepted(2,16,5,1,1) && fuxi::textureShapeAccepted(0,1,1,1,3));
 assert(fuxi::Backend::viewTypeFor(7)==VK_IMAGE_VIEW_TYPE_3D && fuxi::Backend::viewTypeFor(0)==VK_IMAGE_VIEW_TYPE_1D && fuxi::Backend::viewTypeFor(2)==VK_IMAGE_VIEW_TYPE_2D && fuxi::Backend::imageTypeFor(7)==VK_IMAGE_TYPE_3D);
 // The CoreAnimation lookup table: RGBA8Unorm 16x16x16, ShaderRead.
 auto *t=b.allocateTexture(70,1,16,16,1,7,16);
 assert(t->textureType==7 && t->depth==16 && t->levelDepth(0)==16 && t->levelDepth(1)==0 && t->image && t->view && (t->usage&VK_IMAGE_USAGE_SAMPLED_BIT) && !(t->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
 constexpr size_t slice=16*16*4,all=slice*16;
 auto *up=b.allocate(all*2),*out=b.allocate(all*2);auto *in=(uint8_t *)up->contents,*result=(uint8_t *)out->contents;
 std::vector<uint8_t> pattern(all);
 for(size_t i=0;i<all;++i)pattern[i]=uint8_t((i*31+i/slice*7)%253);
 memcpy(in,pattern.data(),all);
 fuxi::Operation upload{};upload.src=up;upload.texture=t;upload.upload=true;
 b.submit({upload,readOf(t,out)});assert(t->isDefined(0) && !memcmp(result,pattern.data(),all));
 // Padded rows and slices (row 80 bytes, 20 rows per image) on both transfer directions.
 constexpr size_t row=80,image=row*20;std::vector<uint8_t> padded(image*16+64,0xa7);
 for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)memcpy(padded.data()+16+z*image+y*row,pattern.data()+z*slice+y*64,64);
 memcpy(in,padded.data(),padded.size());
 upload.from=16;upload.rowBytes=row;upload.imageBytes=image;memset(result,0,all*2);
 fuxi::Operation padRead=readOf(t,out);padRead.to=32;padRead.rowBytes=row;padRead.imageBytes=image;
 b.submit({upload,padRead});
 for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y){assert(!memcmp(result+32+z*image+y*row,pattern.data()+z*slice+y*64,64));for(unsigned x=64;x<row;++x)assert(result[32+z*image+y*row+x]==0);}
 // A partial box into a fresh texture zero-fills the rest; slice order is preserved.
 auto *box=b.allocateTexture(70,1,16,16,1,7,16);
 uint8_t boxBytes[4*2*3*4];for(unsigned i=0;i<sizeof(boxBytes);++i)boxBytes[i]=uint8_t(200+i);memcpy(in,boxBytes,sizeof(boxBytes));
 fuxi::Operation part{};part.src=up;part.texture=box;part.upload=true;part.rowBytes=16;part.width=4;part.height=2;part.depth=3;part.dstX=5;part.dstY=6;part.dstZ=9;part.imageBytes=32;
 b.submit({part,readOf(box,out)});
 for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
  bool inside=z>=9&&z<12&&y>=6&&y<8&&x>=5&&x<9;auto *p=result+z*slice+y*64+x*4;
  for(unsigned c=0;c<4;++c)assert(p[c]==(inside?boxBytes[((z-9)*2+(y-6))*16+(x-5)*4+c]:0));
 }
 // 3D to 3D copies snapshot overlapping z ranges and respect z offsets.
 memcpy(in,pattern.data(),all);
 fuxi::Operation c{};c.sourceTexture=t;c.texture=t;c.srcZ=0;c.dstZ=1;c.depth=8;
 upload.from=0;upload.rowBytes=0;upload.imageBytes=0;
 b.submit({upload,c,readOf(t,out)});
 std::vector<uint8_t> expected(pattern);memcpy(expected.data()+slice,pattern.data(),8*slice);
 assert(!memcmp(result,expected.data(),all));
 auto *other=b.allocateTexture(70,1,16,16,1,7,16);fuxi::Operation whole{};whole.sourceTexture=t;whole.texture=other;
 memset(result,0,all);b.submit({whole,readOf(other,out)});assert(!memcmp(result,expected.data(),all));
 // A one-slice box read at z=5.
 fuxi::Operation one=readOf(t,out);one.srcZ=5;one.depth=1;one.to=8;memset(result,0,all);b.submit({one});assert(!memcmp(result+8,expected.data()+5*slice,slice));
 // Unsupported 3D uses refuse cleanly and leave the device usable.
 auto charged=b.resourceBytes();
 refuses([&]{b.allocateTexture(70,5,16,16,1,7,16);});refuses([&]{b.allocateTexture(70,4,16,16,1,7,16);});
 refuses([&]{b.allocateTexture(70,1,16,16,2,7,16);});refuses([&]{b.allocateTexture(70,1,16,16,1,7,0);});
 refuses([&]{b.allocateTexture(70,1,16,16,1,7,1u<<20);});refuses([&]{b.allocateTexture(70,1,16,16,1,2,4);});refuses([&]{b.allocateTexture(70,1,16,16,1,0,4);});
 assert(b.resourceBytes()==charged);
 fuxi::Operation bad=c;bad.dstZ=9;refuses([&]{b.submit({bad});});
 bad=c;bad.srcZ=9;refuses([&]{b.submit({bad});});
 bad=one;bad.srcZ=16;refuses([&]{b.submit({bad});});
 bad=one;bad.depth=17;refuses([&]{b.submit({bad});});
 bad=one;bad.imageBytes=row*3;bad.rowBytes=row;refuses([&]{b.submit({bad});});
 bad=one;bad.imageBytes=row*16+1;bad.rowBytes=row;refuses([&]{b.submit({bad});});
 auto *small=b.allocate(slice);bad=one;bad.depth=16;bad.srcZ=0;bad.to=0;bad.dst=small;refuses([&]{b.submit({bad});});b.destroy(small);
 fuxi::Operation mip{};mip.texture=t;mip.generateMips=true;mip.mipCount=1;refuses([&]{b.submit({mip});});
 fuxi::Operation clear{};clear.texture=t;clear.clearOnly=true;refuses([&]{b.submit({clear});});
 auto *flat=b.allocateTexture(70,1,16,16);
 bad=c;bad.sourceTexture=flat;refuses([&]{b.submit({bad});});
 fuxi::Operation flatDepth=readOf(flat,out);flatDepth.depth=2;refuses([&]{b.submit({flatDepth});});
 fuxi::Operation flatZ=readOf(flat,out);flatZ.srcZ=1;refuses([&]{b.submit({flatZ});});
 fuxi::Operation flatImage=readOf(flat,out);flatImage.rowBytes=64;flatImage.imageBytes=1024;refuses([&]{b.submit({flatImage});});
 assert(!b.poisoned);memset(result,0,all);b.submit({readOf(t,out)});assert(!memcmp(result,expected.data(),all));
 b.destroy(flat);b.destroy(other);b.destroy(box);b.destroy(t);b.destroy(up);b.destroy(out);b.trimMemory();assert(b.resourceBytes()==baseline);
 puts("PASS: M9p 3D sampled textures: shape/limits, VK 3D image+view, tight/padded slice transfers, partial boxes, overlapping z copies, render/mip/storage/2D-mixing refusals");
}
