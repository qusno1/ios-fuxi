/* Original exact-input cache-key controls; no external translator or GPU. */
#define FUXI_MTL_HOST_TEST
#include "Translator.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 using namespace fuxi;DrawInput a{};
 auto different=[&](const DrawInput &b){assert(!sameDrawInput(a,b) && !sameDrawInput(b,a));};
 assert(sameDrawInput(a,a));
 for(auto field:{&DrawInput::count,&DrawInput::start,&DrawInput::indexed,&DrawInput::strip}){auto b=a;b.*field=1;different(b);}
 for(auto field:{&DrawInput::indexLength,&DrawInput::indexOffset}){auto b=a;b.*field=2;different(b);}
 for(unsigned i=0;i<resourceSlots;++i){
  auto b=a;b.formats[i]=80;different(b);b=a;b.filters[i]=1;different(b);
  b=a;b.lodMin[i]=-0.0f;different(b);b=a;b.lodMax[i]=12;different(b);
  for(unsigned stage=0;stage<2;++stage){
   b=a;b.buffers[stage][i].length=16;different(b);b=a;b.buffers[stage][i].offset=4;different(b);
   b=a;b.buffers[stage][i].bytes=1;different(b);
  }
 }
 // Identical NaN payloads are equal as ABI bits, unlike float ==.
 uint32_t bits=0x7fc00001;memcpy(&a.lodMin[0],&bits,4);auto b=a;assert(sameDrawInput(a,b));
 puts("PASS: exact draw-input key covers every slot/stage/field and float bits");
}
