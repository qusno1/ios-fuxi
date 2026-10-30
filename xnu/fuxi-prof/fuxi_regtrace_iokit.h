#ifndef FUXI_REGTRACE_IOKIT_H
#define FUXI_REGTRACE_IOKIT_H
#if defined(FUXI)
#include <kern/fuxi_regtrace.h>
class FuxiRegTrace {
 struct frt_token t;
 const char *cls,*key,*plane;
 uint32_t options;
 unsigned api;
public:
 FuxiRegTrace(IORegistryEntry *entry,const char *key,const char *plane,uint32_t options,unsigned api):t{},cls(NULL),key(key),plane(plane),options(options),api(api) {
  t.start=fuxi_regtrace_begin();t.last=t.start;
  if(t.start)cls=entry?entry->getMetaClass()->getClassName():"invalid";
 }
 ~FuxiRegTrace(){if(t.start)fuxi_regtrace_end(&t,cls,key,plane,options,api);}
 void phase(unsigned p){if(t.start)fuxi_regtrace_phase(&t,p);}
 void size(uint64_t n){t.bytes=n;}
};
#define FRT_SCOPE(e,k,p,o,a) FuxiRegTrace frt(e,k,p,o,a)
#define FRT_PHASE(p) frt.phase(p)
#define FRT_SIZE(n) frt.size(n)
#else
#define FRT_SCOPE(e,k,p,o,a) do {} while(0)
#define FRT_PHASE(p) do {} while(0)
#define FRT_SIZE(n) do {} while(0)
#endif
#endif
