/* Original bounded 2D subresource bookkeeping, shared by host model and Vulkan. */
#pragma once
#include <algorithm>
#include <cstdint>
namespace fuxi {
inline uint32_t mipCount(uint32_t w,uint32_t h) {
 uint32_t n=0;for(uint32_t d=std::max(w,h);d;d>>=1)++n;return n;
}
struct TextureLevels {
 uint32_t width=256,height=256,levels=1,textureType=2,depth=1;
 // Preserve the level-zero field used by completion qualification and imports.
 bool defined=false;uint32_t definedLevels=0;
 uint32_t levelWidth(uint32_t l) const {return l<levels && l<32?std::max(1u,width>>l):0;}
 uint32_t levelHeight(uint32_t l) const {return l<levels && l<32?std::max(1u,height>>l):0;}
 uint32_t levelDepth(uint32_t l) const {return l<levels && l<32?std::max(1u,depth>>l):0;}
 bool isDefined(uint32_t l) const {return l<levels && l<32 && (l?bool(definedLevels&(1u<<l)):defined);}
 void setDefined(uint32_t l,bool value=true) {if(l>=levels || l>=32)return;if(l==0)defined=value;if(value)definedLevels|=1u<<l;else definedLevels&=~(1u<<l);}
};
}
