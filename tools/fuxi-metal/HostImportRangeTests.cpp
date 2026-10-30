#include "HostImportRange.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace fuxi;
int main(){
 const size_t P=16384,A=64,M=size_t(1)<<30;
 // Already aligned: identity, no offset.
 auto a=planHostImport(5*P,2*P,P,A,M);assert(a.ok() && a.base==5*P && a.bytes==2*P && a.offset==0);
 // Aligned pointer, ragged length rounds up to the next page.
 a=planHostImport(5*P,P+1,P,A,M);assert(a.ok() && a.base==5*P && a.bytes==2*P && a.offset==0);
 // Unaligned pointer inside one page: one page, offset kept.
 a=planHostImport(5*P+256,100,P,A,M);assert(a.ok() && a.base==5*P && a.bytes==P && a.offset==256);
 // Range straddling a page boundary needs both pages.
 a=planHostImport(6*P-64,128,P,A,M);assert(a.ok() && a.base==5*P && a.bytes==2*P && a.offset==P-64);
 // Last byte exactly at a page end stays in one page; one more spills.
 a=planHostImport(5*P+64,P-64,P,A,M);assert(a.ok() && a.bytes==P);
 a=planHostImport(5*P+64,P-63,P,A,M);assert(a.ok() && a.bytes==2*P);
 // Every imported page holds a caller byte, and the caller range is inside the import.
 for(size_t off=0;off<2*P;off+=64)for(size_t len:{size_t(1),size_t(64),P-64,P,P+64,3*P+128}){
  uintptr_t ptr=7*P+off;a=planHostImport(ptr,len,P,A,M);assert(a.ok());
  assert(a.base%P==0 && a.bytes%P==0 && a.base<=ptr && ptr+len<=a.base+a.bytes && a.base+a.offset==ptr);
  assert(a.base+P>ptr && ptr+len>a.base+a.bytes-P);
 }
 // Offsets the buffer cannot bind at are refused, not rounded (would shift contents).
 a=planHostImport(5*P+8,64,P,A,M);assert(a.why==HostImportWhy::bufferAlignment);
 assert(planHostImport(5*P+8,64,P,0,M).ok() && planHostImport(5*P+8,64,P,8,M).ok());
 // Limits, wraparound and degenerate input.
 assert(planHostImport(5*P,2*P,P,A,P).why==HostImportWhy::tooLarge);
 assert(planHostImport(5*P+1,P,P,1,P).why==HostImportWhy::tooLarge);
 assert(planHostImport(0,P,P,A,M).why==HostImportWhy::range);
 assert(planHostImport(P,0,P,A,M).why==HostImportWhy::range);
 assert(planHostImport(P,P,0,A,M).why==HostImportWhy::range && planHostImport(P,P,3,A,M).why==HostImportWhy::range);
 assert(planHostImport(UINTPTR_MAX-63,128,P,A,M).why==HostImportWhy::range);
 assert(planHostImport(UINTPTR_MAX-P+1,P-1,P,A,M).why==HostImportWhy::range);
 assert(planHostImport(UINTPTR_MAX&~uintptr_t(P-1),1,P,A,M).why==HostImportWhy::range);
 puts("PASS: host import enclosing-page range, offset and refusal math");
}
