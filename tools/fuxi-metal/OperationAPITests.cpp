/* Portable compile/runtime guard for the Mac frontend's backend model API. */
#define FUXI_MTL_HOST_TEST
#include "TestBackend.hpp"
int main(){
 fuxi::Buffer buffer{};buffer.length=65536;fuxi::BufferBind whole{&buffer,16,false};assert(whole.length()==65536);
 fuxi::BufferBind slice{&buffer,256,true,293};assert(slice.length()-slice.offset==37);
 auto saved=slice;slice.end=512;assert(saved.length()==293 && slice.length()==512 && sizeof(fuxi::BufferBind)==24);
 fuxi::Operation op{};assert(op.encoderID==0);
 op.encoderID=77;op.colorCount=8;op.colorMask=130;
 fuxi::Texture target{};fuxi::ColorTarget color{&target,2,1,0,{.1f,.2f,.3f,1}};
 op.setColor(1,color);op.setColor(7,color);
 auto copy=op;assert(copy.encoderID==77 && copy.color(0).texture==nullptr);
 for(unsigned i:{1u,7u}){auto c=copy.color(i);assert(c.texture==&target && c.level==2 && c.load==1 && c.store==0 && c.clear[3]==1);}
 puts("PASS: Mac model Operation encoder identity, sparse attachment API and copies");
}
