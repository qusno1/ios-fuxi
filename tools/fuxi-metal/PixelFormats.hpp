/* Metal numeric ABI shared by the portable validator and both backends. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
namespace fuxi {
inline size_t metalTexelBytes(uint32_t format) {
 switch(format){
 case 1:case 10:return 1;
 case 23:case 25:case 30:case 43:return 2;
 case 55:case 70:case 80:case 81:case 94:return 4;
 case 105:case 115:return 8;
 default:throw std::runtime_error("unsupported pixel format");
 }
}
// 550 (private two-plane XR10) and 554 (biased BGR10_XR) deliberately
// have no single-image Vulkan mapping. See metal-m9n.md; UNORM is not XR.
inline bool textureFormatAccepted(uint32_t format) {
 switch(format){case 1:case 10:case 23:case 25:case 30:case 43:case 55:case 70:case 80:case 94:case 105:case 115:return true;default:return false;}
}
// A 2D linear image reads only width*texel bytes from its last row; CA passes that tight size.
inline bool linearImageFits(uint64_t image,uint64_t row,uint64_t width,uint64_t height,uint32_t format) {
 return !image || (height && image>=row*(height-1)+width*metalTexelBytes(format));
}
// A 1D image must retain its dimension through image creation and shader views.
// Height-one 2D storage alone cannot satisfy a SPIR-V Dim1D sampled image.
// A 3D image is sampled-only and single-level: no 3D render target, storage or mip chain.
inline bool textureShapeAccepted(uint32_t type,uint32_t height,uint32_t usage,uint32_t depth=1,uint32_t levels=1) {
 if(type==7)return depth>=1 && levels==1 && (usage&1) && !(usage&4);
 return depth==1 && (type==2 || (type==0 && height==1 && (usage&1) && !(usage&4)));
}
}
