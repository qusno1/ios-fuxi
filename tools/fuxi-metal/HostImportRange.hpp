#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
// Page math for no-copy host imports: Vulkan imports whole host pages, but CA hands
// over unaligned pointers. Importing the enclosing pages and binding the buffer at
// the offset keeps contents==caller pointer. Every imported page holds at least one
// caller byte, so no unmapped page is ever requested.
namespace fuxi {
enum class HostImportWhy {ok,range,tooLarge,bufferAlignment};
struct HostImportPlan {HostImportWhy why=HostImportWhy::range;uintptr_t base=0;size_t bytes=0,offset=0;bool ok() const {return why==HostImportWhy::ok;}};
inline HostImportPlan planHostImport(uintptr_t pointer,size_t length,size_t page,size_t bufferAlignment,size_t maxBytes) {
 HostImportPlan p;
 if(!pointer || !length || !page || (page&(page-1)))return p;
 if(length>UINTPTR_MAX-pointer)return p;
 uintptr_t end=pointer+length,base=pointer&~uintptr_t(page-1);
 if(end>UINTPTR_MAX-(page-1))return p;
 uintptr_t alignedEnd=(end+(page-1))&~uintptr_t(page-1);
 p.base=base;p.offset=pointer-base;p.bytes=alignedEnd-base;
 if(p.bytes>maxBytes){p.why=HostImportWhy::tooLarge;return p;}
 // The VkBuffer binds at the offset inside the imported memory object.
 if(bufferAlignment && p.offset%bufferAlignment){p.why=HostImportWhy::bufferAlignment;return p;}
 p.why=HostImportWhy::ok;return p;
}
inline const char *hostImportWhyText(HostImportWhy w) {
 switch(w){case HostImportWhy::ok:return "ok";case HostImportWhy::range:return "range";case HostImportWhy::tooLarge:return "too-large";case HostImportWhy::bufferAlignment:return "buffer-alignment";}
 return "?";
}
// Process-wide outcome counters so a device log shows which rule refuses client memory.
struct HostImportStats {
 static inline std::atomic<uint64_t> attempts{0},aligned{0},enclosed{0},refusedPlan{0},refusedVulkan{0};
};
}
