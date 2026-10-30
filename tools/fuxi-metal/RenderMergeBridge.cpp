/* Authored host adapter extends the mip fixture with blend/storage controls. */
#include <vulkan/vulkan.h>
#include <unordered_map>
#define fuxi_m2v_compile mip_fixture_compile
#define fuxi_m2v_draw mip_fixture_draw
#define fuxi_m2v_identity mip_fixture_identity
#define fuxi_m2v_product_hash mip_fixture_product_hash
#define fuxi_m2v_free mip_fixture_free
#include "MipFeedbackBridge.cpp"
#undef fuxi_m2v_compile
#undef fuxi_m2v_draw
#undef fuxi_m2v_identity
#undef fuxi_m2v_product_hash
#undef fuxi_m2v_free
static uint64_t planCalls;
extern "C" uint64_t render_merge_plan_calls(){return planCalls;}
static fuxi::Descriptor storageDescriptor(){fuxi::Descriptor d{};d.binding=1;d.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;d.stages=16;d.slot=1;d.range=16;return d;}
static fuxi::Descriptor fetchDescriptor(){fuxi::Descriptor d{};d.binding=1;d.type=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;d.stages=16;d.format=VK_FORMAT_B8G8R8A8_UNORM;return d;}
static std::unordered_map<void *,bool> fetchProducts;
extern "C" void fuxi_m2v_free(void *p){fetchProducts.erase(p);mip_fixture_free(p);}
// Distinct shader products keep distinct cache identities.
extern "C" int fuxi_m2v_identity(void *,const char *,void *,const char *name,const fuxi::CompileOptions *options,uint8_t *out,char *,size_t){memset(out,!strcmp(name,"storage")?7:!strcmp(name,"fetch")?9:1,32);out[31]=options->orderedColorAccess;return 1;}
extern "C" int fuxi_m2v_product_hash(void *p,uint8_t *out){auto *product=(Product *)p;memset(out,product->multi?8:fetchProducts[p]?10:2,32);return 1;}
extern "C" int fuxi_m2v_compile(void *v,const char *vn,void *f,const char *fn,void *c,uint64_t a,uint64_t b,const fuxi::CompileOptions *options,fuxi::Translation *out,char *why,size_t n){
 int result=mip_fixture_compile(v,vn,f,"control",c,a,b,options,out,why,n);
 fetchProducts[out->handle]=!strcmp(fn,"fetch");
 // Small alpha keeps ordering and the initial clear visible after many draws.
 out->colors[0].blend=1;auto *factors=out->colors[0].factors;
 factors[0]=factors[3]=VK_BLEND_FACTOR_SRC_ALPHA;factors[1]=factors[4]=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
 factors[2]=factors[5]=VK_BLEND_OP_ADD;
 if(!strcmp(fn,"storage")){auto *p=(Product *)out->handle;p->multi=true;p->fragment=shader("CONTROL_FRAGMENT_STORAGE");out->fragment=p->fragment.data();out->fragmentCount=p->fragment.size();out->descriptorCount=2;out->descriptors[1]=storageDescriptor();}
 if(!strcmp(fn,"fetch")){auto *p=(Product *)out->handle;p->fragment=shader("CONTROL_FRAGMENT_FETCH");out->fragment=p->fragment.data();out->fragmentCount=p->fragment.size();out->fetchMask=1;out->colors[0].blend=0;out->descriptorCount=2;out->descriptors[1]=fetchDescriptor();}
 return result;
}
extern "C" int fuxi_m2v_draw(void *product,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *why,size_t n){
 ++planCalls;auto *p=(Product *)product;int result=mip_fixture_draw(product,in,out,why,n);if(p->multi)out->descriptors[1]=storageDescriptor();if(fetchProducts[product]){out->descriptorCount=2;out->descriptors[1]=fetchDescriptor();}return result;
}
