#!/usr/bin/env python3
"""Offline native-memory model of gpu.c, never opens a device.

Removes only AArch64 barriers, substitutes the counter and hooks MMIO writes.
This checks C protocol/memory logic, not SMMU hardware or firmware execution.
Optional external firmware input exercises the exact local block image too.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
static uintptr_t probe_fdt;
static uint32_t probe_fdt_size;
char _head[1], _end[1];
extern uint8_t _gpu_fw_start[], _sqe_fw_start[], _sqe_fw_end[], _zap_fw_start[], _zap_fw_end[];
static uint64_t ticks;
static size_t test_zap_bytes;
#define rsr(reg) (strcmp(reg,"cntfrq_el0") ? ++ticks : 1000000ull)
static void delay(unsigned us) { ticks += us; }
static void mock_write(uint64_t at, uint32_t val);
static void mock_smc(uint64_t ret[4], uint32_t fn, uint32_t info, uint64_t a, uint64_t b, uint64_t c);
'''
SUFFIX = r'''
static int firmware_success = 1, ack_mode, corrupt_readback, tlb_busy;
static unsigned scm_calls, scm_fail_call, scm_service_fail, secure_mode, require_zap;
static uint64_t scm_transport, scm_available, scm_supported;
static unsigned metadata_protected;
static uint8_t metadata_snapshot[G5_META_BYTES];

static unsigned firmware_releases, mmio_writes;
static int cp_mode; /* 1 stalled, 2 fault, 3 missing fence, 4 corrupt fence guard */
static uint64_t abort_read, abort_write;
static unsigned blit_mode; /* 8 clear-only loss, 9 copy-only loss, 10 omit clean, 11 omit BR */
static void model_blit_stream(unsigned end);
static unsigned drop_sqe_base, start_fault, doorbells, sqe_controls;
uint32_t probe_fault_esr;
uint64_t probe_fault_far;
uint32_t probe_safe_rd32(uint64_t pa, uint32_t *faulted)
{
 assert(!metadata_protected || pa < G5_META_PA || pa >= G5_META_PA+G5_META_BYTES);
 *faulted = pa == abort_read;
 if (*faulted) { probe_fault_esr=0x96000010; probe_fault_far=pa; return 0xdeadfa17; }
 const unsigned ports[3][2] = {{G3_STAT_ADDR,G3_STAT_DATA},{G3_BV_STAT_ADDR,G3_BV_STAT_DATA},{G3_AC_STAT_ADDR,G3_AC_STAT_DATA}};
 for (unsigned j=0;j<3;j++)
  if (pa == GPU_REG_BASE+ports[j][1]*4)
   return (0xa7400000u+j*0x100) | *(volatile uint32_t *)(uintptr_t)(GPU_REG_BASE+ports[j][0]*4);
 return *(volatile uint32_t *)(uintptr_t)pa;
}
unsigned probe_safe_wr32(uint64_t pa, uint32_t v)
{
 assert(!metadata_protected || pa < G5_META_PA || pa >= G5_META_PA+G5_META_BYTES);
 if (pa == abort_write) { probe_fault_esr=0x96000050; probe_fault_far=pa; return 1; }
 g_wr(pa, 0, v); return 0;
}
static uint32_t *ptr(uint64_t at) { return (uint32_t *)(uintptr_t)at; }
static void mock_smc(uint64_t ret[4], uint32_t fn, uint32_t info, uint64_t a, uint64_t b, uint64_t c)
{
 scm_calls++;
 assert(gpu_extra[5][3]==1 && gpu_extra[5][4]==1);
 assert(*ptr(GPU_REG_BASE+0x806*4)==G3_FENCE_DWORDS);
 if (fn==0x4200010a) { assert(info==2 && a==0 && b==13 && c==0 && g5_zap_loaded); }
 else {
  const unsigned fns[]={0x42000601,0x42000207,0x42000601,0x42000201,
                        0x42000601,0x42000202,0x42000601,0x42000205};
  unsigned relocated=!!(cdb32(_zap_fw_start+108)&(1u<<27));
  unsigned index=scm_calls-1;
  if (!relocated && scm_calls>=5) index+=2;
  assert(index<8 && fn==fns[index]);
  uint32_t *slot=g5_scm_slot(gpu_extra[13],scm_calls-1);
  assert(gpu_extra[13][5]==scm_calls && !slot[3] && slot[1]==fn && slot[2]==info);
  assert((slot[4]|(uint64_t)slot[5]<<32)==a && (slot[6]|(uint64_t)slot[7]<<32)==b);
  if (metadata_protected) assert(!memcmp((void *)(uintptr_t)G5_META_PA,metadata_snapshot,G5_META_BYTES));
  if(fn==0x42000601) {
   const unsigned args[]={0x02000207,0,0x02000201,0,0x02000202,0,0x02000205};
   assert(info==1 && a==args[index] && !b && !c);
  } else if(fn==0x42000207) assert(info==1 && a==13 && !b && !c);
  else if(fn==0x42000201) {
   assert(info==0x82 && a==13 && b==G5_META_PA && !c);
   assert(!(G5_META_PA&4095));
   assert(memcmp((void *)(uintptr_t)G5_META_PA,_zap_fw_start,0x94)==0);
   assert(memcmp((void *)(uintptr_t)(G5_META_PA+0x94),_zap_fw_start+0x2000,0xf38)==0);
   assert(*ptr(G5_ZAP_PA)==0x6b6b6b6b); /* init before LOAD copy */
   memcpy(metadata_snapshot,(void *)(uintptr_t)G5_META_PA,G5_META_BYTES);
  } else if(fn==0x42000202) assert(info==3 && a==13 && b==G5_ZAP_PA && c==0x1000);
  else {
   assert(info==1 && a==13 && !b && !c);
   assert(memcmp((void *)(uintptr_t)G5_ZAP_PA,_zap_fw_start+0x1000,0x430)==0);
  }
 }
 ret[0]=0; ret[1]=fn==0x42000601 ? scm_available : fn==0x42000207 ? scm_supported : 0;
 ret[2]=0x123456789abcdef0ull; ret[3]=0xfedcba9876543210ull;
 if (scm_calls==scm_fail_call) {
  ret[0]=scm_transport;
  if(scm_service_fail) ret[1]=(uint64_t)-7;
 }
 if(fn==0x42000201 && !ret[0] && !ret[1]) metadata_protected=1;
}

static void mock_write(uint64_t at, uint32_t val)
{
 mmio_writes++;
 if (drop_sqe_base && at == GPU_REG_BASE+G3_SQE_BASE*4) *ptr(at)=0;
 if (at == GPU_REG_BASE+G3_SQE_CNTL*4) sqe_controls++;
 assert(at != GPU_REG_BASE+0xf400*4); /* TRUST is strictly read-only */
 /* Model the suspected premature fetch explicitly, not SQE instructions.
  * Both 3 and 1 contain enable bit0. A zero-base fetch is latched even if
  * software later repairs INSTR_BASE; this models the observed signature. */
 if (at == GPU_REG_BASE+G3_SQE_CNTL*4 && (val&1) &&
     (start_fault || !*ptr(GPU_REG_BASE+G3_SQE_BASE*4))) {
  *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=0x402;
  *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x68)=0xb0041;
  *ptr(GPU_SMMU_BASE+g_stride+0x400+g3_bank*4)=0;
  *ptr(GPU_REG_BASE+0x823*4)=0x80;
 }
 if (at == GPU_REG_BASE+0x807*4 && val) {
  doorbells++;
  assert(gpu_extra[5][7] && g3_bank != g_bank && g3_bank != 0);
  assert(*ptr(GPU_REG_BASE+0x808*4)==1 && *ptr(GPU_REG_BASE+0x802*4)==0x20c);
  assert(*ptr(GPU_REG_BASE+0x830*4)==G3_SQE_VA);
  assert(*ptr(G3_RING_PA)==0x70970001 && *ptr(G3_RING_PA+4)==1u<<27);
  assert(*ptr(G3_RING_PA+8)==0x70c80004 && *ptr(G3_RING_PA+12)==0x4b);
  if (cp_mode == 1) return;
  if (cp_mode == 2) { *ptr(GPU_REG_BASE+0x823*4)=4; return; }
  unsigned secure_packet = val==G3_FENCE_DWORDS+2 && *ptr(G3_RING_PA+G3_FENCE_DWORDS*4)==g3_pkt7(0x66,1);
  if (secure_packet) {
   assert(g5_zap_loaded && !*ptr(G3_RING_PA+(G3_FENCE_DWORDS+1)*4));
   if(secure_mode==1) return;
   if(secure_mode==2) { *ptr(GPU_REG_BASE+0x823*4)=4; return; }
   if(secure_mode!=3) *ptr(GPU_REG_BASE+0xf400*4)=0;
  }
  if (val > G3_FENCE_DWORDS && !secure_packet) { model_blit_stream(val); if (blit_mode == 1 || blit_mode == 4) return; }
  *ptr(GPU_REG_BASE+0x806*4)=val; *ptr(G3_PTR_PA)=val;
  if (val==G3_FENCE_DWORDS) {
   assert(*ptr(G3_RING_PA+G3_INIT_DWORDS*4)==0x703d8003);
   assert(*ptr(G3_RING_PA+(G3_INIT_DWORDS+1)*4)==G3_FENCE_VA && !*ptr(G3_RING_PA+(G3_INIT_DWORDS+2)*4));
   if (cp_mode != 3) *ptr(G3_FENCE_PA)=*ptr(G3_RING_PA+(G3_INIT_DWORDS+3)*4);
   if (cp_mode == 4) *ptr(G3_FENCE_PA+4)=0xbad;
  }
 }
 if (at >= GPU_SMMU_BASE + g_cbbase && at < GPU_SMMU_BASE + 0x40000) {
  unsigned off = (at-GPU_SMMU_BASE-g_cbbase)%g_stride;
  if (off == 0x58) *ptr(at) = 0; /* W1C */
  if (off == 0x7f0) *ptr(at+4) = tlb_busy;
  if (off == 0x30 && corrupt_readback) *ptr(at) ^= 1;
 }
 if (at == GPU_GMU_CORE + 0x5000*4 && !val) {
  firmware_releases++;
  if (firmware_success) *ptr(GPU_GMU_CORE+0x501c*4) = 0x100;
 }
 if (at == GPU_GMU_CORE + 0x5186*4 && val) *ptr(GPU_GMU_CORE+0x5180*4) = 1;
 if (at == GPU_GMU_CORE + 0x5191*4) *ptr(GPU_GMU_CORE+0x5192*4) &= ~val;
 if (at != GPU_GMU_CORE + 0x5194*4) return;
 if (val == (1u<<30)) {
  *ptr(GPU_GMU_CORE+0x5192*4) |= 1u<<31;
  *ptr(GPU_GPUCC_BASE + CC_GX_GDSCR) = 1u<<31;
 }
 if (val != 1 || ack_mode == 1) return;
 unsigned rd = *ptr(G_HFI_PA+64), wr = *ptr(G_HFI_PA+68);
 unsigned hdr = *ptr(G_HFI_PA+4096+rd*4);
 unsigned q = *ptr(G_HFI_PA+116);
 *ptr(G_HFI_PA+8192+q*4) = (hdr&0xfff00000) | (1u<<16) | (3u<<8) | 126;
 *ptr(G_HFI_PA+8192+((q+1)&1023)*4) = hdr ^ (ack_mode == 2 ? 1u<<20 : 0);
 *ptr(G_HFI_PA+8192+((q+2)&1023)*4) = ack_mode == 3 ? 9 : 0;
 if (ack_mode == 4) *ptr(G_HFI_PA+8192+q*4) = 126; /* impossible size0 */
 *ptr(G_HFI_PA+116) = (q+4)&1023;
 *ptr(G_HFI_PA+64) = wr;
 *ptr(GPU_GMU_CORE+0x5192*4) |= 1;
}
static void reset_model(void)
{
 drop_sqe_base=start_fault=doorbells=sqe_controls=0;
 test_zap_bytes=(uintptr_t)_zap_fw_end-(uintptr_t)_zap_fw_start;
 scm_calls=scm_fail_call=scm_service_fail=secure_mode=require_zap=g5_zap_loaded=0; scm_transport=0; scm_available=scm_supported=1; metadata_protected=0;
 memset((void *)(uintptr_t)G5_ZAP_PA,0x6b,G5_ZAP_BYTES);
 memset((void *)(uintptr_t)GPU_REG_BASE, 0, 0x100000);
 memset((void *)(uintptr_t)G_MEM, 0, G_MEM_SIZE);
 memset(gpu_report, 0, sizeof(gpu_report)); memset(gpu_extra, 0, sizeof(gpu_extra));
 gpu_snapshot_valid = gpu_gmu_started = gpu_m2_attempted = gpu_s8_ok = 0;
 gpu_s9_attempted = gpu_s9_ok = gpu_s10_attempted = 0;
 g_gpu_selected = 0; g_gpu_ddr_requested = 0;
 ticks = firmware_releases = mmio_writes = 0;
 cp_mode=0; blit_mode=0; abort_read=abort_write=0;
 ack_mode = corrupt_readback = tlb_busy = 0; firmware_success = 1;
 *ptr(GPU_REG_BASE+0xf400*4) = 1;
 *ptr(GPU_SMMU_BASE+0x20) = 0x4c017e18;
 *ptr(GPU_SMMU_BASE+0x24) = 0x40000016;
 *ptr(GPU_SMMU_BASE+0x28) = 0x5511;
 *ptr(GPU_SMMU_BASE) = 0x2f0406;
 *ptr(GPU_GMU_CORE+0x5000*4) = 1;
 *ptr(GPU_GMU_CORE+(0x1c00+0xff8)*4) = 0x20010005;
 gpu_extra[0][0] = 0xa7400009;
 assert(g_snapshot());
}
static void aux_fixture(void)
{
 memset(gpu_cmddb, 0, sizeof(gpu_cmddb));
 uint16_t gfx[] = {0,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};
 uint16_t mx[] = {0,0x40,0x80,0xe0}, cx[] = {0,0x40,0x80};
 const char names[3][8] = {"gfx.lvl","mx.lvl","cx.lvl"};
 uint8_t *h = gpu_cmddb+8; h[0] = 3; h[4] = 0; h[5] = 2; h[6] = 3;
 unsigned off = 0;
 for (unsigned i=0;i<3;i++) {
  uint8_t *p = gpu_cmddb+144+i*24;
  memcpy(p,names[i],8);
  unsigned len = i == 0 ? sizeof(gfx) : i == 1 ? sizeof(mx) : sizeof(cx);
  p[20] = len; p[22] = off;
  memcpy(gpu_cmddb+144+512+off, i == 0 ? gfx : i == 1 ? mx : cx, len); off += len;
 }
 for (unsigned i=0;i<4;i++) gpu_report[87+i] = 0x50000+i*4;
}
static uint64_t walk(unsigned va)
{
 uint64_t *l1=(uint64_t *)(uintptr_t)G_MEM;
 if (!l1[va>>30]) return 0;
 uint64_t *l2=(uint64_t *)(uintptr_t)(l1[va>>30]&~0xfffull);
 if (!l2[(va>>21)&511]) return 0;
 uint64_t *l3=(uint64_t *)(uintptr_t)(l2[(va>>21)&511]&~0xfffull);
 return l3[(va>>12)&511];
}
/* Small PM4 interpreter for the documented subset. This does not model GPU
 * execution/cache ordering. Decode literal HW registers and translate every
 * memory access through the emitted PTEs (including write permissions). */
static uint64_t model_gpu_pa(unsigned va, unsigned write)
{
 uint64_t *l1=(uint64_t *)(uintptr_t)G3_PT;
 assert(l1[va>>30]);
 uint64_t *l2=(uint64_t *)(uintptr_t)(l1[va>>30]&~0xfffull);
 assert(l2[(va>>21)&511]);
 uint64_t *l3=(uint64_t *)(uintptr_t)(l2[(va>>21)&511]&~0xfffull);
 uint64_t pte=l3[(va>>12)&511];
 assert((pte&3)==3 && (!write || !(pte&128)));
 return (pte & 0xfffffffff000ull) | (va&4095);
}
static void model_blit_stream(unsigned end)
{
 unsigned pos=*ptr(GPU_REG_BASE+0x806*4);
 assert(end<=8192 && end>pos);
 if (blit_mode==1) return;
 if (blit_mode==4) { *ptr(GPU_REG_BASE+0x823*4)=4; return; }
 uint32_t regs[0xc000]={0}; unsigned copies=0, fences=0, br=0, pending=0;
 uint32_t cache[65536][2];
 while (pos<end) {
  uint32_t hdr=*ptr(G3_RING_PA+pos++*4);
  if ((hdr>>28)==4) {
   unsigned count=hdr&0x7f, reg=(hdr>>8)&0x3ffff;
   assert(reg+count<0xc000 && pos+count<=end);
   for(unsigned i=reg;i<reg+count;i++) for(unsigned slot=0;slot<48;slot++) {
    uint32_t pr=g3_protect[slot];
    if(pr&0x80000000u) assert(i<(pr&0x3ffffu) || i>(pr&0x3ffffu)+((pr>>18)&0x1fff));
   }
   assert(hdr==g4_pkt4(reg,count));
   while(count--) regs[reg++]=*ptr(G3_RING_PA+pos++*4);
  } else {
   assert((hdr>>28)==7);
   unsigned count=hdr&0x3fff, op=(hdr>>16)&0x7f;
   assert(hdr==g3_pkt7(op,count) && pos+count<=end);
   uint32_t *args=ptr(G3_RING_PA+pos*4);
   if (op==0x2c) {
    assert(count==1 && args[0]==3);
    unsigned solid=!!(regs[0x8c00]&128);
    assert(regs[0x8c00]==(0x10f03000u | solid<<7) && regs[0x8400]==regs[0x8c00]);
    assert(regs[0xa9bf]==0xf180 && regs[0xb2d2]==1u<<29);
    assert((solid || regs[0xb2c0]==0x500430) && regs[0x8c17]==0x430);
    if(solid) assert(!regs[0xb2c0] && !regs[0xb2c2]); /* no source state */
    assert(regs[0x88e5]==0xc0000000);
    assert(!regs[0xb2c3] && !regs[0x8c19]);
    unsigned sx=regs[0x8401]>>8, ex=regs[0x8402]>>8;
    unsigned sy=regs[0x8403]>>8, ey=regs[0x8404]>>8;
    unsigned dx=regs[0x8405]&0xffff, dy=regs[0x8405]>>16;
    unsigned bx=regs[0x8406]&0xffff, by=regs[0x8406]>>16;
    assert(solid || (ex-sx==bx-dx && ey-sy==by-dy));
    unsigned sp=(regs[0xb2c4]>>9)*64, dp=regs[0x8c1a]*64;
    assert((solid || sp==1024) && dp>=(bx+1)*4);
    assert((regs[0x8c18]&63)==0 && (regs[0xb2c2]&63)==0);
    for(unsigned y=0;y<=by-dy;y++) for(unsigned x=0;x<=bx-dx;x++) {
     unsigned src=regs[0xb2c2]+(sy+y)*sp+(sx+x)*4;
     unsigned dst=regs[0x8c18]+(dy+y)*dp+(dx+x)*4;
     uint32_t pixel=solid ? regs[0x8c2e] | regs[0x8c2d]<<8 | regs[0x8c2c]<<16 | regs[0x8c2f]<<24
                         : *ptr(model_gpu_pa(src,0));
     /* Independent model: A7xx writes remain in a separate blit cache until
      * resolve-clean. Color/depth clean and UCHE fence don't drain it. */
     if(br && (!require_zap || !*ptr(GPU_REG_BASE+0xf400*4)) && !(blit_mode==8 && solid) && !(blit_mode==9 && !solid)) {
      assert(pending<65536); cache[pending][0]=dst; cache[pending++][1]=pixel;
     }
    }
    copies++;
   } else if(op==0x17) {
    assert(count==1 && args[0]==((1u<<27)|1u));
    br=blit_mode!=11;
   } else if(op==0x46 && count==1 && args[0]==26) {
    if(blit_mode!=10) {
     for(unsigned i=0;i<pending;i++) *ptr(model_gpu_pa(cache[i][0],1))=cache[i][1];
     pending=0;
    }
   } else if(op==0x46 && count==4) {
    assert(args[0]==(49u|1u<<27) && !args[2]);
    if(blit_mode!=3) *ptr(model_gpu_pa(args[1],1))=args[3];
    fences++;
   } else assert((op==0x26 && count==0) || (op==0x65 && count==1 && args[0]==12) || (op==0x46 && count==1));
   pos+=count;
  }
 }
 assert(copies && fences==1);
 if (blit_mode==2) *ptr(G4_DST_PA+259*4)^=1;
 if (blit_mode==5) *ptr(G4_DST_PA-4)=0;
 if (blit_mode==6) *ptr(G4_SRC_PA)^=1;
 if (blit_mode==7) *ptr(G4_FB_PA+((G4_FB_Y)*1080+G4_FB_X-1)*4)^=1;
}
static void panel_fixture(void)
{
 *ptr(0x0ae05014)=0xb8000000;
 *ptr(0x0ae05024)=4320;
 *ptr(0x0ae05000)=(2400u<<16)|1080;
 *ptr(0x0ae05030)=0x237ff;
 memset((void *)(uintptr_t)G4_FB_PA,0x6b,G4_FB_MAP);
}

/* Synthetic cmd-db BCM metadata, never captured board data. */
static void bcm_fixture(uint32_t proportional_unit)
{
 aux_fixture();
 static const char names[4][8]={"SH0","MC0","ACV","CN0"};
 uint8_t *h=gpu_cmddb+24;
 h[0]=5;h[2]=0;h[3]=0x10;h[4]=0;h[5]=0x11;h[6]=4;
 for(unsigned j=0;j<4;j++) {
  uint8_t *p=gpu_cmddb+144+0x1000+j*24;
  memcpy(p,names[j],8);
  uint32_t address=0x50000+j*4;memcpy(p+16,&address,4);
  p[20]=j<3?8:0;p[22]=j*8;
  uint8_t *aux=gpu_cmddb+144+0x1100+j*8;
  uint32_t unit=j<2?proportional_unit:0;memcpy(aux,&unit,4);
  aux[4]=j==0?16:j==1?4:0;aux[6]=j?1:0;
 }
}
static void test_clock_votes(void)
{
 assert(g_gpu_select(0)==0 && g_gpu_select(6)==6 && g_gpu_select(7)==7 && g_gpu_select(8)==8);
 assert(!g_gpu_select(9) && !g_gpu_select(UINT64_MAX) && !g_gpu_select(0x100000001ULL));
 const unsigned max_ddr[]={0,0,3,3,4,5,7,7,8};
 const unsigned default_ddr[]={0,1,3,3,3,3,3,6,7};
 const uint32_t kbps[]={0,2136718,3000000,6074218,6671875,8171875,10687500,12449218,14398437};
 const uint64_t requests[]={0,1,2,3,4,5,6,7,8,9,UINT64_MAX,0x100000001ULL};
 /* Actual cold-start PERF/BW messages, ACK and GX vote for every selection
  * and override, with both non-saturating and saturating boot DB units. */
 for(unsigned unit_case=0;unit_case<2;unit_case++)
 for(unsigned n=0;n<=8;n++)
 for(unsigned r=0;r<sizeof(requests)/sizeof(requests[0]);r++) {
  uint64_t requested=requests[r];
  unsigned clamped=n<2 || requested>8 ? 0 : requested>max_ddr[n] ? max_ddr[n] : requested;
  unsigned dt=clamped?clamped:default_ddr[n];
  assert(g_gpu_ddr_clamp(n,requested)==clamped);
  reset_model();bcm_fixture(unit_case?1000:1000000);g_gpu_selected=n;g_gpu_ddr_requested=requested;
  assert(g_start() && gpu_s9_ok && firmware_releases==1);
  uint32_t *perf=ptr(G_HFI_PA+4096);
  const uint32_t freqs[]={0,220000,295000,348000,401000,475000,550000,615000,680000,719000};
  const unsigned arcs[]={0,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};
  assert(perf[0]==0x103b04 && perf[1]==10 && perf[2]==3);
  for(unsigned i=0;i<10;i++) {
   unsigned secondary=i==0?0:i<=4?1:i<=6?2:3;
   assert(perf[3+i*3]==(arcs[i]<<16|secondary<<8|i));
   assert(perf[4+i*3]==0xffffffff && perf[5+i*3]==freqs[i]);
  }
  uint32_t *wire=ptr(G_HFI_PA+4096+60*4);
  /* Golden packet layout, including all padding and legacy rows. Old levels
   * with absent/zero DDR retain the exact original off/active packets. */
  uint32_t expected[160]={0x20a003,n?3:1,1,3,1,n?5:7};
  expected[6]=0x5000c;expected[12]=0x40000000;expected[18]=0x60000001;
  for(unsigned j=0;j<3;j++) {
   expected[24+j]=0x50000+j*4;
   expected[32+j]=n && j==1?0:0x40000000;
  }
  if(n) {
   unsigned row2=n<2?6074:kbps[dt]/1000;
   unsigned votes[]={unit_case?16383:2136,unit_case?16383:row2};
   for(unsigned i=0;i<2;i++) {
    expected[40+i*8]=0x60000000u|votes[i]<<14|votes[i];
    expected[41+i*8]=0x20000000u|votes[i]<<14|votes[i];
    expected[42+i*8]=0x60000000;
   }
  }
  assert(!memcmp(wire,expected,sizeof(expected)));
  g_gx();assert(gpu_extra[0][60]==1);
  unsigned wr=*ptr(G_HFI_PA+68);
  uint32_t *vote=ptr(G_HFI_PA+4096+(wr-4)*4);
  assert(vote[0]==0x50041e && vote[1]==1 && vote[2]==(n?n:1));
  assert(vote[3]==(n?(n==1?1:2):0));
 }
 /* Each malformed input refuses before CM3 release or a HFI doorbell. */
 for(unsigned bad=0;bad<12;bad++) {
  reset_model();bcm_fixture(1000);g_gpu_selected=2;
  uint8_t *p=gpu_cmddb+144+0x1000,*aux=gpu_cmddb+144+0x1100;
  switch(bad) {
   case 0:p[0]='X';break; // SH0 absent
   case 1:p[20]=7;break; // aux length
   case 2:memset(aux,0,4);break; // zero unit
   case 3:aux[7]=1;break; // reserved aux byte
   case 4:memcpy(p+24,p,24);break; // duplicate SH0
   case 5:p[16]|=1;break; // unaligned address
   case 6:p[18]=3;break; // wrong slave
   case 7:aux[6]=16;break; // VCD cannot fit the controller domain
   case 8:aux[4]=0;break; // zero proportional width
   case 9:p[22]=p[23]=255;break; // aux outside snapshot
   case 10:memcpy(p+24+16,p+16,4);break; // duplicate resource address
   case 11:p[72]='X';break; // CN0 absent
  }
  assert(!g_start() && gpu_extra[0][3]==0x90a && !firmware_releases && !gpu_gmu_started);
 }
 // Independent proportional arithmetic: known small non-saturating votes.
 struct g_bcm b[3]={{0x50000,1000000,16,0},{0x50004,1000000,4,1},{0x50008,0,0,1}};
 uint32_t out[160];assert(g_gpu_bw_packet(b,0x5000c,14398437,out));
 assert(out[40]==(0x60000000u|(2136u<<14)|2136u));
 assert(out[41]==(0x20000000u|(2136u<<14)|2136u));
 // Largest permitted DDR index: matching widths are below the cap at unit
 // 1000000, and clamp safely for smaller units (no ACV performance bit).
 assert(out[48]==(0x60000000u|(14398u<<14)|14398u));
 assert(out[49]==(0x20000000u|(14398u<<14)|14398u) && out[50]==0x60000000);
 for(unsigned unit=878810;unit<=878865;unit++) {
  b[0].unit=b[1].unit=unit;
  assert(g_gpu_bw_packet(b,0x5000c,14398437,out));
  unsigned v=unit==878865?16382:16383;
  assert(out[48]==(0x60000000u|v<<14|v) && out[49]==(0x20000000u|v<<14|v));
 }
 // Largest allowed aux widths and smallest unit cannot overflow arithmetic.
 b[0].unit=b[1].unit=1;b[0].width=b[1].width=65535;
 assert(g_gpu_bw_packet(b,0x5000c,14398437,out) && out[48]==0x6fffffff && out[49]==0x2fffffff);
 b[0].unit=UINT32_MAX;b[0].width=1;
 assert(g_gpu_bw_packet(b,0x5000c,14398437,out) && out[40]==0x60004001);
 b[0].vcd=b[1].vcd=b[2].vcd;
 assert(g_gpu_bw_packet(b,0x5000c,14398437,out) && out[5]==4 && !out[32] && !out[33]);
 puts("PASS: GPU levels 0..8, DDR indices 1..8/bus-max clamps, wide-input bounds, exact legacy/active HFI tables/votes, BCM VCD grouping/arithmetic and pre-release metadata refusal");
}

int main(int argc, char **argv)
{
 (void)argc; (void)argv;
 assert(mmap((void *)(uintptr_t)GPU_REG_BASE, 0x100000, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)0x0b280000, 0x10000, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)G5_ZAP_PA, G5_ZAP_BYTES, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)G_MEM, G_MEM_SIZE, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)0x0ae05000, 0x1000, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)G4_FB_PA, G4_FB_MAP, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 test_clock_votes();
 reset_model(); unsigned before=mmio_writes;
 assert(g_snapshot() && before == mmio_writes && g_ncb == 22 && g_nsmr == 24 && g_cbbase == 0x20000);
 /* An active, referenced CB1 must be preserved; select CB2 instead. */
 *ptr(GPU_SMMU_BASE+SMMU_SMR(0))=0x80000000;
 *ptr(GPU_SMMU_BASE+SMMU_S2CR(0))=1;
 *ptr(GPU_SMMU_BASE+g_cb(1))=1;
 *ptr(GPU_SMMU_BASE+g_cb(1)+0x24)=1u<<16;
 assert(g_snapshot() && g_select_context() && g_bank==2 && g_slot==1 && g_asid==2);
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80000005;
 assert(g_snapshot() && !g_select_context() && gpu_extra[0][2]==0x804);
 /* Masked existing SID match must also refuse. */
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80070000;
 assert(g_snapshot() && !g_select_context());
 reset_model(); aux_fixture();
 assert(g_firmware(0));
 /* Corrupt external format headers in anonymous process memory, then restore. */
 uint32_t *header=(uint32_t *)(uintptr_t)_gpu_fw_start;
 uint32_t save_size=header[1], save_addr=header[0];
 header[1]=0xfffffffcu; assert(!g_firmware(0) && gpu_extra[0][2]==0x803); header[1]=save_size;
 header[0]=0xfffffffcu; assert(!g_firmware(0) && gpu_extra[0][2]==0x803); header[0]=save_addr;
 assert(g_firmware(0));
 assert(gpu_extra[0][18] == EXPECT_BLOCKS && gpu_extra[0][19] == EXPECT_PREALLOC);
 gpu_m2(8); assert(gpu_s8_ok && !gpu_gmu_started && g_bank==1);
 assert(gpu_extra[0][33] == 13);
 for(unsigned a=0x4000;a<0x1000000;a+=4096) {
  uint64_t pte=walk(a); assert((pte&~0xfffull)==G_IC_PA+a-0x4000 && !(pte&64));
 }
 assert(!walk(0) && !walk(0x1000000));
 assert((walk(G_LOG_VA)&~0xfffull)==G_LOG_PA && (walk(G_LOG_VA)&64));
 assert((walk(G_HFI_VA+0x3000)&~0xfffull)==G_HFI_PA+0x3000);
 assert((walk(G_DEBUG_VA+0x6000)&~0xfffull)==G_DEBUG_PA+0x6000);
 assert(!walk(G_DEBUG_VA+0x7000) && !walk(G_HFI_VA+0x4000));
 gpu_m2(9); assert(gpu_s9_ok && gpu_gmu_started && firmware_releases==1);
 assert(gpu_extra[0][40]==0x103b04 && gpu_extra[0][44]==0x20a003);
 for(unsigned i=0;i<4;i++) assert(gpu_extra[0][40+i*4]==gpu_extra[0][42+i*4] && gpu_extra[0][43+i*4]==0);
 before=mmio_writes; gpu_probe(9); assert(before==mmio_writes && firmware_releases==1 && gpu_gmu_started);
 gpu_m2(10); assert(gpu_extra[0][60]==1 && gpu_extra[0][65]==1);
 /* m3: two GPU streams, separate tables/bank/ASID; GMU memory untouched. */
 uint64_t gmu_root=*(uint64_t *)(uintptr_t)G_MEM;
 gpu_m2(11); assert(gpu_extra[5][2]==1 && g3_bank==2 && g3_asid==2);
 assert(g3_slot[0]==1 && g3_slot[1]==2 && gpu_extra[5][33]==3);
 assert(*(uint64_t *)(uintptr_t)G_MEM==gmu_root && g_bank==1 && g_asid==1);
 for (unsigned v=G3_SQE_VA;v<G3_FENCE_VA+4096;v+=4096) {
  uint64_t *l1=(void *)(uintptr_t)G3_PT;
  uint64_t *l2=(void *)(uintptr_t)(l1[v>>30]&~0xfffull);
  uint64_t *l3=(void *)(uintptr_t)(l2[(v>>21)&511]&~0xfffull);
  uint64_t pte=l3[(v>>12)&511];
  assert((pte&~0xfffull)==G3_SQE_PA+v-G3_SQE_VA);
  assert((pte&0xc0)==(v<G3_RING_VA?0xc0:v==G3_PTR_VA?0:0x40));
 }
 unsigned tail=(G3_FENCE_VA>>12)&511;
 uint64_t *root=(void *)(uintptr_t)G3_PT;
 uint64_t *mid=(void *)(uintptr_t)(root[1]&~0xfffull);
 uint64_t *leaf=(void *)(uintptr_t)(mid[0]&~0xfffull);
 assert(!root[0] && !root[2] && !leaf[tail+1] && !mid[1]);
 gpu_m2(12); assert(gpu_extra[5][3]==1 && gpu_extra[5][49]==G3_INIT_DWORDS && !gpu_extra[5][43] && gpu_extra[5][45]==1);
 assert(gpu_extra[5][17]==_sqe_fw_end-_sqe_fw_start && gpu_extra[5][18]==_sqe_fw_end-_sqe_fw_start-4);
 assert(!memcmp((void *)(uintptr_t)G3_SQE_PA,_sqe_fw_start+4,gpu_extra[5][18]));
 assert(sqe_controls==1 && gpu_extra[5][119]==7);
 assert(gpu_extra[6][34]==0 && gpu_extra[6][35]==0);
 assert(gpu_extra[6][44]==0 && gpu_extra[6][45]==G3_SQE_VA);
 assert(gpu_extra[6][54]==1 && gpu_extra[6][55]==G3_SQE_VA);
 unsigned prefix_words=gpu_extra[5][18]/4;
 if(prefix_words>12) prefix_words=12;
 assert(gpu_extra[5][109]==prefix_words);
 assert(!memcmp(gpu_extra[5]+97,_sqe_fw_start+4,prefix_words*4));
 gpu_m2(13); assert(gpu_extra[5][4]==1 && gpu_extra[5][53]==G3_MAGIC && gpu_extra[5][56]==G3_FENCE_DWORDS);
 assert(gpu_extra[7][3]==3 && gpu_extra[7][4]==32 && gpu_extra[7][63]==51);
 for (unsigned i=0;i<51;i++) {
  assert(gpu_extra[7][64+i]==(0xa7400000u|i));
  for (unsigned stage=0;stage<2;stage++) {
   assert(gpu_extra[9+stage][2]==51 && gpu_extra[9+stage][3]==51);
   assert(gpu_extra[9+stage][16+i]==(0xa7400100u|i));
   assert(gpu_extra[9+stage][67+i]==(0xa7400200u|i));
  }
 }
 before=mmio_writes; gpu_probe(13); gpu_probe(11); assert(before==mmio_writes);
 assert(gpu_extra[6][0]==12 && gpu_extra[6][64]==13 && gpu_gmu_started);
 FILE *f=fopen("report.bin","wb"); assert(f); gpu_report[0]=0xa740;
 fwrite(gpu_report,1,512,f); fwrite(gpu_extra,1,(G_PAGES-1)*512,f); fclose(f);
 /* Queue wrap and each failure are bounded and distinguishable. */
 for(int mode=0;mode<=4;mode++) {
  g_hfi_init(); ack_mode=mode;
  *ptr(G_HFI_PA+64)=*ptr(G_HFI_PA+68)=1020;
  *ptr(G_HFI_PA+112)=*ptr(G_HFI_PA+116)=1020;
  uint32_t msg[5]={0}; int err=g_hfi_send(14,msg,5,0);
  assert(err==(mode==0?0:mode==1?0x903:mode==2?0x906:mode==3?0x907:0x904));
 }
 g_hfi_init(); *ptr(G_HFI_PA+64)=4;
 uint32_t full[4]={0}; assert(g_hfi_send(14,full,4,0)==0x904);
 g_hfi_init(); *ptr(G_HFI_PA+68)=1024;
 uint32_t msg[1]={0}; assert(g_hfi_send(10,msg,1,0)==0x904);
 reset_model(); aux_fixture(); corrupt_readback=1; gpu_m2(8);
 assert(!gpu_s8_ok && gpu_extra[0][2]==0x809 && !gpu_gmu_started);
 reset_model(); aux_fixture(); tlb_busy=1; gpu_m2(8);
 assert(!gpu_s8_ok && gpu_extra[0][2]==0x808 && ticks<=1100);
 reset_model(); aux_fixture(); gpu_m2(8); firmware_success=0; gpu_m2(9);
 assert(!gpu_s9_ok && gpu_extra[0][3]==0x901 && gpu_gmu_started);
 before=mmio_writes; gpu_probe(10); gpu_probe(1);
 assert(before==mmio_writes && gpu_gmu_started && firmware_releases==1);
 assert(!g_fw_dest(0x10007ffc,8) && !g_fw_dest(0xfffffffc,8));
 /* GPU ownership: masked SID collision, no bank, snapshot fault, timeout. */
 reset_model(); aux_fixture();
 *ptr(GPU_SMMU_BASE+SMMU_SMR(3))=0x80000000;
 assert(g_snapshot()); gpu_m2(13);
 assert(gpu_extra[5][2]==0xb03 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10);
 for (unsigned b=2;b<g_ncb;b++) *ptr(GPU_SMMU_BASE+g_cb(b))=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb04 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); gpu_extra[1][51]=1u<<3;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb01 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); tlb_busy=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb06 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); corrupt_readback=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb07 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(11);
 *ptr(GPU_SMMU_BASE+g_cb(g_bank)+0x58)=0x400;
 *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=0x400;
 gpu_m2(13); assert(gpu_extra[5][4]==1); /* reserved FSR bit10 is evidence only */
 /* Failed stages are never retried and never unlock the XNU handoff. */
 for (unsigned mode=1;mode<=4;mode++) {
  reset_model(); aux_fixture(); gpu_m2(11); cp_mode=mode;
  unsigned begun=ticks; gpu_m2(13);
  assert(ticks-begun<100000);
  if (mode<=2) assert(gpu_extra[5][3]==(mode==1?0xc07:0xc06) && !gpu_extra[5][6]);
  else assert(gpu_extra[5][4]==(mode==3?0xd03:0xd05));
  before=mmio_writes; gpu_m2(13); assert(before==mmio_writes && gpu_gmu_started);
 }
 reset_model(); aux_fixture(); gpu_m2(11); abort_read=GPU_REG_BASE+0xf400*4;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc09 && gpu_extra[5][116] && !gpu_extra[5][6]);
 reset_model(); aux_fixture(); gpu_m2(10); abort_read=GPU_SMMU_BASE+SMMU_SMR(1);
 gpu_m2(13); assert(gpu_extra[5][2]==0xb01 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10);
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80000000;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb02 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(11); tlb_busy=0;
 *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=2;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc06 && !gpu_extra[5][6]);
 /* Dropped programming is diagnosed before any SQE execution/doorbell. */
 reset_model(); aux_fixture(); gpu_m2(11); drop_sqe_base=1;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc0b && gpu_extra[5][94]==1);
 assert(!gpu_extra[5][59] && !doorbells && gpu_extra[7][3]==1);
 /* Reproduce the device signature at SQE RUN with an empty ring. The
  * capture must distinguish this from CP_ME_INIT, which cannot run yet. */
 reset_model(); aux_fixture(); gpu_m2(11); start_fault=1;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc06 && gpu_extra[5][60]==7);
 assert(!doorbells && gpu_extra[7][3]==2 && gpu_extra[7][5]==0x80);
 assert(gpu_extra[7][6]==0x402 && gpu_extra[7][40]==0);
 assert(gpu_extra[6][57]==0x80 && gpu_extra[6][63]==0x402);
 assert(!gpu_extra[5][6] && gpu_extra[6][2]==0);
 /* Replay the old prefix in the causal model: CNTL3 at base0 latches the
  * fault before pointer verification, with no final RUN or doorbell. */
 reset_model(); aux_fixture(); gpu_m2(11);
 assert(!g3_sqe() && !g3_hold_gx());
 assert(g3_gw(G3_SQE_CNTL,3) && g3_hw_init() && !g3_verify_pointers());
 assert(g3_faulted() && gpu_extra[7][3]==1 && !gpu_extra[5][59] && !doorbells);
 assert(*ptr(GPU_REG_BASE+G3_SQE_BASE*4)==G3_SQE_VA);
 /* Never take over a live engine or start after an unreadable checkpoint. */
 reset_model(); aux_fixture(); gpu_m2(11); *ptr(GPU_REG_BASE+G3_SQE_CNTL*4)=1;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc0c && !sqe_controls && !doorbells);
 assert(gpu_extra[7][3]==4 && gpu_extra[5][119]==1);
 reset_model(); aux_fixture(); gpu_m2(11); abort_read=GPU_REG_BASE+G3_SQE_CNTL*4;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc05 && !sqe_controls && !doorbells);
 /* Diagnostic abort stops that window and keeps the failure frozen. */
 reset_model(); aux_fixture(); gpu_m2(11); abort_read=GPU_REG_BASE+G3_BV_STAT_DATA*4;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc05 && !gpu_extra[5][6]);
 assert(gpu_extra[9][2]==0 && gpu_extra[9][3]==51 && gpu_extra[9][4]);
 /* Pixel work is modeled separately from CP_MEM_WRITE. */
 reset_model(); aux_fixture(); panel_fixture(); gpu_m2(14);
 assert(gpu_extra[11][2]==1 && gpu_extra[11][21]==0 && gpu_extra[11][20]==gpu_extra[11][19]);
 assert(!gpu_extra[12][1] && gpu_extra[5][4]==1);
 assert(gpu_extra[11][28]==5 && gpu_extra[11][31]==82 && gpu_extra[11][54]==82 && gpu_extra[11][32]==156);
 assert(gpu_extra[11][44]==1 && !gpu_extra[11][56] && !gpu_extra[11][58]);
 assert(gpu_extra[11][40] && gpu_extra[11][43]==63);
 printf("M4b rings: clear 11..%u, copy %u..%u (%u words)\n",gpu_extra[11][54],gpu_extra[11][31],gpu_extra[11][32],gpu_extra[11][33]);
 assert(gpu_extra[11][38] && gpu_extra[11][37]==0xa740b014);
 uint32_t frozen14[128]; memcpy(frozen14,gpu_extra[11],sizeof(frozen14));
 before=doorbells; gpu_m2(15);
 assert(gpu_extra[12][2]==1 && gpu_extra[12][21]==0 && gpu_extra[12][47]==0);
 assert(!gpu_extra[12][26] && !gpu_extra[12][49] && !gpu_extra[12][45]);
 assert(gpu_extra[12][28]==10 && doorbells==before+1 && gpu_extra[12][32]==1679);
 printf("M4b stage15 ring: %u..%u (%u words)\n",gpu_extra[12][31],gpu_extra[12][32],gpu_extra[12][33]);
 assert(memcmp(frozen14,gpu_extra[11],sizeof(frozen14))==0);
 before=doorbells; gpu_m2(15); gpu_m2(14); assert(doorbells==before && gpu_gmu_started);
 FILE *m4=fopen("report-m4.bin","wb");
 assert(m4); gpu_report[0]=0xa740; fwrite(gpu_report,sizeof(gpu_report),1,m4);
 fwrite(gpu_extra,sizeof(gpu_extra),1,m4); fclose(m4);
 for(unsigned mode=1;mode<=6;mode++) {
  reset_model(); aux_fixture(); gpu_m2(13); blit_mode=mode; unsigned begun=ticks;
  gpu_m2(15);
  unsigned errors[]={0,0xe05,0xe06,0xe05,0xe02,0xe07,0xe06};
  assert(gpu_extra[11][2]==errors[mode] && !gpu_extra[12][1] && ticks-begun<100000);
  if(mode==2) assert(gpu_extra[11][21]==1 && gpu_extra[11][22]==3 && gpu_extra[11][23]==1);
  before=doorbells; gpu_m2(15); assert(doorbells==before && gpu_gmu_started);
 }
 /* Diagnose missing writes independently: source-free failure vs source-only
  * failure, and the two m4 omissions with CP still consuming/fencing. */
 for(unsigned mode=8;mode<=11;mode++) {
  reset_model(); aux_fixture(); gpu_m2(13); blit_mode=mode; gpu_m2(15);
  uint32_t *d=gpu_extra[11];
  assert(d[2]==(mode==8 ? 0xe0b : 0xe06) && !gpu_extra[12][1]);
  assert(d[43]==62 && d[55]==G4_CLEAR_MAGIC && d[37]==0xa740b014);
  assert(d[58]==(mode==9 ? 0 : 65536));
  assert(d[21]==(mode==8 ? 0 : 65536));
  assert(!d[40] && !d[67] && !d[71]);
 }
 reset_model(); aux_fixture(); gpu_m2(14); panel_fixture(); *ptr(0x0ae05024)=4352;
 before=doorbells; gpu_m2(15); assert(gpu_extra[12][2]==0xe08 && doorbells==before);
 reset_model(); aux_fixture(); gpu_m2(14); panel_fixture(); blit_mode=7;
 gpu_m2(15); assert(gpu_extra[12][2]==0xe07 && !gpu_extra[12][40]);
 reset_model(); aux_fixture(); gpu_m2(13); tlb_busy=1;
 gpu_m2(15); assert(gpu_extra[11][2]==0xe03 && !gpu_extra[12][1]);
 reset_model(); aux_fixture(); gpu_m2(13); abort_read=GPU_REG_BASE+0x806*4;
 gpu_m2(15); assert(gpu_extra[11][2]==0xe01 && !gpu_extra[12][1]);
 reset_model(); aux_fixture(); gpu_m2(13); abort_read=G4_DST_PA;
 gpu_m2(15); assert(gpu_extra[11][2]==0xe01 && gpu_extra[11][92] && !gpu_extra[12][1]);
 for(unsigned mode=1;mode<=4;mode+=2) {
  reset_model(); aux_fixture(); gpu_m2(14); panel_fixture(); blit_mode=mode;
  gpu_m2(15); assert(gpu_extra[12][2]==0xe05 && !gpu_extra[12][40]);
  before=doorbells; gpu_m2(15); assert(before==doorbells && gpu_gmu_started);
 }
 reset_model(); aux_fixture(); gpu_m2(13); abort_write=GPU_REG_BASE+G4_RB_NC*4;
 before=doorbells; gpu_m2(15); assert(gpu_extra[11][2]==0xe01 && before==doorbells);
 reset_model(); aux_fixture(); gpu_m2(14); panel_fixture(); abort_read=0x0ae05014;
 gpu_m2(15); assert(gpu_extra[12][2]==0xe01);
 reset_model(); aux_fixture(); gpu_m2(13); abort_read=GPU_REG_BASE+G4_CCU_CNTL*4;
 gpu_m2(15); assert(gpu_extra[11][2]==0xe01 && !gpu_extra[12][1]);
 reset_model(); aux_fixture(); gpu_m2(13); g3_wptr=8190;
 before=doorbells; gpu_m2(15); assert(gpu_extra[11][2]==0xe04 && before==doorbells);
 /* M5: exact metadata packing/relocation + full-word SCM returns and no retry. */
 reset_model(); aux_fixture(); require_zap=1; gpu_m2(16);
 uint32_t *z=gpu_extra[13];
 assert(z[2]==1 && z[3]==13 && z[4] && scm_calls==8 && z[5]==8);
 assert(z[7]==0xfcc && z[12]==0x1000 && z[13]==0x2000 && z[16]==0x1000);
 assert(z[17]==0x430 && !z[18] && z[19]==1 && !z[20]);
 assert(z[25]==11 && z[27]==13 && z[28]==13 && z[39]==g3_pkt7(0x66,1));
 assert(z[37]==1 && gpu_extra[11][54]==84 && gpu_extra[11][32]==158 && !gpu_extra[12][1]);
 assert(z[48+13]==0x12345678 && z[48+15]==0xfedcba98 && gpu_stage==16);
 uint32_t frozen16[256]; memcpy(frozen16,z,sizeof(frozen16));
 before=doorbells; gpu_m2(16); gpu_m2(14);
 assert(doorbells==before && scm_calls==8 && !memcmp(z,frozen16,sizeof(frozen16)));
 FILE *m5=fopen("report-m5.bin","wb"); assert(m5); gpu_report[0]=0xa740;
 fwrite(gpu_report,sizeof(gpu_report),1,m5); fwrite(gpu_extra,sizeof(gpu_extra),1,m5); fclose(m5);
 panel_fixture(); gpu_m2(15); assert(gpu_extra[12][2]==1 && gpu_extra[12][32]==1681);
 /* A fixed-address image skips mem_setup just as Linux does. The embedded
  * host fixture is writable; restore every altered header byte afterwards. */
 uint8_t saved_pa[4]; memcpy(saved_pa,_zap_fw_start+96,4);
 uint8_t saved_flags=_zap_fw_start[111]; _zap_fw_start[111]&=~8;
 for(unsigned i=0;i<4;i++) _zap_fw_start[96+i]=(G5_ZAP_PA>>(i*8))&255;
 reset_model(); aux_fixture(); gpu_m2(16); z=gpu_extra[13];
 assert(z[2]==1 && z[5]==6 && !z[14] && g5_scm_slot(z,5)[1]==0x42000205);
 memcpy(_zap_fw_start+96,saved_pa,4); _zap_fw_start[111]=saved_flags;
 reset_model(); assert(g5_copy(G5_ZAP_PA,0,32));
 for(unsigned i=0;i<8;i++) assert(!*ptr(G5_ZAP_PA+i*4));
 assert(*ptr(G5_ZAP_PA+32)==0x6b6b6b6b);
 /* Only a positively owned loaded image may use set_remote_state(0,13).
  * Stage/report replays above never take this path. */
 reset_model(); aux_fixture(); gpu_m2(13); g5_zap_loaded=1;
 struct g5_plan zp={0}; z=gpu_extra[13]; z[3]=3;
 assert(g5_pas(z,_zap_fw_start,&zp)==1 && z[38]==2 && scm_calls==1);
 assert(z[49]==0x4200010a && z[52]==0 && z[54]==13);
 /* False/unknown query results, including the observed support=0, are
  * advisory. Operational calls still execute and authenticate normally. */
 const uint64_t query_values[]={0,1,0x123456789abcdef0ull};
 for(unsigned i=0;i<3;i++) for(unsigned j=0;j<3;j++) {
  reset_model(); aux_fixture(); scm_available=query_values[i]; scm_supported=query_values[j];
  gpu_m2(16); z=gpu_extra[13];
  assert(z[2]==1 && z[4] && scm_calls==8 && !z[44]);
  for(unsigned k=0;k<8;k++) {
   uint32_t *slot=g5_scm_slot(z,k);
   uint64_t value=slot[10]|(uint64_t)slot[11]<<32;
   assert(slot[3] && !slot[8] && !slot[9]);
   if(k==1) assert(value==query_values[j]);
   else if(k==0 || k==2 || k==4 || k==6) assert(value==query_values[i]);
   else assert(!value);
  }
 }
 const int transport_errors[]={-1,-2,-3,-4,-5,-12,1,2};
 for(unsigned failed=1;failed<=8;failed++) for(unsigned kind=0;kind<=8;kind++) {
  /* x1 is only a status on init/setup/auth, not on boolean queries. */
  if(!kind && failed!=4 && failed!=6 && failed!=8) continue;
  reset_model(); aux_fixture(); scm_fail_call=failed;
  scm_transport=kind ? (uint64_t)(int64_t)transport_errors[kind-1] : 0;
  scm_service_fail=!kind;
  gpu_m2(16); z=gpu_extra[13];
  const unsigned errors[]={0,0xf05,0xf06,0xf05,0xf07,0xf05,0xf08,0xf05,0xf0a};
  assert(z[2]==errors[failed] && scm_calls==failed && z[44]==failed);
  uint32_t *slot=g5_scm_slot(z,failed-1);
  assert(slot[3] && (slot[8]|(uint64_t)slot[9]<<32)==scm_transport);
  if(!kind) assert((slot[10]|(uint64_t)slot[11]<<32)==(uint64_t)-7);
  assert(!z[4] && !z[43] && !gpu_extra[11][1] && !gpu_extra[12][1]);
  uint32_t frozen[256]; memcpy(frozen,z,sizeof(frozen));
  before=doorbells; gpu_m2(16); gpu_m2(15); gpu_m2(14);
  assert(scm_calls==failed && doorbells==before && !gpu_extra[11][1]);
  assert(!memcmp(frozen,z,sizeof(frozen)));
 }
 for(unsigned mode=1;mode<=3;mode++) {
  reset_model(); aux_fixture(); secure_mode=mode; gpu_m2(16); z=gpu_extra[13];
  unsigned errors[]={0,0xf0d,0xf0c,0xf0e};
  assert(z[2]==errors[mode] && z[4] && z[43] && !gpu_extra[11][1]);
  before=doorbells; gpu_m2(15); gpu_m2(16); assert(before==doorbells && scm_calls==8);
 }
 reset_model(); aux_fixture(); gpu_m2(14); before=doorbells; gpu_m2(16);
 assert(gpu_extra[13][2]==0xf0f && !scm_calls && before==doorbells);
 reset_model(); aux_fixture(); gpu_m2(13); abort_write=G5_META_PA; gpu_m2(16);
 assert(gpu_extra[13][2]==0xf09 && scm_calls==3 && !gpu_extra[11][1]);
 reset_model(); aux_fixture(); gpu_m2(13); abort_write=G5_ZAP_PA; gpu_m2(16);
 assert(gpu_extra[13][2]==0xf09 && scm_calls==6 && !gpu_extra[13][4]);
 reset_model(); aux_fixture(); gpu_m2(13); abort_read=G5_META_PA; gpu_m2(16);
 assert(gpu_extra[13][2]==0xf09 && scm_calls==3);
 reset_model(); aux_fixture(); gpu_m2(13); blit_mode=9; gpu_m2(16);
 assert(gpu_extra[13][2]==0xe06 && gpu_extra[13][37]==0xe06 && !gpu_extra[12][1]);
 before=doorbells; gpu_m2(15); assert(before==doorbells && !gpu_extra[12][1]);
 reset_model(); aux_fixture(); gpu_m2(13); test_zap_bytes=0; gpu_m2(16);
 assert(gpu_extra[13][2]==0xf02 && !scm_calls && !gpu_extra[11][1]);
 before=doorbells; gpu_m2(15); gpu_m2(16); assert(before==doorbells && !scm_calls);
 /* Untrusted ELF bounds/relocation tests operate on an isolated local copy. */
 uint8_t mbn[0x10000]; size_t zb=(uintptr_t)_zap_fw_end-(uintptr_t)_zap_fw_start;
 memcpy(mbn,_zap_fw_start,zb); assert(!g5_plan_mbn(mbn,zb,&zp));
 assert(g5_plan_mbn(mbn,51,&zp)==0xf03);
 mbn[44]=255; assert(g5_plan_mbn(mbn,zb,&zp)==0xf03); memcpy(mbn,_zap_fw_start,zb);
 mbn[52+32+16]=0xff; mbn[52+32+17]=0xff;
 assert(g5_plan_mbn(mbn,zb,&zp)==0xf04); memcpy(mbn,_zap_fw_start,zb);
 mbn[52+32+27]&=~8; assert(g5_plan_mbn(mbn,zb,&zp)==0xf04); memcpy(mbn,_zap_fw_start,zb);
 mbn[52+64+24]=0; mbn[52+64+27]=0; assert(g5_plan_mbn(mbn,zb,&zp)==0xf04);
 puts("PASS: M5b advisory queries, retained contiguous metadata/relocation, ABI, all SCM errors/no-retry, packet/idle/trust/2D gates and frozen reports");
 puts("PASS: M4 pixels/row pitches/fences/guards/faults/no-retry; SMMU snapshot/ownership/readback, all icache PTEs, firmware load, HFI order/ACK/wrap/errors/timeouts, m3 mappings/CP packets/fence/abort/timeouts, one-shot GMU/handoff latch");
 return 0;
}
'''


def audit_linux(msm):
    """Compare the hand-copied tables/critical offsets to the supplied source."""
    xml = (msm / 'registers/adreno/a6xx.xml').read_text()
    catalog = (msm / 'adreno/a6xx_catalog.c').read_text()
    header = (ROOT / 'shim/fuxi-probe/gpu-a740.h').read_text()
    offsets = {name: int(off, 16) for off, name in re.findall(
        r'<reg(?:32|64) offset="(0x[0-9a-fA-F]+)" name="([^"]+)"', xml)}
    def body(text, name):
        return re.search(r'\b' + name + r'\[.*?=\s*\{(.*?)\n\};', text, re.S)[1]
    local = [(int(off, 16), int(val, 16)) for off, val in re.findall(
        r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', body(header, 'g3_hwcg'))]
    linux = [(offsets[name], int(val, 16)) for name, val in re.findall(
        r'\{ REG_A[67]XX_(\w+), (0x[0-9a-f]+) \}', body(catalog, 'a740_hwcg'))]
    assert len(local) == 54 and local == linux, 'A740 HWCG offset/value drift'
    protect = re.sub(r'/\*.*?\*/', '', body(catalog, 'a730_protect_regs'), flags=re.S)
    expected = []
    for line in protect.splitlines():
        match = re.search(r'A6XX_PROTECT_(RDONLY|NORDWR)\((0x[0-9a-f]+), (0x[0-9a-f]+)\)', line)
        if match:
            mode, base, count = match.groups()
            expected.append(int(base, 16) | int(count, 16) << 18 |
                            (0x80000000 if mode == 'NORDWR' else 0))
        elif line.strip() == '0,':
            expected.append(0)
    local_protect = re.sub(r'/\*.*?\*/', '', body(header, 'g3_protect'), flags=re.S)
    actual = [int(x.strip(), 0) for x in local_protect.split(',') if x.strip()]
    assert len(actual) == 48 and actual == expected, 'A740 protection table drift'
    names = {'G3_RB_BASE': 'CP_RB_BASE', 'G3_RB_CNTL': 'CP_RB_CNTL',
             'G3_RPTR_ADDR': 'CP_RB_RPTR_ADDR', 'G3_SQE_CNTL': 'CP_SQE_CNTL',
             'G3_SQE_BASE': 'CP_SQE_INSTR_BASE', 'G3_APRIV': 'CP_APRIV_CNTL',
             'G3_STAT_ADDR': 'CP_SQE_STAT_ADDR', 'G3_STAT_DATA': 'CP_SQE_STAT_DATA',
             'G3_APERTURE_HOST': 'CP_APERTURE_CNTL_HOST',
             'G3_BV_STAT_ADDR': 'CP_BV_SQE_STAT_ADDR', 'G3_BV_STAT_DATA': 'CP_BV_SQE_STAT_DATA',
             'G3_BV_RPTR_ADDR': 'CP_BV_RB_RPTR_ADDR',
             'G3_AC_STAT_ADDR': 'CP_SQE_AC_STAT_ADDR', 'G3_AC_STAT_DATA': 'CP_SQE_AC_STAT_DATA',
             'G3_LPAC_CNTL': 'CP_LPAC_SQE_CNTL',
             'G3_MISC': 'CP_MISC_CNTL', 'G3_CHICKEN': 'CP_CHICKEN_DBG',
             'G3_BV_UCODE_ADDR': 'CP_BV_SQE_UCODE_DBG_ADDR', 'G3_BV_UCODE_DATA': 'CP_BV_SQE_UCODE_DBG_DATA',
             'G3_AC_UCODE_ADDR': 'CP_SQE_AC_UCODE_DBG_ADDR', 'G3_AC_UCODE_DATA': 'CP_SQE_AC_UCODE_DBG_DATA'}
    names.update({'G4_GRAS_BLIT': 'GRAS_2D_BLIT_CNTL', 'G4_GRAS_SRC_X': 'GRAS_2D_SRC_TL_X',
                  'G4_GRAS_NC': 'GRAS_NC_MODE_CNTL', 'G4_CCU_CNTL2': 'RB_CCU_CNTL2',
                  'G4_RB_BLIT': 'RB_2D_BLIT_CNTL', 'G4_RB_PIXEL': 'RB_2D_UNKNOWN_8C01',
                  'G4_RB_DST_INFO': 'RB_2D_DST_INFO', 'G4_CCU_CNTL': 'RB_CCU_CNTL',
                  'G4_RB_NC': 'RB_NC_MODE_CNTL', 'G4_SP_FORMAT': 'SP_2D_DST_FORMAT',
                  'G4_SP_NC': 'SP_NC_MODE_CNTL', 'G4_SP_SRC_INFO': 'SP_PS_2D_SRC_INFO',
                  'G4_SP_WINDOW': 'SP_PS_2D_WINDOW_OFFSET', 'G4_TPL1_BLIT': 'SP_PS_UNKNOWN_B2D2',
                  'G4_TPL1_NC': 'TPL1_NC_MODE_CNTL', 'G4_RB_SOLID': 'RB_2D_SRC_SOLID_C0',
                  'G4_RB_DBG': 'RB_DBG_ECO_CNTL', 'G4_RB_CMP_DBG': 'RB_CMP_DBG_ECO_CNTL'})
    for macro, name in names.items():
        value = int(re.search(r'^#define ' + macro + r'\s+(0x[0-9a-f]+)', header, re.M)[1], 16)
        assert value == offsets[name], f'{macro}: Linux {name} offset drift'
    driver = (msm / 'adreno/a6xx_gpu.c').read_text()
    cold = driver.split('static int hw_init(struct msm_gpu *gpu)', 1)[1].split('static int a6xx_hw_init', 1)[0]
    controls = re.findall(r'gpu_write\(gpu, REG_A6XX_CP_SQE_CNTL, (\d+)\)', cold)
    assert controls == ['1'] and cold.index('REG_A6XX_CP_SQE_INSTR_BASE') < cold.index('REG_A6XX_CP_SQE_CNTL')
    assert 'REG_A6XX_RBBM_SW_RESET_CMD' not in cold
    firmware = (msm / 'adreno/adreno_gpu.c').read_text().split('struct drm_gem_object *adreno_fw_create_bo', 1)[1].split('int adreno_hw_init', 1)[0]
    assert 'memcpy(ptr, &fw->data[4], fw->size - 4)' in firmware
    assert 'MSM_BO_WC | MSM_BO_GPU_READONLY' in firmware
    print('PASS: Linux source audit: 54 HWCG pairs, 48 protection slots, 39 CP/2D offsets; cold SQE order and firmware copy')


def audit_linux_pas(msm):
    kernel = msm.parents[3]
    defs = (kernel / 'drivers/firmware/qcom/qcom_scm.h').read_text()
    expected = {'SVC_PIL': 2, 'PIL_PAS_INIT_IMAGE': 1, 'PIL_PAS_MEM_SETUP': 2,
                'PIL_PAS_AUTH_AND_RESET': 5, 'PIL_PAS_IS_SUPPORTED': 7,
                'SVC_BOOT': 1, 'BOOT_SET_REMOTE_STATE': 10,
                'SVC_INFO': 6, 'INFO_IS_CALL_AVAIL': 1}
    for name, value in expected.items():
        assert int(re.search(r'#define QCOM_SCM_' + name + r'\s+(0x[0-9a-f]+)', defs)[1], 16) == value
    scm = (kernel / 'drivers/firmware/qcom/qcom_scm.c').read_text()
    assert 'QCOM_SCM_ARGS(2, QCOM_SCM_VAL, QCOM_SCM_RW)' in scm
    xml = (msm / 'registers/adreno/adreno_pm4.xml').read_text()
    assert re.search(r'name="CP_SET_SECURE_MODE" value="0x66"', xml)
    dts = (kernel / 'arch/arm64/boot/dts/qcom/sm8550.dtsi').read_text()
    assert re.search(r'gpu_micro_code_mem:.*?reg = <0 0x9b09a000 0 0x2000>;.*?no-map;', dts, re.S)
    mdt = (kernel / 'drivers/soc/qcom/mdt_loader.c').read_text()
    valid = mdt.split('static bool mdt_phdr_valid', 1)[1].split('static ', 1)[0]
    assert 'p_type != PT_LOAD' in valid and 'QCOM_MDT_TYPE_HASH' in valid and '!phdr->p_memsz' in valid
    init = mdt.split('int qcom_mdt_pas_init(', 1)[1].split('EXPORT_SYMBOL_GPL', 1)[0]
    assert 'phdr->p_flags & QCOM_MDT_RELOCATABLE' in init
    assert 'min_addr = phdr->p_paddr' in init
    assert 'max_addr = ALIGN(phdr->p_paddr + phdr->p_memsz, SZ_4K)' in init
    assert re.search(r'if \(relocate\)\s*\{\s*ret = qcom_scm_pas_mem_setup\(pas_id, mem_phys, max_addr - min_addr\)', init)
    assert init.index('qcom_scm_pas_init_image') < init.index('qcom_scm_pas_mem_setup')
    pas = scm.split('int qcom_scm_pas_init_image(', 1)[1].split('EXPORT_SYMBOL_GPL', 1)[0]
    assert 'dma_alloc_coherent' in pas and 'desc.args[1] = mdata_phys' in pas
    assert 'physically contiguous, 4K aligned' in pas and 'ctx->ptr = mdata_buf' in pas
    assert 'desc.args[2]' not in pas
    zap = (msm / 'adreno/adreno_gpu.c').read_text().split('static int zap_shader_load_mdt(', 1)[1].split('int adreno_zap_shader_load', 1)[0]
    assert 'qcom_scm_pas_supported' not in zap
    assert zap.index('qcom_mdt_load(') < zap.index('qcom_scm_pas_auth_and_reset(')
    print('PASS: Linux PAS IDs/ABI, advisory zap support, MDT relocation/setup arguments and contiguous metadata context')


def audit_mesa(mesa):
    """Independent A7xx bit-field/event checks against the supplied Mesa XML."""
    import xml.etree.ElementTree as ET
    xml = mesa / 'src/freedreno/registers/adreno'
    regs = ET.parse(xml / 'a6xx.xml').getroot()
    pm4 = ET.parse(xml / 'adreno_pm4.xml').getroot()

    def named(root, tag, name):
        return [e for e in root.iter() if e.tag.rsplit('}', 1)[-1] == tag and e.get('name') == name]

    def enum(root, name):
        return {int(e.get('value'), 0) for e in named(root, 'value', name)}

    def field(root, tag, name, bit, low, high):
        for e in named(root, tag, name):
            for f in e.iter():
                if f.get('name') == bit:
                    a = int(f.get('pos', f.get('low', '-1')))
                    b = int(f.get('pos', f.get('high', '-1')))
                    if (a, b) == (low, high):
                        return
        raise AssertionError((name, bit, low, high))

    assert enum(pm4, 'CP_SET_THREAD_BR') == {1}
    assert enum(pm4, 'CCU_RESOLVE_CLEAN') == {26}
    assert enum(pm4, 'CCU_CLEAN_COLOR') == {33}
    assert enum(pm4, 'CACHE_CLEAN') == {49}
    assert enum(regs, 'FMT6_8_8_8_8_UNORM') == {0x30}
    assert enum(regs, 'R2D_UNORM8') == {0x10}
    assert enum(regs, 'CCU_CACHE_SIZE_FULL') == {0}
    field(pm4, 'domain', 'CP_THREAD_CONTROL', 'THREAD', 0, 1)
    field(pm4, 'domain', 'CP_THREAD_CONTROL', 'CONCURRENT_BIN_DISABLE', 27, 27)
    field(pm4, 'domain', 'CP_EVENT_WRITE7', 'WRITE_ENABLED', 27, 27)
    for name, low, high in [('SOLID_COLOR', 7, 7), ('COLOR_FORMAT', 8, 15), ('MASK', 20, 23), ('IFMT', 24, 28)]:
        field(regs, 'bitset', 'a6xx_2d_blit_cntl', name, low, high)
    field(regs, 'reg32', 'RB_CCU_CNTL2', 'COLOR_OFFSET', 23, 31)
    # A7xx pitch is bytes<<3 (A6xx's shr6/low9 yields the same aligned value).
    field(regs, 'reg32', 'SP_PS_2D_SRC_PITCH', 'PITCH', 3, 23)
    assert 1024 << 3 == (1024 >> 6) << 9 == 8192
    assert (0x180000 >> 12) << 23 == 0xc0000000
    print('PASS: Mesa A7xx thread/event/2D format, solid-color, pitch and CCU fields')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zap', type=Path, help='external local single-file a740_zap.mbn')
    ap.add_argument('--sqe', type=Path, help='external local a740_sqe.fw')
    ap.add_argument('--firmware', type=Path, help='external local gmu_gen70200.bin; never copied into git')
    ap.add_argument('--linux-msm', type=Path, help='supplied drivers/gpu/drm/msm source directory for register/table audit')
    ap.add_argument('--mesa', type=Path, help='local Mesa source for A7xx field/event audit')
    args = ap.parse_args()
    usb = (ROOT / 'shim/fuxi-probe/probe.c').read_text()
    assert 'index >= 1 && index <= 16' in usb
    assert 'index < 0x100 + G_PAGES' in usb
    assert 'monitor_report[MON_ACTIVE] && !gpu_gmu_started' in usb
    if args.mesa:
        audit_mesa(args.mesa)
    if args.linux_msm:
        audit_linux(args.linux_msm)
        audit_linux_pas(args.linux_msm)
    source = (ROOT / 'shim/fuxi-probe/gpu.c').read_text()
    source = source.replace('#include \"gpu-zap.h\"', (ROOT / 'shim/fuxi-probe/gpu-zap.h').read_text())
    source, raw_count = re.subn(r'static __attribute__\(\(noinline\)\) void g5_smc_raw\(.*?\n\}', 'static void g5_smc_raw(uint64_t ret[4], uint32_t fn, uint32_t info, uint64_t a, uint64_t b, uint64_t c) { mock_smc(ret, fn, info, a, b, c); }', source, flags=re.S)
    assert raw_count == 1
    source = source.replace('extern const uint8_t _gpu_fw_start[], _gpu_fw_end[];',
                            'extern uint8_t _gpu_fw_start[], _gpu_fw_end[];')
    source = source.replace('extern const uint8_t _sqe_fw_start[], _sqe_fw_end[];',
                            'extern uint8_t _sqe_fw_start[], _sqe_fw_end[];')
    source = source.replace('extern const uint8_t _zap_fw_start[], _zap_fw_end[];', 'extern uint8_t _zap_fw_start[], _zap_fw_end[];')
    source = source.replace('size_t bytes = (uintptr_t)_zap_fw_end-(uintptr_t)_zap_fw_start;', 'size_t bytes = test_zap_bytes;')
    source = re.sub(r'__asm__ volatile\([^;]+;', '', source)
    needle = '*(volatile uint32_t *)G_PTR_RANGE(base + off,4) = v;'
    assert source.count(needle) == 1
    source = source.replace(needle, needle + '\n mock_write(base + off, v);')
    with tempfile.TemporaryDirectory(prefix='fuxi-gpu-test-') as tmp:
        tmp = Path(tmp)
        fw = args.firmware.resolve() if args.firmware else tmp / 'fixture.bin'
        if not args.firmware:
            import struct
            fw.write_bytes(struct.pack('<4I', 0x10007fe0, 0, 1, 32) +
                           struct.pack('<4I', 0x10007fe0, 32, 0, 0) +
                           struct.pack('<I', 0x20010005) + b'\0'*28)
        sqe = args.sqe.resolve() if args.sqe else tmp / 'sqe.bin'
        if not args.sqe:
            sqe.write_bytes(bytes.fromhex('000000000102030405060708'))
        zap = args.zap.resolve() if args.zap else tmp / 'zap.mbn'
        if not args.zap:
            import struct
            b = bytearray(12088)
            struct.pack_into('<16sHHIIIIIHHHHHH', b, 0, b'\x7fELF\x01\x01\x01'+b'\0'*9, 2, 164, 1, 0x1000, 52, 0, 3, 52, 32, 3, 0, 0, 0)
            for i, ph in enumerate(((0, 0, 0, 0, 0x94, 0, 0x07000000, 0),
                                     (1, 0x1000, 0x1000, 0x1000, 0x430, 0x430, 0x08000007, 0x100000),
                                     (0, 0x2000, 0, 0, 0xf38, 0xf38, 0x02000000, 0x1000))):
                struct.pack_into('<8I', b, 52+i*32, *ph)
            b[0x1000:0x1430] = bytes(i & 255 for i in range(0x430))
            b[0x2000:] = b'\x5a'*0xf38
            zap.write_bytes(b)
        asm = tmp / 'fw.S'
        import json
        asm.write_text('.section .data\n.global _gpu_fw_start, _gpu_fw_end\n_gpu_fw_start:\n.incbin ' +
                       json.dumps(str(fw)) + '\n_gpu_fw_end:\n.global _sqe_fw_start, _sqe_fw_end\n_sqe_fw_start:\n.incbin ' +
                       json.dumps(str(sqe)) + '\n_sqe_fw_end:\n.global _zap_fw_start, _zap_fw_end\n_zap_fw_start:\n.incbin ' + json.dumps(str(zap)) + '\n_zap_fw_end:\n.section .note.GNU-stack,"",@progbits\n')
        cfile = tmp / 'test.c'
        cfile.write_text(PREFIX + source + SUFFIX)
        exe = tmp / 'test'
        subprocess.run(['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'shim/fuxi-probe'), '-DFUXI_XNU_LOADER=1', '-DEXPECT_BLOCKS=' + ('4' if args.firmware else '1'),
                        '-DEXPECT_PREALLOC=' + ('11' if args.firmware else '1'),
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cfile), str(asm), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, cwd=tmp)
        report = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(tmp / 'report.bin')],
                                check=True, capture_output=True, text=True).stdout
        assert 'SQE_STAT count=51' in report and 'BR=0xa7400000/a7400001' in report
        assert '70c80004 0000004b' in report and 'SQElo: before_RUN=0x40000000' in report
        assert 'OOB/pre-setup: SQE_CNTL=0x0 SQEbase=0000000000000000' in report
        assert 'configured/pre-RUN: SQE_CNTL=0x0 SQEbase=0000000040000000' in report
        assert 'immediate/post-RUN: SQE_CNTL=0x1' in report
        assert 'BV SQE_STAT count=51' in report and 'LPAC SQE_STAT count=51' in report
        import runpy
        decoder = runpy.run_path(str(ROOT / 'tools/fuxi-gpu-probe.py'))
        client = decoder['fault_client']
        assert client(0, 0) == 'CP' and client(3, 9) == 'BV_SP' and client(8, 7) == 'LPAC_TP'
        assert decoder['bit_names'](0x80, decoder['CP_BITS']) == 'BR_ILLEGAL_INSTR'
        m4report = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(tmp / 'report-m4.bin')],
                                  check=True, capture_output=True, text=True).stdout
        assert 'stage 14 A7xx 2D blit' in m4report and 'stage 15 A7xx 2D blit' in m4report
        assert 'mismatch=0' in m4report and 'CPU_mismatch=0' in m4report
        assert 'solid_clear=0x1 (PASS)' in m4report and 'validity=0x3f' in m4report
        assert 'CCU_CNTL2=' in m4report and 'copy after 100us' in m4report
        m5report = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(tmp / 'report-m5.bin')],
                                  check=True, capture_output=True, text=True).stdout
        assert 'stage 16 PAS13' in m5report and 'authenticated=1' in m5report
        assert 'fn=0x42000201 arginfo=0x82 returned=1' in m5report
        assert 'x2=0x123456789abcdef0 x3=0xfedcba9876543210' in m5report
        assert 'target=13 after=13/13' in m5report and 'TRUST before/after=0x1/0x0' in m5report
        assert 'SCM[8] step=8 fn=0x42000205' in m5report and 'calls=8' in m5report
        assert 'arg2(span)=0x1000' in m5report and m5report.count('advisory query boolean') == 5
        # v7 has fourteen pages; retain its clear validity and timing decoding.
        import struct
        old7 = bytearray((tmp / 'report-m4.bin').read_bytes()[:7168])
        for page in (1, 6, 8, 9, 10, 11, 12, 13):
            struct.pack_into('<I', old7, page*512, 0xa7400007)
        legacy7 = tmp / 'v7.bin'; legacy7.write_bytes(old7)
        decoded7 = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(legacy7)],
                                  check=True, capture_output=True, text=True).stdout
        assert 'solid_clear=0x1 (PASS)' in decoded7 and 'validity=0x3f' in decoded7
        blob = (tmp / 'report-m5.bin').read_bytes()
        assert len(blob) == 8192
        words = [list(struct.unpack('<128I', blob[i:i+512])) for i in range(0, len(blob), 512)]
        # Legacy v8 keeps its five original slots and failure explanations.
        old8 = [p[:] for p in words[:15]]
        for page in old8[1:]:
            if page[0] == 0xa7400009:
                page[0] = 0xa7400008
        slots = [words[14][48+i*16:64+i*16] if i<5 else words[15][16+(i-5)*16:32+(i-5)*16] for i in range(8)]
        old8[14][48:] = sum([slots[i] for i in (0, 1, 3, 5, 7)], [])
        old8[14][5] = 5
        legacy8 = tmp / 'v8.bin'; legacy8.write_bytes(b''.join(struct.pack('<128I', *p) for p in old8))
        decoded8 = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(legacy8)],
                                  check=True, capture_output=True, text=True).stdout
        assert 'SCM[5] step=8 fn=0x42000205' in decoded8 and 'advisory query' not in decoded8
        old8[14][2] = 0xf06
        assert 'PAS13 unsupported or SCM error' in decoder['decode'](old8[0], old8[1:])
        # Failed/in-flight slots on both sides of the page boundary retain
        # high return halves; an incomplete v9 capture reports missing slots.
        for failed in range(8):
            z = words[14]; z[2] = 0xf07; z[5] = failed+1; z[3] = 5
            slot = words[14] if failed<5 else words[15]
            off = 48+failed*16 if failed<5 else 16+(failed-5)*16
            slot[off+3] = 0
            text = decoder['decode'](words[0], words[1:])
            assert 'PAS init_image SCM error' in text and 'in flight (no returned registers)' in text
            slot[off+3] = 1; slot[off+8] = 0xfffffff4; slot[off+9] = 0xffffffff
            assert 'x0=0xfffffffffffffff4' in decoder['decode'](words[0], words[1:])
        assert 'SCM records missing: captured=5 issued=8' in decoder['decode'](words[0], words[1:15])
        # Reproduce v6's failed-stage14 reporting: zero CPU ticks mean unrun.
        old6 = bytearray((tmp / 'report-m4.bin').read_bytes())
        import struct
        struct.pack_into('<I', old6, 512, 0xa7400006)
        struct.pack_into('<I', old6, 12*512, 0xa7400006)
        struct.pack_into('<II', old6, 12*512+2*4, 0xe06, 6)
        struct.pack_into('<II', old6, 12*512+40*4, 0, 0)
        words = [list(struct.unpack('<128I', old6[i:i+512])) for i in range(0, len(old6), 512)]
        text = decoder['decode'](words[0], words[1:])
        assert 'CPU=N/A (not run)' in text and 'CPU=0.000 us' not in text
        # v5's unchanged prefix must still decode as a 12-page saved report.
        legacy = bytearray((tmp / 'report.bin').read_bytes()[:6144])
        struct.pack_into('<I', legacy, 512, 0xa7400005)
        old = tmp / 'v5.bin'; old.write_bytes(legacy)
        subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(old)], check=True, stdout=subprocess.DEVNULL)
        print('PASS: sixteen-page M5b, legacy M5/v8, M4/v7/v6 and v5 offline Python decoder')


if __name__ == '__main__':
    main()
