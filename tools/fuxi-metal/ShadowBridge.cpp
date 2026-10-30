/* Authored uniform-window fixture; no external or Apple shader binaries. */
#define fuxi_m2v_compile shadow_fixture_compile
#define fuxi_m2v_draw shadow_fixture_draw
#include "MipFeedbackBridge.cpp"
#undef fuxi_m2v_compile
#undef fuxi_m2v_draw
static fuxi::Descriptor uniform(){fuxi::Descriptor d{};d.type=6;d.stages=16;d.range=16;return d;}
extern "C" int fuxi_m2v_compile(void *v,const char *vn,void *f,const char *fn,void *c,uint64_t a,uint64_t b,const fuxi::CompileOptions *options,fuxi::Translation *out,char *why,size_t n){
 int ok=shadow_fixture_compile(v,vn,f,fn,c,a,b,options,out,why,n);out->rangeCount=out->pushSize=out->pushStages=0;out->bindingCount=1;out->bindings[0]={0,0,8};out->attributeCount=1;out->attributes[0]={0,0,103,0};out->descriptors[0]=uniform();return ok;
}
extern "C" int fuxi_m2v_draw(void *,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *,size_t){
 *out={};out->count=in->count;out->first=in->indexed?0:in->start;out->indexed=in->indexed;out->indexOffset=in->indexOffset;out->vertexCount=1;out->vertexBindings[0]=0;out->vertexSlots[0]=0;out->vertexOffsets[0]=in->buffers[0][0].offset;out->descriptorCount=1;out->descriptors[0]=uniform();out->descriptors[0].offset=in->buffers[1][0].offset;return 1;
}
#ifndef SHADOW_OLD_ADAPTER
extern "C" int fuxi_m2v_shadow_windows_v1(void *,fuxi::ShadowWindow *out){for(unsigned i=0;i<2*fuxi::resourceSlots;++i)out[i]={2,0,0,0};out[fuxi::resourceSlots]={1,0,0,16};return 1;}
#endif
