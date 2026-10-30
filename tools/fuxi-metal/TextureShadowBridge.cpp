/* Authored sampling/storage fixtures; no Apple shader inputs. */
#define fuxi_m2v_draw texture_shadow_fixture_draw
#define fuxi_m2v_compile texture_shadow_fixture_compile
#define fuxi_m2v_identity texture_shadow_fixture_identity
#define fuxi_m2v_product_hash texture_shadow_fixture_hash
#include "MipFeedbackBridge.cpp"
#undef fuxi_m2v_draw
#undef fuxi_m2v_compile
#undef fuxi_m2v_identity
#undef fuxi_m2v_product_hash
extern "C" int fuxi_m2v_identity(void *,const char *,void *,const char *name,const fuxi::CompileOptions *,uint8_t *out,char *,size_t){memset(out,!strcmp(name,"writer")?9:1,32);return 1;}
extern "C" int fuxi_m2v_product_hash(void *p,uint8_t *out){memset(out,((Product *)p)->variant?10:2,32);return 1;}
extern "C" int fuxi_m2v_compile(void *v,const char *vn,void *f,const char *fn,void *device,uint64_t a,uint64_t b,const fuxi::CompileOptions *options,fuxi::Translation *out,char *why,size_t cap){
 int ok=texture_shadow_fixture_compile(v,vn,f,fn,device,a,b,options,out,why,cap);
 if(ok && !strcmp(fn,"writer")){auto *p=(Product *)out->handle;p->variant=true;p->fragment=shader("CONTROL_STORAGE_FRAGMENT");out->fragment=p->fragment.data();out->fragmentCount=p->fragment.size();out->descriptorCount=2;out->descriptors[1]={1,7,16,1};}return ok;
}
extern "C" int fuxi_m2v_draw(void *p,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *why,size_t cap){
 int ok=texture_shadow_fixture_draw(p,in,out,why,cap);out->indexed=in->indexed;out->indexOffset=in->indexOffset;out->descriptors[0].format=in->formats[0]==1?9:16;
 if(((Product *)p)->variant){out->descriptorCount=2;out->descriptors[1]={1,7,16,1};out->descriptors[1].range=4096;}return ok;
}
