#!/usr/bin/env python3
"""Actual startup clock diagnostics with fault/rollover/register models, offline.

No reference checkout is imported or executed. The full HFI table/selection
coverage is in test-fuxi-gpu-probe.py; boot parsing in adreno-mapping.py.
"""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('adreno', ROOT/'tools/test-fuxi-adreno.py')
adreno = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adreno)

MODEL = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "gpu-clock.h"
static unsigned g_gpu_selected, fg_clock_measure;
static uint64_t g_gpu_ddr_requested;
static uint32_t gpu_extra[1][128];
static uint64_t ticks, counter_base;
static unsigned rate, mode, writes, reads, restore_writes;
static uint32_t control, selector, cx, gx;
static void model_log(const char *format, ...) {
 va_list ap;va_start(ap,format);vprintf(format,ap);va_end(ap);
}
#define printf model_log
static uint64_t cpu_counter(const char *name) {
 return !strcmp(name,"cntfrq_el0") ? 1000000 : ++ticks;
}
#define rsr(name) cpu_counter(name)
static void delay(unsigned us) {ticks+=us;}
static uintptr_t fuxi_gpu_nc_pointer(uint64_t pa, unsigned size) {
 assert(size==4);return mode==1?0:pa;
}
static unsigned fuxi_gpu_access(uintptr_t pa,uint64_t *value,unsigned write,unsigned wide) {
 assert(!wide);reads+=!write;writes+=write;
 if(mode==2)return 1;
 if(pa==GPU_GPUCC_BASE+CC_CX_GDSCR){assert(!write);*value=cx;return 0;}
 if(pa==GPU_GPUCC_BASE+CC_GX_GDSCR){assert(!write);*value=gx;return 0;}
 if(pa>=GPU_GPUCC_BASE && pa<GPU_GPUCC_BASE+0x10000) {
  assert(!write);
  switch(pa-GPU_GPUCC_BASE) {
   case CC_PLL0+PLL_MODE:*value=0x80000005;break;
   case CC_PLL0+PLL_L_VAL:*value=0x4444001e;break;
   case CC_PLL0+0x14:*value=0xbaaa;break;
   case CC_PLL1+PLL_MODE:*value=0x80000005;break;
   case CC_PLL1+PLL_OPMODE:*value=1;break;
   case CC_PLL1+PLL_L_VAL:*value=0x44440016;break;
   case CC_PLL1+0x14:*value=0xeaaa;break;
   case CC_GMU_RCGR+4:*value=0x303;break;
   default:assert(0);
  }
  return 0;
 }
 if(write) {
  if(writes>2)restore_writes++;
  if(mode==3 && writes==2)return 1; // failed enable, still restore BOTH
  if(pa==GPU_REG_BASE+0x500*4)control=*value;
  else {assert(pa==GPU_REG_BASE+0x50a*4);selector=*value;}
 } else if(pa==GPU_REG_BASE+0x500*4)*value=control;
 else if(pa==GPU_REG_BASE+0x50a*4)*value=selector;
 else {
  assert(pa==GPU_REG_BASE+0x322*4 || pa==GPU_REG_BASE+0x323*4);
  uint64_t counter=counter_base+ticks*rate;
  *value=pa==GPU_REG_BASE+0x322*4?(uint32_t)counter:counter>>32;
  if(mode==4 && pa==GPU_REG_BASE+0x323*4)*value=reads; // never stable
 }
 return 0;
}
'''
CASES = r'''
static void reset(void) {
 ticks=0;counter_base=0xfffffff0ULL;rate=475;mode=writes=reads=restore_writes=0;
 control=0x20;selector=7;cx=gx=1u<<31;
 g_gpu_selected=fg_clock_measure=0;g_gpu_ddr_requested=0;
}
int main(void) {
 // Keep helpers from the shared header visible to the compiler as in gpu.c.
 assert(g_gpu_select(8)==8 && !g_gpu_select(9) && g_gpu_bw_index(8)==2);(void)g_gpu_bw_packet;
 reset();puts("SAMPLE_OK");fg_clock_sample();
 assert(control==0x20 && selector==7 && restore_writes==2 && ticks>=1000);
 for(unsigned m=1;m<=4;m++) {
  reset();mode=m;puts("SAMPLE_FAIL");fg_clock_sample();
  assert(control==0x20 && selector==7);
  if(m==1 || m==2)assert(!writes);else assert(restore_writes==2);
 }
 reset();rate=0;puts("SAMPLE_FAIL");fg_clock_sample();assert(selector==7 && control==0x20);
 reset();puts("LOG_DEFAULT");fg_clock_log();assert(!writes);
 reset();g_gpu_selected=4;fg_clock_measure=1;puts("LOG_ACTIVE");fg_clock_log();assert(writes==4);
 reset();fg_clock_measure=1;gx=0;puts("LOG_POWER_OFF");fg_clock_log();assert(!writes);
 reset();fg_clock_measure=1;mode=2;puts("LOG_READ_FAULT");fg_clock_log();assert(!writes);
 const unsigned levels[]={0,1,2,6,7,8,7,8,6,6};
 const uint64_t requests[]={8,8,8,8,0,0,8,8,9,UINT64_MAX};
 for(unsigned i=0;i<sizeof(levels)/sizeof(levels[0]);i++) {
  reset();g_gpu_selected=levels[i];g_gpu_ddr_requested=requests[i];fg_clock_log();assert(!writes);
 }
 puts("PASS: actual diagnostics, GPUCC-derived GMU rate, gated counter sample, bounded rollover, fault/zero handling and restoration");
}
'''

def main():
    driver = (ROOT/'xnu/fuxi-adreno/fuxi_adreno.c').read_text()
    probe = (ROOT/'shim/fuxi-probe/gpu.c').read_text()
    names = ('GPU_REG_BASE','GPU_GPUCC_BASE','CC_PLL0','CC_PLL1','PLL_MODE',
             'PLL_OPMODE','PLL_L_VAL','PLL_OUTCTRL','PLL_RUN','CC_GMU_RCGR',
             'CC_CX_GDSCR','CC_GX_GDSCR','GDSC_PWR_ON')
    defines = '\n'.join(re.search(r'^#define '+n+r'\s+.*', probe, re.M)[0] for n in names)
    functions = adreno.function(probe, 'g_gdsc_on')
    functions += ''.join(adreno.function(driver, n) for n in
                         ('fg_clock_io','fg_clock_counter','fg_clock_sample','fg_clock_log'))
    functions = re.sub(r'__asm__ volatile\([^;]*\);', ';', functions)
    with tempfile.TemporaryDirectory() as d:
        path = Path(d)/'clock.c'
        path.write_text(defines+'\n'+MODEL+functions+CASES)
        exe = Path(d)/'clock'
        subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g',
                        '-fsanitize=address,undefined','-I'+str(ROOT/'shim/fuxi-probe'),
                        str(path),'-o',str(exe)],check=True)
        out = subprocess.run([str(exe)],check=True,text=True,capture_output=True).stdout
        assert 'rbbm_count_khz=475000 sample_us=1001' in out, out
        assert out.count('rbbm_count_khz=unavailable') == 6, out
        assert 'level=0 gpu_index=1 requested_khz=220000' in out
        assert 'level=4 gpu_index=4 requested_khz=401000 gx_arc=0 bw_index=2 ddr_kBps=6074218' in out
        assert 'bw_index=0 ddr_kBps=0 ddr_requested=0 ddr_clamped=0 hfi_ack=1' in out
        for level, khz, kbps, requested, clamped in (
                (0,220000,0,8,0), (1,220000,2136718,8,0), (2,295000,6074218,8,3),
                (6,550000,12449218,8,7), (7,615000,10687500,0,0),
                (8,680000,12449218,0,0), (7,615000,12449218,8,7),
                (8,680000,14398437,8,8), (6,550000,6074218,9,0),
                (6,550000,6074218,2**64-1,0)):
            line = next(line for line in out.splitlines() if
                        f'level={level} ' in line and f'ddr_requested={requested} ' in line)
            assert f'requested_khz={khz} ' in line, line
            assert f'ddr_kBps={kbps} ddr_requested={requested} ddr_clamped={clamped} hfi_ack=1' in line, line
        assert 'gmu_rcgr=0x303 gmu_derived_khz=219999' in out
        assert 'gpucc_readback=unavailable' in out
        print('PASS: '+out.rsplit('PASS:',1)[-1].strip())
        # Compile the same functions with their real ARM barriers/counter reads,
        # without an Apple SDK, firmware, reference code or a kernel link.
        cross = r'''
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;
#include "gpu-clock.h"
extern int printf(const char *, ...) __attribute__((format(printf,1,2)));
extern uintptr_t fuxi_gpu_nc_pointer(uint64_t, uint64_t);
extern unsigned fuxi_gpu_access(uintptr_t,uint64_t *,unsigned,unsigned);
extern void delay(unsigned);
#define rsr(r) ({uint64_t v;__asm__ volatile("mrs %0, " r : "=r"(v));v;})
static unsigned fg_clock_measure, g_gpu_selected;
static uint64_t g_gpu_ddr_requested;
static uint32_t gpu_extra[1][128];
'''
        raw = adreno.function(probe, 'g_gdsc_on')
        raw += ''.join(adreno.function(driver, n) for n in
                       ('fg_clock_io','fg_clock_counter','fg_clock_sample','fg_clock_log'))
        path.write_text(defines+'\n'+cross+raw+'''
void compile_clock(void) {fg_clock_log();(void)g_gpu_select(1);(void)g_gpu_bw_packet;}
''')
        for target in ('aarch64-none-elf','arm64-apple-darwin'):
            subprocess.run(['clang','--target='+target,'-ffreestanding','-O2',
                            '-Wall','-Wextra','-Werror','-I'+str(ROOT/'shim/fuxi-probe'),
                            '-c',str(path),'-o',str(Path(d)/'clock.o')],check=True)
        print('PASS: actual startup diagnostics compile for AArch64 ELF and arm64 Darwin')

if __name__ == '__main__':
    main()
