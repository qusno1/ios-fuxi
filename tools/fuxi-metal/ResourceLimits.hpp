/* Original shared resource bounds. No dimension/stride products before checks. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace fuxi {
struct ResourceLimits {
 static constexpr size_t budget=256u*1024*1024;
 static constexpr size_t single=128u*1024*1024;
 uint32_t dimension=4096;
 size_t used=0;
 explicit ResourceLimits(uint32_t device=4096):dimension(std::min(device,4096u)){}
 bool extent(size_t w,size_t h,size_t pixel) const {
  return w && h && w<=dimension && h<=dimension && pixel && w<=single/pixel && h<=single/(w*pixel);
 }
 bool volume(size_t w,size_t h,size_t d,size_t pixel) const {
  return extent(w,h,pixel) && d && d<=dimension && d<=single/(w*h*pixel);
 }
 bool rows(size_t w,size_t h,size_t pixel,size_t row,size_t offset,size_t length) const {
  return extent(w,h,pixel) && row>=w*pixel && row%pixel==0 && offset<=length && row<=single/h && row*h<=length-offset;
 }
 // Buffer textures need only the used texels of the final padded row.
 bool linearRows(size_t w,size_t h,size_t pixel,size_t row,size_t offset,size_t length) const {
  return extent(w,h,pixel) && row>=w*pixel && row%pixel==0 && offset%pixel==0 &&
   offset<=length && row<=single/h && (h-1)*row+w*pixel<=length-offset;
 }
 void reserve(size_t bytes) {if(!bytes || bytes>single || bytes>budget-used)throw std::runtime_error("resource byte budget");used+=bytes;}
 void release(size_t bytes){used-=bytes;}
};
inline bool scissorFits(size_t x,size_t y,size_t w,size_t h,size_t targetW,size_t targetH) {
 return x<=targetW && y<=targetH && w<=targetW-x && h<=targetH-y;
}
inline bool viewportFits(double x,double y,double w,double h,double near,double far,size_t targetW,size_t targetH) {
 return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) && std::isfinite(near) && std::isfinite(far) &&
  x>=0 && y>=0 && x<=targetW && y<=targetH && w>0 && h>0 && w<=targetW-x && h<=targetH-y && near>=0 && far<=1 && near<=far;
}
}
