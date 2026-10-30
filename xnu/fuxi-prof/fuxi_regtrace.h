#ifndef FUXI_REGTRACE_H
#define FUXI_REGTRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct frt_token { uint64_t start,last,stage[3],bytes;unsigned phase; };
uint64_t fuxi_regtrace_begin(void);
void fuxi_regtrace_phase(struct frt_token *t,unsigned phase);
void fuxi_regtrace_end(struct frt_token *t,const char *cls,const char *key,const char *plane,uint32_t options,unsigned api);
void fuxi_regtrace_work_end(uint64_t *start);
void fuxi_regtrace_start(void);
#ifdef __cplusplus
}
#endif
#endif
