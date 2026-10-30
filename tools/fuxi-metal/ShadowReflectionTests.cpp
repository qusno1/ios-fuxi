/* Actual public pipeline access proof from our hand-authored AIR control. */
#include "Backend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc,char **argv){
 using namespace fuxi;assert(argc==3);std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(file),{}};
 auto &tr=Translator::get();assert(tr.shadow_windows);char why[1024]{};auto *lib=tr.open(air.data(),air.size(),why,sizeof(why));assert(lib);
 Backend backend;CompileOptions options{};options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
 options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
 auto *pipeline=backend.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);
 auto v=pipeline->shadowWindows[0][2],f=pipeline->shadowWindows[1][1];assert(v.kind==1 && v.start==0 && v.size==16);assert(f.kind==1 && f.start==0 && f.size==16);
 assert(pipeline->shadowWindows[0][0].kind==2 && pipeline->shadowWindows[1][0].kind==2);
 ShadowRanges vertices(4*1024*1024),uniforms(4*1024*1024);vertices.binding(pipeline->shadowWindows[0][0],65536);
 for(auto a:options.attributes)if(a.format)vertices.vertex(65536,2,6,false,options.strides[0],a.offset,a.format);
 uniforms.binding(v,262144);uniforms.binding(f,262160);assert(vertices.bytes()==120 && uniforms.bytes()==32);
 backend.destroy(pipeline);tr.close(lib);
 std::ifstream sharedFile(argv[2],std::ios::binary);std::vector<uint8_t> sharedAIR{std::istreambuf_iterator<char>(sharedFile),{}};lib=tr.open(sharedAIR.data(),sharedAIR.size(),why,sizeof(why));assert(lib);
 pipeline=backend.compilePipeline(lib,"fuxi_layer_vertex",lib,"fuxi_layer_fragment",options);assert(pipeline->shadowWindows[0][0].kind==0);
 backend.destroy(pipeline);tr.close(lib);puts("PASS: real AIR bounded vertex/fragment uniforms, vertex descriptor union and shared shader-argument full fallback");
}
