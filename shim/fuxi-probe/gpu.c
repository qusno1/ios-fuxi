/* Fuxi A740 staged GPU/GMU probe (lane-gpu, milestones 1–5).
 *
 * Single translation unit #included by probe.c before ud_setup(), so these
 * statics share probe.c's helpers (delay(), the volatile memcpy) and the USB
 * report path. Request 0x56 runs a gated, bisectable sequence and returns a
 * fixed 128-word report. Nothing here is reached unless the host asks for it,
 * and every stage runs only if the previous one proved its prerequisite, so a
 * single boot records the whole chain. New MMIO reads recover synchronous
 * aborts; powered-off interconnect hangs/SError can still reset the SoC.
 *
 * The loader runs with the MMU and caches OFF and VA==PA, so every "window"
 * below is just a physical base; accesses are naturally uncached Device-nGnRE
 * which is what the GPU/GMU bring-up wants. All register maths is ported from
 * the pinned Linux msm/adreno + qcom clk drivers (ianchb/sm8550-mainline
 * 42f3b40c) and cross-checked against UniXNU-sheng's GpuPowerBoard.h; every
 * address carries its Fuxi-verified source. Facts vs. guesses: see
 * docs-lanes/gpu.md.
 */

/* Platform hooks preserve the loader's identity mapping by default. XNU
 * supplies mapped addresses, fault-contained MMIO and staged firmware. */
#ifndef G_PTR
#define G_PTR(pa) ((uintptr_t)(pa))
#endif
#ifndef G_PTR_RANGE
#define G_PTR_RANGE(pa, bytes) G_PTR(pa)
#endif
#ifndef G_RING_INDEX
#define G_RING_INDEX(w) (w)
#endif
#include "gpu-clock.h"
/* XNU sets this once before cold start. Loader probes retain zero. */
static unsigned g_gpu_selected;
static uint64_t g_gpu_ddr_requested;
/* ---- Fuxi-verified physical bases (research/device/probe-09/live.dtb) ---- */
#define GPU_GCC_BASE      0x00100000ULL   /* gcc kalama-v2 @100000 (clock-controller) */
#define GPU_GCC_GPU_PAGE  (GPU_GCC_BASE + 0x71000) /* gcc-kalama: gpu branch page */
#define GPU_GCC_VOTE_PAGE (GPU_GCC_BASE + 0x52000) /* gcc gpll0 GPU vote register */
#define GPU_GPUCC_BASE    0x03d90000ULL   /* gpucc @3d90000 size 0xa000 (qcom,kalama-gpucc) */
#define GPU_SMMU_BASE     0x03da0000ULL   /* kgsl-smmu@3da0000 size 0x40000 (adreno-smmu) */
#define GPU_REG_BASE      0x03d00000ULL   /* kgsl-3d0@3d00000 size 0x40000 */
#define GPU_GMU_CORE      0x03d6a000ULL   /* GMU core (upstream/sheng base; Fuxi node gmu@3d68000/0x37000 contains it) */
#define GPU_RSCC_BASE     0x03d50000ULL   /* gmu rscc */
#define GPU_CXMISC_BASE   0x03d9e000ULL   /* gpu cx_misc / cx_mem */
#define GPU_CMDDB_BASE    0x81c60000ULL   /* reserved-memory aop_cmd_db_region@81c60000 reg .. 0x20000 (no-map) */
#define GPU_CMDDB_BYTES   0x20000ULL

/* GCC GPU branch offsets within the 0x71000 page (gcc-sm8550.c / gcc-kalama.c). */
#define GCC_GPU_CFG_AHB      0x004   /* gcc_gpu_cfg_ahb_clk (critical; hwcg bit1 -> CLK_OFF unreliable, use enable bit0) */
#define GCC_GPU_MEMNOC_GFX   0x010   /* gcc_gpu_memnoc_gfx_clk (voted) */
#define GCC_GPU_SNOC_DVM_GFX 0x018
#define GCC_DDRSS_GPU_AXI    0x154   /* gcc_ddrss_gpu_axi_clk 0x71154 */
#define CBCR_CLK_OFF (1u << 31)
#define CBCR_ENABLE  (1u << 0)
#define GPLL0_VOTES  ((1u << 15) | (1u << 16))   /* gcc_gpu_gpll0_clk_src / _div_clk_src in the 0x52000 page */

/* GPUCC offsets (gpucc-sm8550.c, confirmed against the Fuxi DTB node layout). */
#define CC_PLL0        0x0000
#define CC_PLL1        0x1000
#define PLL_MODE       0x0
#define PLL_OPMODE     0x4
#define PLL_L_VAL      0x10
#define PLL_USER_CTL   0x18
#define PLL_LOCK_DET   (1u << 31)
#define PLL_OUTCTRL    (1u << 0)
#define PLL_RESET_N    (1u << 2)
#define PLL_OUT_MASK   0x7u
#define PLL_RUN        1u
#define CC_CX_GDSCR    0x9108   /* gpu_cc_cx_gdsc.gdscr */
#define CC_GX_GDSCR    0x905c   /* gpu_cc_gx_gdsc.gdscr */
#define CC_CX_GDS_HW   0x953c   /* gpu_cc_cx_gdsc.gds_hw_ctrl (VOTABLE: poll PWR_ON here) */
#define CC_AHB_CLK     0x911c   /* gpu_cc_ahb_clk */
#define CC_CX_GMU_CLK  0x913c   /* gpu_cc_cx_gmu_clk */
#define CC_SMMU_VOTE   0x7000   /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define CC_CXO_AON     0x9004   /* gpu_cc_cxo_aon_clk */
#define CC_DEMET       0x900c   /* gpu_cc_demet_clk */
#define CC_CXO_CLK     0x9144
#define CC_HUB_CX_INT  0x9148
#define CC_GMU_RCGR    0x9318   /* gpu_cc_gmu_clk_src cmd_rcgr; CFG at +4 */
#define CC_HUB_RCGR    0x93ec   /* gpu_cc_hub_clk_src */
#define GDSC_PWR_ON       (1u << 31)
#define GDSC_SW_COLLAPSE  (1u << 0)
#define GDSC_RETAIN_MEM   (1u << 14)
#define GDSC_RETAIN_PERIPH (1u << 13)
#define GDSC_RETAIN_FF    (1u << 11)
#define RCG_UPDATE   (1u << 0)
#define RCG_ROOT_EN  (1u << 1)
#define RCG_ROOT_OFF (1u << 31)
#define RCG_CFG_CLEAR (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))

/* ARM SMMUv2 global space 0 (IHI 0062D). adreno_smmu: qsmmu-v500 + adreno-smmu. */
#define SMMU_SCR0  0x0
#define SMMU_ID0   0x20
#define SMMU_ID1   0x24
#define SMMU_ID2   0x28
#define SMMU_GFSR  0x48
#define SMMU_SMR(n)  (0x800 + 4 * (n))
#define SMMU_S2CR(n) (0xc00 + 4 * (n))

/* GPU + GMU register dword offsets (a6xx.xml / a6xx_gmu.xml, A7XX variants).
 * The GMU core block sits SHENG_GMU_CORE_GPU_DELTA=0x6a000 bytes above GPU reg
 * base, so GMU dword D is absolute GPU_GMU_CORE + (D*4 - 0x6a000). */
#define GMU_DWORD(d) (GPU_GMU_CORE + ((uint64_t)(d) * 4 - 0x6a000))
#define GMUD_CM3_SYSRESET   0x1f800
#define GMUD_HFI_CTRL_STATUS 0x1f980
#define GMUD_SPTPRAC_STATUS 0x1f8d0   /* GMU_SPTPRAC_PWR_CLK_STATUS; see stage10 mask below */
#define RSCC_SEQ_BUSY_DRV0  0x101     /* rscc dword */
#define CXMISC_TCM_RET_CNTL 0x39      /* cx_misc dword */
#define GPU_RBBM_STATUS     0x210     /* gpu reg dword */

/* ---- report pages (each 128 u32 = the 512-byte control-IN cap) ---- */
#define GPU_REPORT_WORDS 128
static uint32_t gpu_report[GPU_REPORT_WORDS];
static unsigned gpu_stage;   /* furthest stage entered, for a post-mortem photo */
static void g_enter(unsigned stage) { gpu_stage = stage; gpu_report[2] = stage;
#ifdef FUXI_GPU_XNU
 printf("FUXI GPU: stage %u\n", stage);
#endif
}

static uint32_t g_rd(uint64_t base, uint32_t off)
{
 #ifdef FUXI_GPU_XNU
 uint32_t f, v = probe_safe_rd32(base + off, &f);
#else
 uint32_t v = *(volatile uint32_t *)G_PTR_RANGE(base + off,4);
#endif
 __asm__ volatile("dmb oshld" ::: "memory");
 return v;
}
static void g_wr(uint64_t base, uint32_t off, uint32_t v)
{
 __asm__ volatile("dmb oshst" ::: "memory");
 #ifdef FUXI_GPU_XNU
 (void)probe_safe_wr32(base + off, v);
#else
 *(volatile uint32_t *)G_PTR_RANGE(base + off,4) = v;
#endif
 __asm__ volatile("dsb osh" ::: "memory");
}
static int g_clk_on(uint32_t cbcr) { return (cbcr & CBCR_CLK_OFF) == 0; }
static int g_gdsc_on(uint32_t gdscr) { return (gdscr & GDSC_PWR_ON) != 0; }

/* lucid_ole config (gpucc-sm8550.c): write L/alpha and the fixed config words,
 * leave OUTCTRL clear, OPMODE STANDBY, assert RESET_N. Zero values are skipped.
 * `pll` is the PLL's offset within the GPUCC window (CC_PLL0 / CC_PLL1). */
static void g_pll_configure(uint32_t pll, uint32_t l, uint32_t alpha)
{
 static const uint32_t off[] = {0x10,0x14,0x20,0x24,0x28,0x18,0x1c,0x2c,0x30,0x34,0x38};
 const uint32_t val[] = {l | 0x44u<<16 | 0x44u<<24, alpha, 0x20485699u, 0x00182261u,
                         0x82aa299cu, 0, 5, 0, 3, 0x9000, 0x34};
 for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++)
  if (val[i]) g_wr(GPU_GPUCC_BASE, pll + off[i], val[i]);
 uint32_t mode = g_rd(GPU_GPUCC_BASE, pll + PLL_MODE);
 g_wr(GPU_GPUCC_BASE, pll + PLL_MODE, mode & ~PLL_OUTCTRL);
 g_wr(GPU_GPUCC_BASE, pll + PLL_OPMODE, 0);
 mode = g_rd(GPU_GPUCC_BASE, pll + PLL_MODE);
 g_wr(GPU_GPUCC_BASE, pll + PLL_MODE, mode | PLL_RESET_N);
}
/* clk_rcg2_shared_force_enable_clear: ROOT_EN, wait ROOT_OFF clear, CFG, UPDATE. 0 ok / 1 timeout. */
static int g_rcg_set(uint32_t rcgr, uint32_t sel, uint32_t div)
{
 uint32_t cmd = g_rd(GPU_GPUCC_BASE, rcgr);
 g_wr(GPU_GPUCC_BASE, rcgr, cmd | RCG_ROOT_EN);
 unsigned i;
 for (i = 0; i < 500; i++) { if (!(g_rd(GPU_GPUCC_BASE, rcgr) & RCG_ROOT_OFF)) break; delay(1); }
 if (i == 500) return 1;
 uint32_t cfg = g_rd(GPU_GPUCC_BASE, rcgr + 4);
 g_wr(GPU_GPUCC_BASE, rcgr + 4, (cfg & ~RCG_CFG_CLEAR) | sel << 8 | div);
 g_wr(GPU_GPUCC_BASE, rcgr, g_rd(GPU_GPUCC_BASE, rcgr) | RCG_UPDATE);
 for (i = 0; i < 500; i++) { if (!(g_rd(GPU_GPUCC_BASE, rcgr) & RCG_UPDATE)) break; delay(1); }
 if (i == 500) return 1;
 g_wr(GPU_GPUCC_BASE, rcgr, g_rd(GPU_GPUCC_BASE, rcgr) & ~RCG_ROOT_EN);
 return 0;
}
static void g_set_bit0(uint64_t base, uint32_t off)
{
 uint32_t v = g_rd(base, off);
 if (!(v & CBCR_ENABLE)) g_wr(base, off, v | CBCR_ENABLE);
}

/* little-endian cmd-db accessors over the already-copied snapshot */
static uint16_t cdb16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t cdb32(const uint8_t *p) { return cdb16(p) | (uint32_t)cdb16(p + 2) << 16; }

/* The staged sequence. `limit` (USB wIndex) is the last stage allowed;
 * the report records the actual outcome of each stage it reached. */
/* Milestone 2: Linux 6.12 a6xx_gmu.c/a6xx_hfi.c, ARM SMMUv2.
 * Fixed scratch in the loader-only XNU RAM window. Never part of .bss.
 * Firmware and tables must survive until reset once CM3 has been released.
 */
#define G_MEM       0x9fe000000ULL
#define G_MEM_SIZE  0x02000000u
#define G_IC_PA     (G_MEM + 0x100000)
#define G_DATA_PA   (G_MEM + 0x1100000)
#define G_DUMMY_VA  0x60000000u
#define G_LOG_VA    0x60002000u
#define G_HFI_VA    0x60006000u
#define G_DEBUG_VA  0x60400000u
#define G_LOG_PA    (G_DATA_PA + 0x2000)
#define G_HFI_PA    (G_DATA_PA + 0x6000)
#define G_DEBUG_PA  (G_DATA_PA + 0xa000)
#define G_IC_SIZE   0xffc000u
#define G_PAGES     16
#define G_STAGE_8A  0x8au
static uint32_t gpu_extra[G_PAGES - 1][GPU_REPORT_WORDS];
static unsigned gpu_gmu_started; /* monotonic: also covers failed init/ACK */
static unsigned gpu_m2_attempted, gpu_s8_ok, gpu_s9_attempted, gpu_s9_ok, gpu_s10_attempted;
static unsigned g_bank, g_slot, g_asid, g_stride, g_cbbase, g_ncb, g_nsmr, g_pt_next;
static unsigned gpu_snapshot_valid;
static uint8_t gpu_cmddb[GPU_CMDDB_BYTES] __attribute__((aligned(8)));

/* All new GMU offsets are relative to the upstream GMU core resource,
 * unlike the GPU-relative dword offsets in milestone 1. */
static uint32_t gm_rd(unsigned reg) { return g_rd(GPU_GMU_CORE, reg * 4); }
static void gm_wr(unsigned reg, uint32_t val) { g_wr(GPU_GMU_CORE, reg * 4, val); }
static void g_wr64(uint64_t at, uint64_t val)
{
 __asm__ volatile("dmb oshst" ::: "memory");
 #ifdef FUXI_GPU_XNU
 fg_store64(at, val);
#else
 *(volatile uint64_t *)G_PTR_RANGE(at,8) = val;
#endif
 __asm__ volatile("dsb sy" ::: "memory");
}
static void g_zero(uint64_t at, unsigned bytes)
{
 volatile uint64_t *p = (volatile uint64_t *)G_PTR_RANGE(at,bytes);
 for (unsigned i = 0; i < bytes / 8; i++) p[i] = 0;
 __asm__ volatile("dsb sy" ::: "memory");
}
static unsigned g_cb(unsigned b) { return g_cbbase + b * g_stride; }
static void g_live(void)
{
 uint32_t *e = gpu_extra[0];
 e[4] = gpu_gmu_started;
 e[84] = gm_rd(0x5192); e[85] = gm_rd(0x9305); e[86] = gm_rd(0x9313);
 e[87] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
 if (gpu_m2_attempted && e[15]) {
  unsigned cb = g_cb(g_bank);
  e[88] = g_rd(GPU_SMMU_BASE, cb + 0x58);
  e[89] = g_rd(GPU_SMMU_BASE, cb + 0x60);
  e[90] = g_rd(GPU_SMMU_BASE, cb + 0x64);
  e[91] = g_rd(GPU_SMMU_BASE, cb + 0x68);
  e[92] = g_rd(GPU_SMMU_BASE, cb + 0x6c);
 }
 if (gpu_s8_ok) {
  e[93] = g_rd(G_HFI_PA, 24 + 40); e[94] = g_rd(G_HFI_PA, 24 + 44);
  e[95] = g_rd(G_HFI_PA, 72 + 40); e[96] = g_rd(G_HFI_PA, 72 + 44);
 }
}
uint32_t probe_safe_rd32(uint64_t pa, uint32_t *faulted);
extern uint32_t probe_fault_esr; extern uint64_t probe_fault_far;
/* Snapshot read that survives a synchronous abort (hypervisor/TZ-owned
 * windows). page1[48] fault count, [49] first ESR, [50] first offset,
 * [51] SMR/S2CR fault mask, [52] per-bank CB fault mask. */
static uint32_t g_srd(uint32_t off, uint32_t bit, unsigned word)
{
 uint32_t f, v = probe_safe_rd32(GPU_SMMU_BASE + off, &f);
 if (f) {
  uint32_t *p = gpu_extra[1];
  if (!p[48]++) { p[49] = probe_fault_esr; p[50] = off; }
  p[word] |= bit;
 }
 return v;
}
static int g_snapshot(void)
{
 uint32_t *e = gpu_extra[0];
 for (unsigned i = 48; i < 53; i++) gpu_extra[1][i] = 0;
 uint32_t id0 = g_rd(GPU_SMMU_BASE, SMMU_ID0), id1 = g_rd(GPU_SMMU_BASE, SMMU_ID1);
 uint32_t id2 = g_rd(GPU_SMMU_BASE, SMMU_ID2);
 g_stride = (id1 >> 31) ? 65536 : 4096;
 g_cbbase = (2u << ((id1 >> 28) & 7)) * g_stride;
 g_ncb = id1 & 255; g_nsmr = id0 & 255;
 e[5] = g_nsmr; e[6] = g_ncb; e[7] = g_stride; e[8] = g_cbbase;
 /* Bound every access to Fuxi's 256K resource and report capacity. */
 if (!g_ncb || g_ncb > 24 || !g_nsmr || g_nsmr > 32 ||
     g_cbbase >= 0x40000 || g_ncb * g_stride > 0x40000 - g_cbbase ||
     !(id0 & (1u << 30)) || !(id0 & (1u << 27)) ||
     !(id2 & (1u << 12)) || ((id2 >> 4) & 15) < 1 ||
     (g_rd(GPU_SMMU_BASE, SMMU_SCR0) & 1)) { e[2] = 0x801; return 0; }
 for (unsigned i = 0; i < g_nsmr; i++) {
  gpu_extra[1][i * 2] = g_srd(SMMU_SMR(i), 1u << i, 51);
  gpu_extra[1][i * 2 + 1] = g_srd(SMMU_S2CR(i), 1u << i, 51);
 }
 for (unsigned b = 0; b < g_ncb; b++) {
  uint32_t *d = &gpu_extra[2 + b / 8][(b % 8) * 16];
  unsigned cb = g_cb(b);
  /* Spare words in the stream page retain implementation context controls. */
  gpu_extra[1][64 + b] = g_srd(cb + 4, 1u << b, 52); /* ACTLR */
  gpu_extra[1][88 + b] = g_srd(cb + 0x34, 1u << b, 52); /* CONTEXTIDR */
  d[0] = g_srd(g_stride + b * 4, 1u << b, 52);
  d[1] = g_srd(g_stride + 0x800 + b * 4, 1u << b, 52);
  static const unsigned offs[14] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c,0x58,0x60,0x64,0x68,0x6c};
  for (unsigned i = 0; i < 14; i++) d[2 + i] = g_srd(cb + offs[i], 1u << b, 52);
 }
 e[9] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
 gpu_snapshot_valid = 1; e[1] = 1;
 return 1;
}
static uint64_t g_new_table_at(uint64_t root, unsigned *next)
{
 if (*next >= 256) return 0;
 return root + 4096 * (*next)++;
}
/* 32-bit IOVA, 4K granules: L1 root -> L2 -> L3 pages. All leafs NC,
 * inner-shareable (MAIR index 0 = 0x44), AF=1. Privileged fixed regions
 * match MSM_BO_MAP_PRIV; log/HFI permit unprivileged GMU accesses. */
static int g_map_at(uint64_t root, unsigned *next, unsigned va, uint64_t pa, unsigned size, unsigned attr)
{
 for (unsigned off = 0; off < size; off += 4096) {
  unsigned v = va + off;
  volatile uint64_t *l1 = (volatile uint64_t *)G_PTR_RANGE(root,4096);
  unsigned a = v >> 30;
  if (!l1[a]) { uint64_t t = g_new_table_at(root, next); if (!t) return 0; l1[a] = t | 3; }
  volatile uint64_t *l2 = (volatile uint64_t *)G_PTR_RANGE(l1[a] & ~0xfffull,4096);
  unsigned b = (v >> 21) & 511;
  if (!l2[b]) { uint64_t t = g_new_table_at(root, next); if (!t) return 0; l2[b] = t | 3; }
  volatile uint64_t *l3 = (volatile uint64_t *)G_PTR_RANGE(l2[b] & ~0xfffull,4096);
  unsigned c = (v >> 12) & 511;
  if (l3[c]) return 0;
  l3[c] = (pa + off) | 3 | (3u << 8) | (1u << 10) | attr;
 }
 __asm__ volatile("dsb sy" ::: "memory");
 return 1;
}
static int g_map(unsigned va, uint64_t pa, unsigned size, unsigned unpriv)
{ return g_map_at(G_MEM, &g_pt_next, va, pa, size, unpriv ? (1u << 6) : 0); }
static int g_range(unsigned addr, unsigned size, unsigned base, unsigned bytes)
{ return addr >= base && addr - base < bytes && size <= bytes - (addr - base); }
/* Return a physical destination, or zero for an unmapped address. */
static uint64_t g_fw_dest(unsigned addr, unsigned size)
{
 if (g_range(addr, size, 0, 0x4000)) return GPU_GMU_CORE + 0xc00 * 4 + addr;
 if (g_range(addr, size, 0x10004000, 0x4000)) return GPU_GMU_CORE + 0x1c00 * 4 + addr - 0x10004000;
 if (g_range(addr, size, 0x4000, G_IC_SIZE)) return G_IC_PA + addr - 0x4000;
 if (g_range(addr, size, G_DUMMY_VA, 0x2000)) return G_DATA_PA + addr - G_DUMMY_VA;
 return 0;
}
static int g_firmware(unsigned load)
{
#if defined(FUXI_GPU_XNU)
 unsigned bytes_unused;
 const uint8_t *fw = fg_firmware(0, &bytes_unused);
 size_t bytes = bytes_unused;
#elif defined(FUXI_XNU_LOADER)
 extern const uint8_t _gpu_fw_start[], _gpu_fw_end[];
 const uint8_t *fw = _gpu_fw_start;
 size_t bytes = (uintptr_t)_gpu_fw_end - (uintptr_t)_gpu_fw_start;
#else
 const uint8_t *fw = 0; size_t bytes = 0;
#endif
 uint32_t *e = gpu_extra[0]; e[17] = bytes;
 if (!bytes) { e[2] = 0x802; return 0; }
 size_t at = 0; unsigned blocks = 0, prealloc = 0, total = 0;
 while (at < bytes) {
  if (bytes - at < 16) { e[2] = 0x803; return 0; }
  unsigned addr = cdb32(fw + at), size = cdb32(fw + at + 4);
  unsigned extent = size ? size : cdb32(fw + at + 12);
  uint64_t dst = g_fw_dest(addr, extent);
  if ((addr & 3) || (size & 3) || size > bytes - at - 16 || !dst || !extent) {
   e[2] = 0x803; e[21] = addr; e[22] = size; return 0;
  }
  if (size) {
   blocks++; total += size;
   if (load) for (unsigned i = 0; i < size; i += 4) {
    uint32_t word = cdb32(fw + at + 16 + i);
    g_wr(dst, i, word);
    if (g_rd(dst, i) != word) { e[2] = 0x80a; e[21] = addr + i; return 0; }
   }
  } else prealloc++;
  at += 16 + size;
 }
 e[18] = blocks; e[19] = prealloc; e[20] = total;
 if (!blocks) { e[2] = 0x803; return 0; }
 return 1;
}
static int g_select_context(void)
{
 uint32_t *e = gpu_extra[0]; unsigned used = 0;
 g_slot = g_nsmr;
 for (unsigned i = 0; i < g_nsmr; i++) {
  unsigned smr = gpu_extra[1][2*i], s2cr = gpu_extra[1][2*i+1];
  if (gpu_extra[1][51] & (1u << i)) continue; /* unreadable: neither free nor ours */
  if (!(smr & (1u << 31))) { if (g_slot == g_nsmr) g_slot = i; continue; }
  unsigned mask = (smr >> 16) & 0x7fff;
  if (((smr ^ 5) & ~mask & 0x7fff) == 0) { e[2] = 0x804; return 0; }
  if (((s2cr >> 16) & 3) == 0) {
   unsigned bank = s2cr & 255;
   if (bank < g_ncb) used |= 1u << bank;
  }
 }
 g_bank = g_ncb;
 unsigned s2banks = (g_rd(GPU_SMMU_BASE, SMMU_ID1) >> 16) & 255;
 for (unsigned b = s2banks ? s2banks : 1; b < g_ncb; b++)
  if (!(used & (1u << b)) && !(gpu_extra[1][52] & (1u << b)) &&
      !(gpu_extra[2+b/8][b%8*16+2] & 1)) { g_bank = b; break; }
 if (g_slot == g_nsmr || g_bank == g_ncb) { e[2] = 0x805; return 0; }
 /* ASIDs are shared across banks; avoid every enabled bank's two TTBR ASIDs. */
 for (g_asid = 1; g_asid < 256; g_asid++) {
  int used_asid = 0;
  for (unsigned b = 0; b < g_ncb; b++) {
   uint32_t *d = &gpu_extra[2+b/8][b%8*16];
   if ((gpu_extra[1][52] & (1u << b)) == 0 && (d[2] & 1) && ((d[5] >> 16) == g_asid || (d[7] >> 16) == g_asid)) used_asid = 1;
  }
  if (!used_asid) break;
 }
 if (g_asid == 256) { e[2] = 0x805; return 0; }
 e[10] = g_slot; e[11] = g_bank; e[12] = g_asid;
 return 1;
}
static int g_program_context(void)
{
 uint32_t *e = gpu_extra[0]; unsigned cb = g_cb(g_bank);
 /* An unused, disabled bank only; never alter GR0 or unrelated streams. */
 if (g_rd(GPU_SMMU_BASE, cb) & 1) { e[2] = 0x805; return 0; }
 uint64_t ttbr = G_MEM | ((uint64_t)g_asid << 48);
 g_wr(GPU_SMMU_BASE, g_stride + 0x800 + g_bank * 4, 1);
 g_wr(GPU_SMMU_BASE, g_stride + g_bank * 4, 0x1f300);
 /* 36-bit PA, AArch64 ASID, TTBR0=32-bit IOVA, TTBR1 disabled.
  * Table walks are NC/outer-shareable, coherent with uncached CPU writes. */
 g_wr(GPU_SMMU_BASE, cb + 0x10, 0x38011);
 g_wr(GPU_SMMU_BASE, cb + 0x30, 0x80802020);
 g_wr64(GPU_SMMU_BASE + cb + 0x20, ttbr);
 g_wr64(GPU_SMMU_BASE + cb + 0x28, (uint64_t)g_asid << 48);
 g_wr(GPU_SMMU_BASE, cb + 0x38, 0xff44);
 g_wr(GPU_SMMU_BASE, cb + 0x3c, 0);
 g_wr(GPU_SMMU_BASE, cb + 0x58, 0xffffffff);
 e[15] = 1;
 g_wr(GPU_SMMU_BASE, cb + 0x610, g_asid);
 g_wr(GPU_SMMU_BASE, cb + 0x7f0, 0);
 unsigned i;
 for (i = 0; i < 1000; i++) {
  e[16] = g_rd(GPU_SMMU_BASE, cb + 0x7f4);
  if (!(e[16] & 1)) break;
  delay(1);
 }
 if (i == 1000) { e[2] = 0x808; return 0; }
 e[23] = g_rd(GPU_SMMU_BASE, g_stride + g_bank * 4);
 e[24] = g_rd(GPU_SMMU_BASE, g_stride + 0x800 + g_bank * 4);
 e[25] = g_rd(GPU_SMMU_BASE, cb + 0x10); e[26] = g_rd(GPU_SMMU_BASE, cb + 0x30);
 e[27] = g_rd(GPU_SMMU_BASE, cb + 0x20); e[28] = g_rd(GPU_SMMU_BASE, cb + 0x24);
 e[29] = g_rd(GPU_SMMU_BASE, cb + 0x38);
 if (e[23] != 0x1f300 || e[24] != 1 || (e[25] & ~0x60u) != 0x38011 ||
     e[26] != 0x80802020 || e[27] != (uint32_t)ttbr || e[28] != (uint32_t)(ttbr >> 32) ||
     e[29] != 0xff44 || g_rd(GPU_SMMU_BASE, cb + 0x28) ||
     g_rd(GPU_SMMU_BASE, cb + 0x2c) != (g_asid << 16) || g_rd(GPU_SMMU_BASE, cb + 0x3c)) {
  e[2] = 0x809; return 0;
 }
 g_wr(GPU_SMMU_BASE, cb, 0x1067); /* faults reported, no interrupt controller needed */
 e[30] = g_rd(GPU_SMMU_BASE, cb);
 if ((e[30] & ~0x1000u) != 0x67) { e[2] = 0x809; return 0; }
 g_wr(GPU_SMMU_BASE, SMMU_S2CR(g_slot), g_bank); /* translate, PRIVCFG=default */
 g_wr(GPU_SMMU_BASE, SMMU_SMR(g_slot), 0x80000005);
 e[31] = g_rd(GPU_SMMU_BASE, SMMU_SMR(g_slot));
 e[32] = g_rd(GPU_SMMU_BASE, SMMU_S2CR(g_slot));
 if (e[31] != 0x80000005 || e[32] != g_bank) { e[2] = 0x809; return 0; }
 return 1;
}
static int g_memory_load(void)
{
 uint32_t *e = gpu_extra[0];
 if (gm_rd(0x5000) != 1 || g_rd(GPU_CXMISC_BASE, 0x39*4) ||
     g_rd(GPU_RSCC_BASE, 0x101*4) || (gm_rd(0x5180) & 1)) { e[2] = 0x806; return 0; }
 if (!g_firmware(0) || !g_select_context()) return 0;
#ifndef FUXI_GPU_XNU
 extern char _head[], _end[];
 uint64_t image = (uintptr_t)_head, end = (uintptr_t)_end;
 uint64_t fdt = probe_fdt;
 if ((image < G_MEM + G_MEM_SIZE && G_MEM < end) ||
     (fdt < G_MEM + G_MEM_SIZE && G_MEM < fdt + probe_fdt_size)) { e[2] = 0x807; return 0; }
 #endif
 g_zero(G_MEM, 0x100000); g_zero(G_IC_PA, G_IC_SIZE); g_zero(G_DATA_PA, 0x11000);
 g_pt_next = 1;
 if (!g_map(0x4000, G_IC_PA, G_IC_SIZE, 0) || !g_map(G_DUMMY_VA, G_DATA_PA, 0x2000, 0) ||
     !g_map(G_LOG_VA, G_LOG_PA, 0x4000, 1) || !g_map(G_HFI_VA, G_HFI_PA, 0x4000, 1) ||
     !g_map(G_DEBUG_VA, G_DEBUG_PA, 0x7000, 0)) { e[2] = 0x80b; return 0; }
 e[13] = (uint32_t)G_MEM; e[14] = G_MEM >> 32; e[33] = g_pt_next;
 /* Linux fw_start's FAL veto + retention precede the TCM firmware writes. */
 gm_wr(0x50f1, 1); gm_wr(0x50f0, 1); g_wr(GPU_CXMISC_BASE, 0x39*4, 1);
 if (!g_program_context() || !g_firmware(1)) return 0;
 if (g_rd(GPU_SMMU_BASE, g_cb(g_bank) + 0x58) & 0xc00001feu) { e[2] = 0x80c; return 0; }
 e[2] = 1; gpu_s8_ok = 1;
 return 1;
}
/* Linux has two queues, not three: command id0 and response id1. */
static void g_hfi_init(void)
{
 g_zero(G_HFI_PA, 0x4000);
 static const uint32_t table[6] = {0,120,6,12,2,2}; /* size is bytes in Linux implementation */
 for (unsigned i = 0; i < 6; i++) g_wr(G_HFI_PA, i*4, table[i]);
 for (unsigned q = 0; q < 2; q++) {
  unsigned h = 24 + q*48;
  g_wr(G_HFI_PA, h, 1); g_wr(G_HFI_PA, h+4, G_HFI_VA + (q+1)*4096);
  g_wr(G_HFI_PA, h+8, (10u<<8)|q); g_wr(G_HFI_PA, h+12, 1024);
  g_wr(G_HFI_PA, h+24, 1); g_wr(G_HFI_PA, h+28, 1); g_wr(G_HFI_PA, h+32, 1);
 }
}
/* Return cmd-db aux data only after checking both the header and data extents. */
static const uint8_t *g_aux(const char *name, unsigned *count)
{
 for (unsigned s = 0; s < 8; s++) {
  const uint8_t *h = gpu_cmddb + 8 + s*16;
  unsigned n = cdb16(h+6), at = 144 + cdb16(h+2), data = 144 + cdb16(h+4);
  if (!cdb16(h)) continue;
  if (at > GPU_CMDDB_BYTES || n > (GPU_CMDDB_BYTES-at)/24) return 0;
  for (unsigned i = 0; i < n; i++) {
   const uint8_t *p = gpu_cmddb + at + i*24;
   unsigned j; for (j = 0; j < 8; j++) if (p[j] != (uint8_t)name[j]) break;
   if (j != 8) continue;
   unsigned len = cdb16(p+20), off = data + cdb16(p+22);
   if (!len || (len & 1) || off > GPU_CMDDB_BYTES || len > GPU_CMDDB_BYTES-off || len > 512) return 0;
   *count = len/2; return gpu_cmddb + off;
  }
 }
 return 0;
}
static int g_arc_vote(const char *primary, const char *secondary, unsigned level, uint32_t *vote)
{
 unsigned np = 0, ns = 0;
 const uint8_t *p = g_aux(primary, &np), *s = g_aux(secondary, &ns);
 if (!p || !s) return 0;
 unsigned pi, si = 0;
 for (pi = 0; pi < np; pi++) if (cdb16(p+pi*2) >= level) break;
 if (pi == np) return 0;
 for (unsigned j = 0; j < ns; j++) {
  if (cdb16(s+j*2) >= level) { si = j; break; }
  if (cdb16(s+j*2)) si = j;
 }
 *vote = (uint32_t)cdb16(p+pi*2)<<16 | si<<8 | pi;
 return 1;
}
/* Opt-in only: require unique, well-formed BCM records from the boot's own
 * command DB. Reuse no rail aux interpretation or fixed resource addresses. */
static int g_active_bw(uint32_t bw[160])
{
 static const char names[4][8] = {"SH0", "MC0", "ACV", "CN0"};
 struct g_bcm bcm[3] = {{0}};
 uint32_t address[4] = {0};
 for (unsigned s = 0; s < 8; s++) {
  const uint8_t *h = gpu_cmddb+8+s*16;
  unsigned slave = cdb16(h), n = cdb16(h+6), at = 144+cdb16(h+2), data = 144+cdb16(h+4);
  if (!slave) break;
  if (at > GPU_CMDDB_BYTES || n > (GPU_CMDDB_BYTES-at)/24 || data > GPU_CMDDB_BYTES) return 0;
  for (unsigned i = 0; i < n; i++) {
   const uint8_t *p = gpu_cmddb+at+i*24;
   for (unsigned j = 0; j < 4; j++) {
    unsigned k; for (k = 0; k < 8; k++) if (p[k] != (uint8_t)names[j][k]) break;
    if (k != 8) continue;
    unsigned len = cdb16(p+20), off = data+cdb16(p+22);
    uint32_t addr = cdb32(p+16);
    if (address[j] || slave != 5 || !addr || (addr & 3) || ((addr>>16)&7) != slave ||
        off > GPU_CMDDB_BYTES || len > GPU_CMDDB_BYTES-off || (j < 3 && len != 8)) return 0;
    address[j] = addr;
    if (j < 3) {
     const uint8_t *aux = gpu_cmddb+off;
     if (aux[7]) return 0;
     bcm[j] = (struct g_bcm){addr,cdb32(aux),cdb16(aux+4),aux[6]};
    }
   }
  }
 }
 return g_gpu_bw_packet(bcm, address[3], g_gpu_ddr_row2(g_gpu_selected, g_gpu_ddr_requested), bw);
}
/* e[40..59]: five command/ACK records {sent, response, returned, error}.
 * Poll shared queue indices as well as interrupt status; reject malformed or
 * unsolicited packets within one bounded deadline (no unbounded drain loop). */
static int g_hfi_send(unsigned id, uint32_t *msg, unsigned words, unsigned record)
{
 uint32_t *e = gpu_extra[0], *rec = e + 40 + record*4;
 unsigned seq = record + 1, count = (words+3)&~3u;
 unsigned rd = g_rd(G_HFI_PA, 64), wr = g_rd(G_HFI_PA, 68);
 rec[0] = msg[0] = seq<<20 | words<<8 | id;
 if (words > 255 || rd >= 1024 || wr >= 1024 || (wr & 3) || ((rd-wr-1)&1023) < count) return 0x904;
 for (unsigned i = 0; i < count; i++) g_wr(G_HFI_PA+4096, ((wr+i)&1023)*4, i < words ? msg[i] : 0xfafafafa);
 g_wr(G_HFI_PA, 68, (wr+count)&1023);
 gm_wr(0x5194, 1);
 uint64_t until = rsr("cntpct_el0") + rsr("cntfrq_el0") / 20; /* 50ms total */
 do {
  unsigned intr = gm_rd(0x5192);
  if (intr & (1u<<23)) return 0x905;
  rd = g_rd(G_HFI_PA, 112); wr = g_rd(G_HFI_PA, 116);
  if (rd >= 1024 || wr >= 1024 || (rd & 3) || (wr & 3)) return 0x904;
  if (rd == wr) { delay(10); continue; }
  __asm__ volatile("dmb oshld" ::: "memory");
  unsigned hdr = g_rd(G_HFI_PA+8192, rd*4), n = (hdr>>8)&255;
  rec[1] = hdr;
  if (n < 3 || n > 19 || ((n+3)&~3u) > ((wr-rd)&1023)) return 0x904;
  rec[2] = g_rd(G_HFI_PA+8192, ((rd+1)&1023)*4);
  rec[3] = g_rd(G_HFI_PA+8192, ((rd+2)&1023)*4);
  g_wr(G_HFI_PA, 112, (rd+((n+3)&~3u))&1023);
  gm_wr(0x5191, 1);
  if ((hdr&255) == 100) return 0x905;
  /* gen70200 GMU echoes the command id in the ACK header (observed
   * 0x00010304 for PERF_TABLE); older firmware uses id 126. Linux only
   * matches ret_header, so accept either id with type ACK. */
  if (((hdr&255) != 126 && (hdr&255) != id) || ((hdr>>16)&15) != 1 || rec[2] != rec[0]) return 0x906;
  if (rec[3]) return 0x907;
  return 0;
 } while (rsr("cntpct_el0") < until);
 return 0x903;
}
static void g_rpmh_init(void)
{
 static const unsigned regs[] = {4,0xb,0xe,0xd,0x10,0xf,0x12,0x11,0x100,8,9,0xa,0x154,0x155,0x156,0x157,0x158};
 static const uint32_t vals[] = {1u<<24,1,0,0,0,0,0x80000021,0,0,0x4520,0x4510,0x4514,
  0xeaaae5a0,0xe1a1ebab,0xa2e0a581,0xecac82e2,0x0020edad};
 for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) g_wr(GPU_RSCC_BASE, regs[i]*4, vals[i]);
 /* A7xx PDC uCode is AOP-owned: only enable/configure, never replace it. */
 g_wr(0x0b280000, 0x1148*4, 0); g_wr(0x0b280000, 0x1140*4, 0x80000001);
}
static int g_poll(unsigned reg, uint32_t mask, uint32_t value, unsigned word)
{
 uint32_t *e = gpu_extra[0];
 for (unsigned i = 0; i < 1000; i++) {
  e[word] = gm_rd(reg);
  if ((e[word]&mask) == value) return 1;
  if (gm_rd(0x5192) & (1u<<23)) return 0;
  delay(10);
 }
 return 0;
}
static int g_start(void)
{
 uint32_t *e = gpu_extra[0], perf[59], bw[160];
 /* Volatile stores avoid compiler-emitted libc memset in a freestanding TU. */
 for (unsigned i = 0; i < 59; i++) ((volatile uint32_t *)perf)[i] = 0;
 for (unsigned i = 0; i < 160; i++) ((volatile uint32_t *)bw)[i] = 0;
 /* Ascending Linux frequency indices, including OFF at index 0. kHz. */
 static const uint32_t cfreq[3] = {0,200000,500000};
 static const unsigned clevel[3] = {0,0x40,0x80};
 static const char gfx[8] = "gfx.lvl", mx[8] = "mx.lvl", cx[8] = "cx.lvl", gmxc[8] = "gmxc.lvl";
 unsigned unused;
 const char *secondary = g_aux(gmxc, &unused) ? gmxc : mx;
 perf[1] = 10; perf[2] = 3;
 for (unsigned i = 0; i < 10; i++) {
  if (!g_arc_vote(gfx, secondary, g_gpu_level[i], &perf[3+i*3])) { e[3] = 0x908; return 0; }
  perf[4+i*3] = 0xffffffff; perf[5+i*3] = g_gpu_freq[i]; e[100+i] = perf[3+i*3];
 }
 for (unsigned i = 0; i < 3; i++) {
  if (!g_arc_vote(cx, mx, clevel[i], &perf[51+i*2])) { e[3] = 0x908; return 0; }
  perf[52+i*2] = cfreq[i]; e[110+i] = perf[51+i*2];
 }
 /* Full Linux bw struct is 160 words: 6 scalar + 6 + 12 + 8 + 128. */
 bw[1] = 1; bw[2] = 1; bw[3] = 3; bw[4] = 1; bw[5] = 7;
 bw[6] = gpu_report[90]; bw[12] = 0x40000000; bw[18] = 0x60000001;
 for (unsigned i = 0; i < 3; i++) {
  bw[24+i] = gpu_report[87+i]; bw[32+i] = 0x40000000;
  if (!bw[24+i]) { e[3] = 0x908; return 0; }
 }
 if (!bw[6]) { e[3] = 0x908; return 0; }
 g_gpu_selected = g_gpu_select(g_gpu_selected);
 if (g_gpu_selected && !g_active_bw(bw)) { e[3] = 0x90a; return 0; }
 if (gm_rd(0x5000) != 1 || g_rd(GPU_RSCC_BASE, 0x101*4) || (gm_rd(0x5180)&1)) { e[3] = 0x909; return 0; }
 g_hfi_init(); g_rpmh_init();
 gm_wr(0x9304, 0xffffffff); gm_wr(0x9306, 0xffffffff);
 gm_wr(0x5191, 0xffffffff); gm_wr(0x5193, 0xffffffff); /* polled, no GIC IRQ */
 gm_wr(0x501c, 0); gm_wr(0x5001, 2);
 gm_wr(0x5185, G_HFI_VA); gm_wr(0x5184, 1);
 gm_wr(0x9311, 0x80000000u | (0x32u<<18) | 0x8a0);
 gm_wr(0x502d, 0x4052); gm_wr(0x51cf, 0x7020100); gm_wr(0x51cd, G_LOG_VA | 3);
 gm_wr(0x4c0f, 1); gm_wr(0x4c00, 1); gm_wr(0x4c01, 1);
 e[34] = gm_rd(0x1c00+0xff8);
 uint32_t mask = e[34] <= 0x20010004 ? 0xffffffff : 0x1ff;
 e[35] = e[34] <= 0x20010004 ? 0xbabeface : 0x100;
 gm_wr(0x5000, 1); gm_wr(0x51ce, 0);
 gpu_gmu_started = 1; e[4] = 1; /* latch BEFORE releasing reset, including timeout */
 __asm__ volatile("dsb sy" ::: "memory");
 gm_wr(0x5000, 0);
 if (!g_poll(0x501c, mask, e[35], 36)) { e[3] = 0x901; return 0; }
 gm_wr(0x5186, 1);
 if (!g_poll(0x5180, 1, 1, 37)) { e[3] = 0x902; return 0; }
 e[38] = gm_rd(0x5181);
 int err = g_hfi_send(4, perf, 59, 0);
 if (!err) err = g_hfi_send(3, bw, 160, 1);
 uint32_t core[2] = {0}, start[1] = {0};
 if (!err) err = g_hfi_send(14, core, 2, 2);
 if (!err) err = g_hfi_send(10, start, 1, 3);
 if (err) { e[3] = err; return 0; }
 e[3] = 1; gpu_s9_ok = 1;
 return 1;
}
static void g_gx(void)
{
 uint32_t *e = gpu_extra[0];
 if (g_rd(GPU_SMMU_BASE, g_cb(g_bank) + 0x58) & 0xc00001feu) { e[60] = 0xa04; return; }
 uint32_t vote[4] = {0,1,g_gpu_selected ? g_gpu_selected : 1,g_gpu_bw_index(g_gpu_selected)};
 int err = g_hfi_send(30, vote, 4, 4);
 if (err) { e[60] = err; return; }
 gm_wr(0x5191, 1u<<31); gm_wr(0x5194, 1u<<30);
 int ack = g_poll(0x5192, 1u<<31, 1u<<31, 61);
 gm_wr(0x5191, 1u<<31);
 if (!ack) { gm_wr(0x5194, 1u<<31); e[60] = 0xa01; return; }
 /* Hold GPU_SET while checking GX and reading RBBM, just as Linux hw_init.
  * Linux XML GX_HM_OFF bits6/7 must be clear; also wait transition bits0/1. */
 if (!g_poll(0x50d0, 0xc3, 0, 62)) { gm_wr(0x5194, 1u<<31); e[60] = 0xa02; return; }
 e[63] = g_rd(GPU_GPUCC_BASE, CC_GX_GDSCR);
 if (!g_gdsc_on(e[63])) { gm_wr(0x5194, 1u<<31); e[60] = 0xa03; return; }
 e[64] = g_rd(GPU_REG_BASE, GPU_RBBM_STATUS*4); e[65] = 1;
 gm_wr(0x5194, 1u<<31); e[60] = 1;
}

/* Milestone 3: fixed GPU address space, Linux a7xx CP init, one CP_MEM_WRITE.
 * Only the USB monitor CPU calls this. All new MMIO accesses recover local
 * synchronous aborts; an asynchronous SError/power failure can still reset. */
#define G3_PT       (G_MEM + 0x1200000)
#define G3_SQE_PA   (G_MEM + 0x1300000)
#define G3_RING_PA  (G_MEM + 0x1320000)
#define G3_PTR_PA   (G_MEM + 0x1328000)
#define G3_FENCE_PA (G_MEM + 0x1329000)
/* The ABI root needs the full client aperture. Keep the runtime ring/SQE
 * below it in BOTH roots when enabled; loader/default-off addresses stay put. */
#if defined(FUXI_GPU_XNU)
#define G3_SQE_VA   (fg_abi_enabled ? 0x10000000u : 0x40000000u)
#else
#define G3_SQE_VA   0x40000000u
#endif
#define G3_RING_VA  (G3_SQE_VA+0x20000u)
#define G3_PTR_VA   (G3_SQE_VA+0x28000u)
#define G3_FENCE_VA (G3_SQE_VA+0x29000u)
#define G3_MAGIC    0xa740f00du
#define G3_GUARD    0x5ac3e17bu
#define G3_FSR_MASK 0xc00001feu /* SMMUv2 defined faults; keep reserved bit10 in report */
#define G3_CP_ERRORS 0x3fff7u
#define G3_RBBM_ERRORS 0x33c002c2u
#include "gpu-a740.h"
_Static_assert(G3_PT >= G_DEBUG_PA + 0x7000, "GPU tables overlap GMU buffers");
_Static_assert(G3_PT + 0x100000 == G3_SQE_PA, "GPU table pool limit");
_Static_assert(G3_FENCE_PA + 0x1000 <= G_MEM + G_MEM_SIZE, "GPU scratch overflow");
static unsigned g3_bank, g3_asid, g3_slot[2], g3_next, g3_wptr;
unsigned probe_safe_wr32(uint64_t pa, uint32_t value);
static void g3_fault(uint64_t pa, unsigned op)
{
 uint32_t *e = gpu_extra[5];
 if (!e[116]++) { e[117] = pa; e[118] = pa >> 32; }
 e[110] = pa; e[111] = pa >> 32; e[112] = probe_fault_esr;
 e[113] = probe_fault_far; e[114] = probe_fault_far >> 32; e[115] = op;
}
static uint32_t g3_read(uint64_t pa)
{
 uint32_t fault, v = probe_safe_rd32(pa, &fault);
 if (fault) g3_fault(pa, 1);
 return v;
}
static int g3_write(uint64_t pa, uint32_t v)
{
 if (probe_safe_wr32(pa, v)) { g3_fault(pa, 2); return 0; }
 return 1;
}
static uint32_t g3_gr(unsigned reg) { return g3_read(GPU_REG_BASE + reg*4); }
static int g3_gw(unsigned reg, uint32_t v) { return g3_write(GPU_REG_BASE + reg*4, v); }
static int g3_gw64(unsigned reg, uint64_t v)
{ return g3_gw(reg, v) && g3_gw(reg+1, v >> 32); }
static uint32_t g3_sr(unsigned off) { return g3_read(GPU_SMMU_BASE + off); }
static int g3_sw(unsigned off, uint32_t v) { return g3_write(GPU_SMMU_BASE + off, v); }

/* Frozen 8a ownership plus a full live recheck: never assume reset junk is an
 * ASID, never steal any enabled/referenced/unreadable bank, even from ourselves. */
static int g3_select(void)
{
 uint32_t *e = gpu_extra[5]; unsigned used = 1u << g_bank;
 unsigned free_slots = 0;
 for (unsigned i = 0; i < g_nsmr; i++) {
  if (gpu_extra[1][51] & (1u << i)) continue;
  unsigned smr = g3_sr(SMMU_SMR(i)), s2cr = g3_sr(SMMU_S2CR(i));
  if (e[116]) return 0xb01;
  if (i == g_slot) {
   if (smr != 0x80000005 || s2cr != g_bank) return 0xb02;
  } else if (smr != gpu_extra[1][2*i] || s2cr != gpu_extra[1][2*i+1]) return 0xb02;
  if (!(smr & (1u << 31))) {
   if (free_slots < 2) g3_slot[free_slots++] = i;
   continue;
  }
  unsigned mask = (smr >> 16) & 0x7fff;
  for (unsigned sid = 0; sid < 2; sid++)
   if (((smr ^ sid) & ~mask & 0x7fff) == 0) return 0xb03;
  if (((s2cr >> 16) & 3) == 0 && (s2cr & 255) < g_ncb) used |= 1u << (s2cr & 255);
 }
 /* Faulted stream ownership cannot prove the absence of a masked SID0/1. */
 if (gpu_extra[1][51]) return 0xb01;
 g3_bank = g_ncb;
 unsigned s2banks = (g3_sr(SMMU_ID1) >> 16) & 255;
 for (unsigned b = s2banks ? s2banks : 1; b < g_ncb; b++) {
  if ((used & (1u << b)) || (gpu_extra[1][52] & (1u << b))) continue;
  if (!(gpu_extra[2+b/8][b%8*16+2] & 1) && !(g3_sr(g_cb(b)) & 1)) { g3_bank = b; break; }
 }
 if (e[116]) return 0xb01;
 if (free_slots < 2 || g3_bank == g_ncb) return 0xb04;
 unsigned asids[8] = {0};
 asids[g_asid/32] |= 1u << (g_asid%32);
 for (unsigned b = 0; b < g_ncb; b++) {
  if (gpu_extra[1][52] & (1u << b)) continue;
  unsigned cb = g_cb(b), ctl = g3_sr(cb);
  if (ctl & 1) {
   unsigned a0 = g3_sr(cb+0x24) >> 16, a1 = g3_sr(cb+0x2c) >> 16;
   asids[(a0&255)/32] |= 1u << (a0%32);
   asids[(a1&255)/32] |= 1u << (a1%32);
  }
 }
 if (e[116]) return 0xb01;
 for (g3_asid = 1; g3_asid < 256; g3_asid++)
  if (!(asids[g3_asid/32] & (1u << (g3_asid%32)))) break;
 if (g3_asid == 256) return 0xb04;
 e[8] = g_bank; e[9] = g_asid; e[10] = g3_slot[0]; e[11] = g3_bank;
 e[12] = g3_asid; e[34] = g3_slot[1];
 return 0;
}
static int g3_context(void)
{
 uint32_t *e = gpu_extra[5]; unsigned cb = g_cb(g3_bank);
 uint64_t ttbr = G3_PT | ((uint64_t)g3_asid << 48);
 if (g3_sr(cb) & 1) return 0xb02;
 e[72] = g3_sr(cb+4); e[73] = g3_sr(cb+0x34); /* ACTLR/CONTEXTIDR preserved */
 if (e[116]) return 0xb01;
 if (!g3_sw(g_stride+0x800+g3_bank*4, 1) || !g3_sw(g_stride+g3_bank*4, 0x1f300) ||
     !g3_sw(cb+0x10, 0x38011) || !g3_sw(cb+0x30, 0x80802020) ||
     !g3_sw(cb+0x20, (uint32_t)ttbr) || !g3_sw(cb+0x24, ttbr >> 32) ||
     !g3_sw(cb+0x28, 0) || !g3_sw(cb+0x2c, g3_asid << 16) ||
     !g3_sw(cb+0x38, 0xff44) || !g3_sw(cb+0x3c, 0) ||
     !g3_sw(cb+0x58, 0xffffffff) || !g3_sw(cb+0x610, g3_asid) || !g3_sw(cb+0x7f0, 0)) return 0xb01;
 e[15] = 1;
 unsigned i;
 for (i = 0; i < 1000; i++) {
  e[16] = g3_sr(cb+0x7f4);
  if (e[116]) return 0xb01;
  if (!(e[16] & 1)) break;
  delay(1);
 }
 if (i == 1000) return 0xb06;
 e[23] = g3_sr(g_stride+g3_bank*4); e[24] = g3_sr(g_stride+0x800+g3_bank*4);
 e[25] = g3_sr(cb+0x10); e[26] = g3_sr(cb+0x30);
 e[27] = g3_sr(cb+0x20); e[28] = g3_sr(cb+0x24); e[29] = g3_sr(cb+0x38);
 if (e[23] != 0x1f300 || e[24] != 1 || (e[25] & ~0x60u) != 0x38011 ||
     e[26] != 0x80802020 || e[27] != (uint32_t)ttbr || e[28] != (uint32_t)(ttbr >> 32) ||
     e[29] != 0xff44 || g3_sr(cb+0x28) || g3_sr(cb+0x2c) != g3_asid << 16 ||
     g3_sr(cb+0x3c)) return e[116] ? 0xb01 : 0xb07;
 if (!g3_sw(cb, 0x1067)) return 0xb01;
 e[30] = g3_sr(cb);
 if ((e[30] & ~0x1000u) != 0x67) return e[116] ? 0xb01 : 0xb07;
 for (unsigned sid = 0; sid < 2; sid++) {
  unsigned slot = g3_slot[sid], word = sid ? 35 : 31;
  /* Publish stream last; no CP/LPAC DMA has been started yet. */
  if (g3_sr(SMMU_SMR(slot)) & (1u << 31)) return 0xb02;
  if (e[116] || !g3_sw(SMMU_S2CR(slot), g3_bank) ||
      !g3_sw(SMMU_SMR(slot), 0x80000000u | sid)) return 0xb01;
  e[word] = g3_sr(SMMU_SMR(slot)); e[word+1] = g3_sr(SMMU_S2CR(slot));
  if (e[word] != (0x80000000u | sid) || e[word+1] != g3_bank) return e[116] ? 0xb01 : 0xb07;
 }
 return e[116] ? 0xb01 : 0;
}
static int g3_memory(void)
{
 uint32_t *e = gpu_extra[5]; e[0] = 0xa7400009;
 e[37] = G3_SQE_VA; e[38] = G3_RING_VA; e[39] = G3_PTR_VA; e[40] = G3_FENCE_VA;
 e[41] = 0x8000; e[13] = (uint32_t)G3_PT; e[14] = G3_PT >> 32;
 const uint64_t pa[] = {G3_SQE_PA,G3_RING_PA,G3_PTR_PA,G3_FENCE_PA};
 for (unsigned i = 0; i < 4; i++) { e[64+2*i] = pa[i]; e[65+2*i] = pa[i] >> 32; }
 e[74] = g3_sr(g_cb(g_bank)+0x58);
 if (e[116]) return 0xb01;
 if (e[74] & G3_FSR_MASK) return 0xb08;
 int err = g3_select(); if (err) return err;
 e[76] = g3_sr(g_cb(g3_bank)+0x58);
 if (e[116]) return 0xb01;
 /* Existing reservation was overlap-checked in stage8. Only its unused tail
  * is zeroed here; neither the GMU mappings nor its page tables are touched. */
 g_zero(G3_PT, 0x100000); g_zero(G3_SQE_PA, 0x2a000); g3_next = 1;
 /* SQE GPU-read-only, privileged/unprivileged reads; other pages RW/APRIV.
  * CP/UCHE uses SID0; Linux sm8550.dtsi also attaches LPAC SID1 to this domain. */
 if (!g_map_at(G3_PT, &g3_next, G3_SQE_VA, G3_SQE_PA, 0x20000, 3u << 6) ||
     !g_map_at(G3_PT, &g3_next, G3_RING_VA, G3_RING_PA, 0x8000, 1u << 6) ||
     !g_map_at(G3_PT, &g3_next, G3_PTR_VA, G3_PTR_PA, 0x1000, 0) ||
     !g_map_at(G3_PT, &g3_next, G3_FENCE_VA, G3_FENCE_PA, 0x1000, 1u << 6)) return 0xb05;
 e[33] = g3_next;
 err = g3_context(); if (err) return err;
 e[75] = g3_sr(g_cb(g_bank)+0x58); e[77] = g3_sr(g_cb(g3_bank)+0x58);
 if (e[116]) return 0xb01;
 return ((e[75] | e[77]) & G3_FSR_MASK) ? 0xb08 : 1;
}

/* Odd-parity packet7 header, adreno_gpu.h OUT_PKT7 (count excludes header). */
static unsigned g3_parity(unsigned x)
{
 x ^= x >> 16; x ^= x >> 8; x ^= x >> 4;
 return (0x9669u >> (x & 15)) & 1;
}
static uint32_t g3_pkt7(unsigned op, unsigned count)
{ return 0x70000000u | count | g3_parity(count)<<15 | op<<16 | g3_parity(op)<<23; }
static void g3_emit(uint32_t word)
{
#ifdef FUXI_GPU_XNU
 /* Uncached scratch, serialized once at the doorbell. Avoid an MMIO
  * recovery call and three barriers for every command dword. */
 ((volatile uint32_t *)G_PTR_RANGE(G3_RING_PA,32768))[G_RING_INDEX(g3_wptr++)] = word;
#else
 g_wr(G3_RING_PA, G_RING_INDEX(g3_wptr++) * 4, word);
#endif
} /* Caller bounds every submission; only XNU reuses a retired ring. */
static int g3_sqe(void)
{
 uint32_t *e = gpu_extra[5];
#if defined(FUXI_GPU_XNU)
 unsigned bytes_unused;
 const uint8_t *fw = fg_firmware(1, &bytes_unused);
 size_t bytes = bytes_unused;
#elif defined(FUXI_XNU_LOADER)
 extern const uint8_t _sqe_fw_start[], _sqe_fw_end[];
 const uint8_t *fw = _sqe_fw_start;
 size_t bytes = (uintptr_t)_sqe_fw_end - (uintptr_t)_sqe_fw_start;
#else
 const uint8_t *fw = 0; size_t bytes = 0;
#endif
 e[17] = bytes;
 if (!bytes) return 0xc01;
 if (bytes <= 4 || (bytes & 3) || bytes - 4 > 0x20000) return 0xc02;
 e[18] = bytes-4; e[19] = cdb32(fw); e[20] = cdb32(fw+4);
 /* Raw payload prefix (not parsed as pointers or used to choose sections). */
 for (unsigned i = 0; i < 12 && i*4 < bytes-4; i++) {
  e[97+i] = cdb32(fw+4+i*4); e[109] = i+1;
 }
 /* Linux adreno_fw_create_bo drops the file's leading dword. */
 for (unsigned i = 4; i < bytes; i += 4) {
  uint32_t v = cdb32(fw+i); g_wr(G3_SQE_PA, i-4, v);
  if (g_rd(G3_SQE_PA, i-4) != v) { e[22] = i; return 0xc03; }
  e[21]++;
 }
 return 0;
}
static int g3_hold_gx(void)
{
 uint32_t *e = gpu_extra[5];
 if (!g3_write(GPU_GMU_CORE+0x5191*4, 1u<<31) ||
     !g3_write(GPU_GMU_CORE+0x5194*4, 1u<<30)) return 0xc04;
 for (unsigned i = 0; i < 1000; i++) {
  e[61] = g3_read(GPU_GMU_CORE+0x5192*4);
  if (e[116]) return 0xc04;
  if (e[61] & (1u<<31)) { e[7] = 1; break; }
  delay(10);
 }
 if (!e[7]) return 0xc04;
 if (!g3_write(GPU_GMU_CORE+0x5191*4, 1u<<31)) return 0xc04;
 for (unsigned i = 0; i < 1000; i++) {
  e[62] = g3_read(GPU_GMU_CORE+0x50d0*4);
  if (e[116]) return 0xc04;
  if (!(e[62] & 0xc3)) break;
  if (i == 999) return 0xc04;
  delay(10);
 }
 e[63] = g3_read(GPU_GPUCC_BASE+CC_GX_GDSCR);
 return (!e[116] && (e[63] & (1u<<31))) ? 0 : 0xc04;
}

/* The same critical pointers are sampled before RUN and after completion.
 * Keep the register list shared so failed readbacks are identifiable by index.
 * These offsets are from Linux a6xx.xml (A7XX), not guessed BV/LPAC rings. */
static const unsigned g3_pointer_regs[16] = {
 G3_SQE_BASE,G3_SQE_BASE+1,G3_RB_BASE,G3_RB_BASE+1,G3_RB_CNTL,
 G3_RPTR_ADDR,G3_RPTR_ADDR+1,G3_BV_RPTR_ADDR,G3_BV_RPTR_ADDR+1,
 G3_APRIV,0xad0,0xb31,G3_APERTURE_HOST,G3_LPAC_CNTL,0xb82,0xb83,
};
static int g3_verify_pointers(void)
{
 uint32_t *x = gpu_extra[7];
 const uint32_t expected[13] = {
  G3_SQE_VA,0,G3_RING_VA,0,0x20c,G3_PTR_VA,0,G3_PTR_VA+4,0,0x6f,0xf,0xf,0,
 };
 x[0] = 0xa7400009; x[1] = 12; x[3] = 1;
 for (unsigned i = 0; i < 16; i++) x[8+i] = g3_gr(g3_pointer_regs[i]);
 if (gpu_extra[5][116]) return 0xc05;
 for (unsigned i = 0; i < 13; i++)
  if (x[8+i] != expected[i]) { gpu_extra[5][94] = i+1; return 0xc0b; }
 /* Linux starts neither LPAC nor a separate BV ring in hw_init. Their
  * instruction/ring bases must not be invented to hide a null read. */
 return 0;
}
/* Three read-only checkpoints: OOB held, configured/pre-RUN, post-RUN.
 * Preserve these in stage12's frozen record; no status clear or CP reset. */
static void g3_checkpoint(unsigned sample)
{
 uint32_t *d = &gpu_extra[6][34+sample*10];
 static const unsigned regs[] = {
  G3_SQE_CNTL,G3_SQE_BASE,G3_SQE_BASE+1,0x823,0x210,0x821,0x43,G3_MISC,G3_CHICKEN,
 };
 for (unsigned i = 0; i < 9; i++) d[i] = g3_gr(regs[i]);
 d[9] = g3_sr(g_cb(g3_bank)+0x58);
 gpu_extra[5][119] |= 1u << sample;
}
/* See docs-lanes/gpu.md for the frozen diagnostic snapshots. */
static void g3_diagnostics(unsigned stage)
{
 uint32_t *d = &gpu_extra[6][stage == 13 ? 64 : 0];
 uint32_t *x = gpu_extra[stage == 13 ? 8 : 7];
 uint32_t *y = gpu_extra[stage == 13 ? 10 : 9];
 y[0] = 0xa7400009; y[1] = stage;
 if (stage == 13)
  for (unsigned i = 0; i < 24; i++) x[i] = gpu_extra[7][i];
 x[0] = 0xa7400009; x[1] = stage; x[2] = gpu_extra[5][60];
 d[0] = stage;
 static const unsigned regs[] = {0x806,0x807,0x210,0x808,0x821,0x823,0x824,0x201,0xf400,0xfc00,0xfc01};
 static const unsigned words[] = {1,2,6,7,9,10,11,12,13,14,15};
 for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) d[words[i]] = g3_gr(regs[i]);
 if (g3_gw(G3_STAT_ADDR, 1)) d[8] = g3_gr(G3_STAT_DATA);
 d[3] = g_rd(G3_PTR_PA, 0); d[4] = g_rd(G3_PTR_PA, 4); d[5] = g_rd(G3_FENCE_PA, 0);
 d[16] = g3_sr(SMMU_GFSR);
 for (unsigned j = 0; j < 2; j++) {
  unsigned cb = g_cb(j ? g_bank : g3_bank), n = 17 + j*6;
  d[n] = j ? g_bank : g3_bank;
  d[n+1] = g3_sr(cb+0x58); d[n+2] = g3_sr(cb+0x60); d[n+3] = g3_sr(cb+0x64);
  d[n+4] = g3_sr(cb+0x68); d[n+5] = g3_sr(cb+0x6c);
 }
 d[29] = g3_read(GPU_GMU_CORE+0x5192*4);
 d[30] = g3_read(GPU_GMU_CORE+0x9305*4);
 d[32] = g_rd(G3_FENCE_PA, 4); d[33] = g_rd(G3_FENCE_PA, 0xffc);
 for (unsigned i = 0; i < 16; i++) x[24+i] = g3_gr(g3_pointer_regs[i]);
 /* GR1 CBFRSYNRA supplies SID; FSYNR1 supplies the Qualcomm master ID.
  * Both are meaningful only when that bank has a defined FSR fault. */
 x[40] = g3_sr(g_stride+0x400+g3_bank*4);
 x[41] = g3_sr(g_stride+0x400+g_bank*4);
 static const unsigned regs2[] = {
  0xe19,0x211,0x212,0x213,0x825,0x812,0x928,0x929,0x92a,
  0x92b,0x92c,0x92d,0x939,0x93d,0x931,0x932,0x933,0xa64,0xa61,
 };
 for (unsigned i = 0; i < sizeof regs2 / sizeof regs2[0]; i++) x[42+i] = g3_gr(regs2[i]);
 /* a7xx_indexed_reglist: exactly 0x33 BR SQE words. Re-select every
  * index rather than depending on the data port's auto-increment state. */
 for (unsigned i = 0; i < 0x33; i++) {
  if (!g3_gw(G3_STAT_ADDR, i)) break;
  x[64+i] = g3_gr(G3_STAT_DATA); x[63] = i+1;
 }
 const unsigned ports[2][2] = {{G3_BV_STAT_ADDR,G3_BV_STAT_DATA},{G3_AC_STAT_ADDR,G3_AC_STAT_DATA}};
 for (unsigned j = 0; j < 2; j++)
  for (unsigned i = 0; i < 2; i++)
   if (g3_gw(ports[j][0], i)) x[115+j*2+i] = g3_gr(ports[j][1]);
 /* Linux dumps this indexed window too. Keep raw instruction-port words:
  * their interpretation is firmware/hardware dependent, so never gate on
  * them or claim a successful CPU copy proves instruction-cache contents. */
 for (unsigned i = 0; i < 4; i++) {
  if (g3_gw(0x910, i)) x[120+i] = g3_gr(0x911);
  x[124+i] = g_rd(G3_SQE_PA, i*4);
 }
 /* Same 0x33-word bound as Linux a7xx_indexed_reglist for BV and AC.
  * Stop a window on abort; record unsupported/sentinel values verbatim. */
 const unsigned dbg[2][2] = {{G3_BV_UCODE_ADDR,G3_BV_UCODE_DATA},{G3_AC_UCODE_ADDR,G3_AC_UCODE_DATA}};
 for (unsigned j = 0; j < 2; j++) {
  for (unsigned i = 0; i < 0x33; i++) {
   unsigned faults = gpu_extra[5][116];
   if (!g3_gw(ports[j][0], i)) break;
   uint32_t value = g3_gr(ports[j][1]);
   if (gpu_extra[5][116] != faults) break;
   y[16+j*51+i] = value; y[2+j] = i+1;
  }
  for (unsigned i = 0; i < 4; i++) {
   unsigned faults = gpu_extra[5][116];
   if (!g3_gw(dbg[j][0], i)) break;
   y[5+j*4+i] = g3_gr(dbg[j][1]);
   if (gpu_extra[5][116] != faults) break;
  }
  g3_gw(dbg[j][0], 0);
 }
 /* XML defines no separate BV instruction base. AQE words are raw evidence
  * only; Linux does not program these on A740. Never enable an absent pipe. */
 static const unsigned extra_regs[] = {0xb70,0xb71,0xb72,0xb73,G3_MISC,G3_CHICKEN,0xada,0xa03,0x8a1,0x8a2};
 for (unsigned i = 0; i < 10; i++) y[118+i] = g3_gr(extra_regs[i]);
 /* Restore known selector values; never clear/resume a stalled SMMU. */
 g3_gw(G3_STAT_ADDR, 1); g3_gw(G3_BV_STAT_ADDR, 0); g3_gw(G3_AC_STAT_ADDR, 0); g3_gw(0x910, 0);
 y[4] = gpu_extra[5][116]; x[61] = y[4]; d[31] = x[61];
}
/* No interrupt delivery is enabled: inspect raw status, both FSRs and ring.
 * Reserved FSR bit10 was observed on GMU CB1 in m2; retain but don't gate on it. */
static int g3_faulted(void)
{
 uint32_t cp = g3_gr(0x823), irq = g3_gr(0x201);
 uint32_t fsr = g3_sr(g_cb(g3_bank)+0x58) | g3_sr(g_cb(g_bank)+0x58);
 uint32_t gf = g3_sr(SMMU_GFSR);
 return gpu_extra[5][116] || (cp & G3_CP_ERRORS) || (irq & G3_RBBM_ERRORS) || (fsr & G3_FSR_MASK) || gf;
}
static int g3_wait(unsigned fence)
{
 uint32_t *e = gpu_extra[5]; unsigned off = fence ? 55 : 48;
 for (unsigned i = 0; i < 5000; i++) {
  e[off+1] = g3_gr(0x806); e[off+2] = g3_gr(0x807); e[fence ? 58 : 51] = i+1;
  if (e[116]) return fence ? 0xd01 : 0xc05;
  if (!(i & 15) && g3_faulted()) return fence ? 0xd02 : 0xc06;
  if (fence) e[53] = g_rd(G3_FENCE_PA, 0);
  if (e[off+1] == g3_wptr && e[off+2] == g3_wptr &&
      (!fence || e[53] == G3_MAGIC) && !(g3_gr(0x210) & ~1u))
   return g3_faulted() ? (fence ? 0xd02 : 0xc06) : 0;
  delay(10);
 }
 return fence ? 0xd03 : 0xc07;
}
static int g3_hw_init(void)
{
 /* CP/GBIF/UCHE subset of Linux hw_init. No render pipeline or preemption,
  * no concurrent binning/LPAC start, no IFPC register restore list. */
 if (!g3_gw(0x3c45, 0) || !g3_gw(0x16, 0) ||
     !g3_gw(0xf803, 0) || !g3_gw64(0xf800, 0) || !g3_gw(0xf802, 0)) return 0;
 /* Flush both halt clears, as Linux does; retain the readbacks. */
 gpu_extra[5][95] = g3_gr(0x3c45); gpu_extra[5][96] = g3_gr(0x16);
 if (gpu_extra[5][116]) return 0;
 /* a6xx_set_hwcg(A740), with all sourced A740 register values. */
 if (!g3_write(GPU_GMU_CORE+0x9309*4, 0x20202) ||
     !g3_write(GPU_GMU_CORE+0x930a*4, 0x10111) ||
     !g3_write(GPU_GMU_CORE+0x930b*4, 0x5555)) return 0;
 for (unsigned i = 0; i < sizeof g3_hwcg / sizeof g3_hwcg[0]; i++)
  if (!g3_gw(g3_hwcg[i][0], g3_hwcg[i][1])) return 0;
 static const uint32_t regval[][2] = {
  {0x3c03,0x71620},{0x3c04,0x71620},{0x3c05,0x71620},{0x3c06,0x71620},
  {0x11,0x2120212}, /* RBBM_GBIF_CLIENT_QOS_CNTL */
  {0xe3a,0x10240e0}, /* UCHE_GBIF_GX_CONFIG */
  {0xe17,1u<<23}, /* A7xx UCHE_CACHE_WAYS */
  {0x98d,1}, /* CP_AHB_CNTL: AHB default slave ERROR */
  {0x1f,0x40cfffff}, /* A740 hang detection */
  {0xe19,0x81}, {0xe3c,0x6690e}, /* UCHE_CLIENT_PF / A7xx CMDQ */
  {0xe01,3u<<21}, {0x534,1}, /* A740 UCHE/RBBM NC mode (HBB16) */
  {0x844,0x6f}, {0xad0,0xf}, {0xb31,0xf}, /* BR/BV/LPAC expanded APRIV */
  {0x84f,0xb}, /* CP_PROTECT_CNTL */
  {0x38,0}, /* poll raw interrupt status; GIC delivery disabled */
 };
 for (unsigned i = 0; i < sizeof regval / sizeof regval[0]; i++)
  if (!g3_gw(regval[i][0], regval[i][1])) return 0;
 if (!g3_gw64(0xe09, 0x0001fffffffff000ULL) || !g3_gw64(0xe07, 0x0001fffffffff000ULL) ||
     !g3_gw64(0xe0b, 0x1000000) || !g3_gw64(0xe0d, 0x12fffff)) return 0;
 for (unsigned i = 0; i < 48; i++)
  if (g3_protect[i] && !g3_gw(0x850+i, g3_protect[i])) return 0;
 /* 32K ring: BUFSZ=log2(32768/8)=12, BLKSZ=log2(32/8)=2.
  * Expanded APRIV permits the privileged RPTR shadow (NO_UPDATE unset). */
 return g3_gw64(G3_SQE_BASE, G3_SQE_VA) && g3_gw64(G3_RB_BASE, G3_RING_VA) &&
        g3_gw(G3_RB_CNTL, 0x20c) && g3_gw64(G3_RPTR_ADDR, G3_PTR_VA) &&
        g3_gw64(G3_BV_RPTR_ADDR, G3_PTR_VA+4);
}
static int g3_cp_start(void)
{
 uint32_t *e = gpu_extra[5]; e[60] = 1;
 int err = g3_sqe(); if (err) return err;
 e[60] = 2; err = g3_hold_gx(); if (err) return err;
 /* OOB is deliberately retained until reset, on success AND failure. This
  * avoids an IFPC collapse losing CP state between USB stages12 and13. */
 e[60] = 3;
 uint32_t *x = gpu_extra[7]; x[0] = 0xa7400009; x[1] = 12; x[3] = 4;
 g3_checkpoint(0);
 if (e[116]) return 0xc05;
 if (g3_faulted()) return 0xc06;
 if (gpu_extra[6][34]) return 0xc0c; /* refuse an already enabled/live SQE */
 /* Match Linux cold hw_init: no SQE_CNTL write until all bases are valid.
  * Its CNTL=3 is recovery's halt of an already running engine, not a cold
  * disable. M3b faulted with phase1/started0 and CNTL read back1: an early
  * enable/reset command before INSTR_BASE is the leading suspect. */
 if (!g3_hw_init()) return 0xc05;
 g3_checkpoint(1);
 err = g3_verify_pointers(); if (err) return err;
 e[48] = g3_gr(0x806); e[42] = g3_gr(0x807);
 if (e[116]) return 0xc05;
 if (e[48] || e[42]) return 0xc08; /* require reset ring, don't steal a live CP */
 g3_wptr = 0;
 g3_emit(g3_pkt7(0x17, 1)); g3_emit(1u<<27); /* disable concurrent binning */
 /* No IFPC/preemption list exists in this probe. Do not advertise ordinal8
  * REGISTER_INIT_LIST_WITH_SPINLOCK with a null pointer (Linux's TODO).
  * Keep ordinals0/1/3/6 and their four payload words, omit the list ordinal
  * and its three words together. M3b's phase1/started0 fault already
  * precedes this packet's doorbell; keep the absent list omitted. */
 g3_emit(g3_pkt7(0x48, 4));
 g3_emit(0x4b); g3_emit(3); g3_emit(0x20000000); g3_emit(2);
 for (unsigned i = 0; i < G3_INIT_DWORDS; i++) e[80+i] = g_rd(G3_RING_PA, i*4);
 e[60] = 7; /* SQE running, wptr=0: isolate instruction fetch from PM4 */
 if (g3_faulted()) return 0xc06;
 if (!g3_gw(0x807, 0)) return 0xc05;
 __asm__ volatile("dsb sy" ::: "memory");
 x[3] = 2;
 if (!g3_gw(G3_SQE_CNTL, 1)) return 0xc05;
 e[59] = 1;
 g3_checkpoint(2);
 if (e[116]) return 0xc05;
 for (unsigned i = 0; i < 32; i++) {
  x[4] = i+1; x[5] = g3_gr(0x823); x[6] = g3_sr(g_cb(g3_bank)+0x58); x[7] = g3_gr(0x210);
  if (g3_faulted()) return e[116] ? 0xc05 : 0xc06;
  delay(10);
 }
 e[50] = g3_wptr; e[60] = 4; x[3] = 3;
 __asm__ volatile("dsb sy" ::: "memory");
 if (!g3_gw(0x807, g3_wptr)) return 0xc05;
 err = g3_wait(0); if (err) return err;
 /* Retail Fuxi keeps TRUST=1. M4 leaves secure mode alone: read-only
  * SECVID/TSB evidence, no legacy no-zap TRUST write and no SCM/PAS. */
 e[60] = 5; e[44] = g3_gr(0xf400); e[45] = g3_gr(0xf400);
 e[46] = g3_gr(0xfc00); e[47] = g3_gr(0xfc01);
 if (e[116]) return 0xc09;
 if (g3_faulted()) return 0xc06;
 e[60] = 6;
 return 1;
}
static int g3_fence(void)
{
 uint32_t *e = gpu_extra[5]; e[54] = G3_MAGIC;
 e[55] = g3_gr(0x806); e[52] = g_rd(G3_FENCE_PA, 0);
 if (g3_faulted()) return 0xd02;
 if (e[55] != G3_INIT_DWORDS || g3_wptr != G3_INIT_DWORDS || g3_gr(0x807) != G3_INIT_DWORDS || e[52]) return 0xd04;
 g_wr(G3_FENCE_PA, 4, G3_GUARD); g_wr(G3_FENCE_PA, 0xffc, G3_GUARD);
 g3_emit(g3_pkt7(0x3d, 3)); g3_emit(G3_FENCE_VA); g3_emit(0); g3_emit(G3_MAGIC);
 for (unsigned i = 0; i < 4; i++) e[90+i] = g_rd(G3_RING_PA, (G3_INIT_DWORDS+i)*4);
 e[57] = g3_wptr;
 __asm__ volatile("dsb sy" ::: "memory");
 if (!g3_gw(0x807, g3_wptr)) return 0xd01;
 int err = g3_wait(1); if (err) return err;
 if (g_rd(G3_FENCE_PA, 4) != G3_GUARD || g_rd(G3_FENCE_PA, 0xffc) != G3_GUARD) return 0xd05;
 return 1;
}
/* Milestone 4: one-shot A7xx BLIT_OP_SCALE, linear BGRA8888 only.
 * Linux XML names retained; Mesa renamed these to *_A2D_*.
 * References: Mesa 875160ed32cc, fd6_blitter.cc emit_blit_{setup,src,dst},
 * tu_clear_blit.cc r2d_{setup_common,src_buffer,run}, fd6_gmem_cache.h.
 * The loader remains uncached and GPU_SET remains held through all failures. */
#define G4_SRC_PA (G_MEM + 0x1401000)
#define G4_DST_PA (G_MEM + 0x1501000)
#define G4_SRC_VA 0x40201000u
#define G4_DST_VA 0x40401000u
#define G4_FB_VA  0x41000000u
#define G4_FB_PA  0xb8000000ULL
#define G4_BYTES  0x40000u
#define G4_FB_BYTES (1080u * 2400u * 4u)
#define G4_FB_MAP ((G4_FB_BYTES + 4095u) & ~4095u)
#define G4_WIDTH 256u
#define G4_FB_HEIGHT 64u
#define G4_FB_X 64u
#define G4_FB_Y 256u
#define G4_FENCE14 (G3_FENCE_VA + 16u)
#define G4_FENCE15 (G3_FENCE_VA + 32u)
#define G4_CLEAR_FENCE (G3_FENCE_VA + 48u)
#define G4_CLEAR_MAGIC 0xa740c014u
#define G4_CLEAR_PIXEL 0xff336699u
/* v7 record[43] validity: CPU, clear submitted/completed, copy submitted/
 * completed, delayed copy status sampled. Zero ticks without CPU_VALID is N/A. */
#define G4_CPU_VALID 1u
#define G4_CLEAR_SENT 2u
#define G4_CLEAR_DONE 4u
#define G4_COPY_SENT 8u
#define G4_COPY_DONE 16u
#define G4_DELAY_VALID 32u
_Static_assert(G4_SRC_PA - 4096 >= G3_FENCE_PA + 4096, "blit/source overlap");
_Static_assert(G4_SRC_PA + G4_BYTES + 4096 <= G4_DST_PA - 4096, "blit surfaces overlap");
_Static_assert(G4_DST_PA + G4_BYTES + 4096 <= G_MEM + G_MEM_SIZE, "blit exceeds scratch");
/* The largest job has 64 row blits, each <=32 words, plus <128 setup/end.
 * Stage14 has two bounded jobs whose combined size is also below this budget.
 * Capacity is checked before writing even the first new ring word. */
#define G4_RING_BUDGET (G4_FB_HEIGHT * 32u + 128u)
static uint32_t g4_border[2 * G4_FB_HEIGHT + 2 * G4_WIDTH];
static uint32_t g4_pkt4(unsigned reg, unsigned count)
{ return 0x40000000u | count | g3_parity(count)<<7 | reg<<8 | g3_parity(reg)<<27; }
static void g4_reg(unsigned reg, uint32_t val)
{ g3_emit(g4_pkt4(reg, 1)); g3_emit(val); }
static void g4_event(unsigned event)
{ g3_emit(g3_pkt7(0x46, 1)); g3_emit(event); }
static void g4_wfi(void) { g3_emit(g3_pkt7(0x26, 0)); }
static uint32_t g4_pattern(unsigned x, unsigned y, unsigned panel)
{
 if (!panel) return ((x ^ y) << 24) | (x << 16) | (y << 8) | ((x * 13u + y * 7u) & 255);
 static const uint32_t bars[] = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,
                                0xffffff00,0xff00ffff,0xffff00ff,0xff202020};
 return bars[x / 32u];
}
static int g4_tlb(uint32_t *d)
{
 unsigned cb = g_cb(g3_bank);
 __asm__ volatile("dsb sy" ::: "memory");
 if (!g3_sw(cb+0x610, g3_asid) || !g3_sw(cb+0x7f0, 0)) return 0xe01;
 for (unsigned i = 0; i < 1000; i++) {
  d[29] = g3_sr(cb+0x7f4); d[30] = i+1;
  if (gpu_extra[5][116]) return 0xe01;
  if (!(d[29] & 1)) return 0;
  delay(1);
 }
 return 0xe03;
}
/* Source/destination base64 alignment and pitch64 units; nearest, no UBWC,
 * no sRGB, single sample. WXYZ swap (1) is BGRA in Mesa fd6_format_table.c. */
static void g4_blit(unsigned src, unsigned dst, unsigned height,
                    unsigned dst_pitch, unsigned dx, unsigned sy)
{
 g3_emit(g4_pkt4(G4_SP_SRC_INFO, 5));
 g3_emit(0x00500430); /* format30 | WXYZ<<10 | UNK20 | UNK22 */
 g3_emit(G4_WIDTH | 256u<<15); g3_emit(src); g3_emit(0);
 g3_emit((1024u >> 6) << 9);
 g3_emit(g4_pkt4(G4_RB_DST_INFO, 4));
 g3_emit(0x430); g3_emit(dst); g3_emit(0); g3_emit(dst_pitch >> 6);
 g3_emit(g4_pkt4(G4_GRAS_SRC_X, 6));
 g3_emit(0); g3_emit(255u<<8); g3_emit(sy<<8); g3_emit((sy+height-1)<<8);
 g3_emit(dx); g3_emit((dx+255u) | (height-1)<<16);
 g4_event(63); /* LABEL, fd6 emit_blit_fini */
 g3_emit(g3_pkt7(0x2c, 1)); g3_emit(3); /* CP_BLIT / BLIT_OP_SCALE */
 g4_wfi(); /* serialize row address/coord changes; no concurrent binning */
}
static void g4_setup(void)
{
 /* Linux CP init only disabled binning; Turnip tu6_init_hw explicitly
  * selects BR before any render state. THREAD=0 is not CP_SET_THREAD_BR. */
 g3_emit(g3_pkt7(0x17, 1)); g3_emit((1u<<27) | 1u);
 g4_wfi();
 g3_emit(g3_pkt7(0x65, 1)); g3_emit(0xc); /* RM6_BLIT2DSCALE */
 g4_event(33); g4_event(32); g4_event(26);
 g4_event(25); g4_event(24); g4_event(51);
 g4_wfi();
 /* A740: six CCUs x256KiB depth at0 => color at0x180000.
  * Full CCU caches in sysmem mode, fd6_calc_gmem_cache_offsets. */
 g4_reg(G4_CCU_CNTL2, 0xc0000000);
 g4_reg(G4_CCU_CNTL, 0); /* disable concurrent resolve, not a GMEM resolve */
 g4_wfi(); /* A7xx static CCU_CNTL takes effect across WFI */
 g4_reg(G4_RB_BLIT, 0x10f03000); /* UNORM8, mask f, format30, rotate0 */
 g4_reg(G4_RB_PIXEL, 0);
 g4_reg(G4_GRAS_BLIT, 0x10f03000);
 g4_reg(G4_SP_FORMAT, 0xf180); /* float accumulator, format30, mask f */
 g4_reg(G4_TPL1_BLIT, 1u<<29); /* A7xx TEX_2D, raw_copy=false */
 g4_reg(G4_SP_WINDOW, 0);
}
/* The A7xx resolve/blit cache is separate from normal color/depth CCU.
 * Turnip FD_CCU_CLEAN_BLIT_CACHE maps to CCU_RESOLVE_CLEAN (26). A UCHE
 * CACHE_CLEAN fence alone does not request that cache's writeback. */
static void g4_end(unsigned fence, unsigned magic)
{
 g4_event(26); g4_event(33); g4_event(32);
 g4_event(25); g4_event(24);
 g3_emit(g3_pkt7(0x46, 4)); g3_emit(49u | 1u<<27);
 g3_emit(fence); g3_emit(0); g3_emit(magic);
 g4_wfi();
}
static uint64_t g4_counter(void)
{
 /* Serialize the counter at both benchmark boundaries. */
 __asm__ volatile("isb" ::: "memory");
 uint64_t value = rsr("cntpct_el0");
 __asm__ volatile("" ::: "memory");
 return value;
}
static int g4_wait(unsigned fence_off, unsigned magic, uint32_t *polls)
{
 for (unsigned i = 0; i < 5000; i++) {
  *polls = i+1;
  uint32_t rptr = g3_gr(0x806), wptr = g3_gr(0x807);
  uint32_t fence = g3_read(G3_FENCE_PA+fence_off);
  if (gpu_extra[5][116]) return 0xe01;
  if (!(i & 15) && g3_faulted()) return gpu_extra[5][116] ? 0xe01 : 0xe02;
  if (rptr == g3_wptr && wptr == g3_wptr && fence == magic && !(g3_gr(0x210) & ~1u))
   return g3_faulted() ? (gpu_extra[5][116] ? 0xe01 : 0xe02) : 0;
  delay(10);
 }
 return 0xe05;
}
static void g4_delayed_status(uint32_t *d)
{
 delay(100);
 d[112] = g3_gr(0x210); d[113] = g3_gr(0x823);
 /* Raw context reads can depend on the host aperture; report it too.
  * No invented RB/CCU status addresses: busy bits8/7 in RBBM are CCU/RB. */
 static const unsigned regs[] = {G4_CCU_CNTL,G4_CCU_CNTL2,G4_RB_BLIT,
  G4_GRAS_BLIT,G4_SP_FORMAT,G4_SP_SRC_INFO,G4_SP_SRC_INFO+1,
  G4_SP_SRC_INFO+4,G4_RB_DST_INFO,G4_RB_DST_INFO+3,G4_TPL1_BLIT,
  G4_RB_CMP_DBG,G4_RB_DBG,G3_APERTURE_HOST};
 for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) d[114+i] = g3_gr(regs[i]);
 d[43] |= G4_DELAY_VALID;
}
static void g4_snapshot(uint32_t *d)
{
 d[64] = g3_gr(0x806); d[65] = g3_gr(0x807);
 d[66] = g3_gr(0x210); d[67] = g3_gr(0x823); d[68] = g3_gr(0x821);
 d[69] = g3_gr(0x824); d[70] = g3_gr(0x201); d[71] = g3_sr(SMMU_GFSR);
 for (unsigned j = 0; j < 2; j++) {
  unsigned bank = j ? g_bank : g3_bank, at = 72 + j*8, cb = g_cb(bank);
  d[at] = bank; d[at+1] = g3_sr(cb+0x58);
  d[at+2] = g3_sr(cb+0x60); d[at+3] = g3_sr(cb+0x64);
  d[at+4] = g3_sr(cb+0x68); d[at+5] = g3_sr(cb+0x6c);
  d[at+6] = g3_sr(g_stride+0x400+bank*4);
  d[at+7] = g3_sr(cb);
 }
 d[88] = g3_gr(0xf400); /* report-only */
 d[89] = g3_gr(0x211); d[90] = g3_gr(0x212); d[91] = g3_gr(0x213);
 d[92] = gpu_extra[5][116];
 d[93] = gpu_extra[5][110]; d[94] = gpu_extra[5][111]; d[95] = gpu_extra[5][112];
}
static unsigned g4_guard(unsigned panel, unsigned save)
{
 unsigned bad = 0;
 if (!panel) {
  const uint64_t bases[] = {G4_SRC_PA-4096,G4_SRC_PA+G4_BYTES,
                           G4_DST_PA-4096,G4_DST_PA+G4_BYTES};
  for (unsigned j = 0; j < 4; j++)
   for (unsigned off = 0; off < 4096; off += 4) {
    if (save) g_wr(bases[j], off, G3_GUARD);
    else if (g3_read(bases[j]+off) != G3_GUARD) bad++;
   }
 } else {
  for (unsigned i = 0; i < sizeof g4_border / sizeof g4_border[0]; i++) {
   unsigned x, y;
   if (i < 2*G4_FB_HEIGHT) {
    x = i & 1 ? G4_FB_X+G4_WIDTH : G4_FB_X-1; y = G4_FB_Y+i/2;
   } else {
    unsigned k = i-2*G4_FB_HEIGHT;
    x = G4_FB_X+k%G4_WIDTH; y = k < G4_WIDTH ? G4_FB_Y-1 : G4_FB_Y+G4_FB_HEIGHT;
   }
   uint32_t v = g3_read(G4_FB_PA+(y*1080u+x)*4u);
   if (save) g4_border[i] = v; else if (v != g4_border[i]) bad++;
  }
 }
 return bad;
}
static unsigned g4_verify(uint32_t *d, unsigned panel, unsigned cpu)
{
 unsigned height = panel ? G4_FB_HEIGHT : 256u, bad = 0;
 uint32_t actual_sum = 2166136261u, expected_sum = 2166136261u;
 for (unsigned y = 0; y < height; y++)
  for (unsigned x = 0; x < G4_WIDTH; x++) {
   uint32_t want = g4_pattern(x, y, panel);
   uint64_t pa = panel ? G4_FB_PA+((y+G4_FB_Y)*1080u+x+G4_FB_X)*4u : G4_DST_PA+(y*G4_WIDTH+x)*4u;
   uint32_t got = g3_read(pa);
   actual_sum = (actual_sum ^ got)*16777619u; expected_sum = (expected_sum ^ want)*16777619u;
   if (got != want) {
    if (!bad && !cpu) { d[22] = x; d[23] = y; d[24] = want; d[25] = got; }
    bad++;
   }
   /* Also prove the GPU did not corrupt the source. */
   if (!cpu && g3_read(G4_SRC_PA+(y*G4_WIDTH+x)*4u) != want) d[46]++;
  }
 if (cpu) d[47] = bad;
 else { d[19] = expected_sum; d[20] = actual_sum; d[21] = bad; }
 return bad;
}
/* Always run this source-free variant once before the patterned copy.
 * Pixel mismatch alone permits the separately planned copy; faults, a timeout,
 * source corruption or guard damage stop immediately, without another job. */
static int g4_clear(uint32_t *d)
{
 g_wr(G3_FENCE_PA, 48, 0);
 g4_setup();
 g4_reg(G4_RB_BLIT, 0x10f03080); /* SOLID_COLOR bit7 */
 g4_reg(G4_GRAS_BLIT, 0x10f03080);
 g3_emit(g4_pkt4(G4_RB_SOLID, 4));
 g3_emit(0x33); g3_emit(0x66); g3_emit(0x99); g3_emit(0xff);
 /* No source registers are emitted; SOLID_COLOR bypasses source fetch. */
 g3_emit(g4_pkt4(G4_RB_DST_INFO, 4));
 g3_emit(0x430); g3_emit(G4_DST_VA); g3_emit(0); g3_emit(16);
 g3_emit(g4_pkt4(G4_GRAS_SRC_X+4, 2)); g3_emit(0); g3_emit(255u | 255u<<16);
 g3_emit(g3_pkt7(0x2c, 1)); g3_emit(3);
 g4_wfi(); g4_end(G4_CLEAR_FENCE, G4_CLEAR_MAGIC);
 d[54] = g3_wptr;
 __asm__ volatile("dsb sy" ::: "memory");
 if (gpu_extra[5][116] || !g3_gw(0x807, g3_wptr)) return 0xe01;
 d[43] |= G4_CLEAR_SENT;
 int err = g4_wait(48, G4_CLEAR_MAGIC, &d[63]);
 delay(100); d[59] = g3_gr(0x210); d[60] = g3_gr(0x823);
 d[55] = g3_read(G3_FENCE_PA+48);
 if (err) return err;
 d[43] |= G4_CLEAR_DONE;
 for (unsigned i = 0; i < G4_BYTES/4; i++) {
  uint32_t got = g3_read(G4_DST_PA+i*4u);
  if (got != G4_CLEAR_PIXEL) { if (!d[56]) d[57] = got; d[56]++; }
  if (got == 0xdeadbeef) d[58]++;
  if (g3_read(G4_SRC_PA+i*4u) != g4_pattern(i%G4_WIDTH, i/G4_WIDTH, 0)) d[62]++;
 }
 d[61] = g4_guard(0, 0);
 if (g3_read(G3_FENCE_PA+4) != G3_GUARD) d[61]++;
 if (g3_read(G3_FENCE_PA+0xffc) != G3_GUARD) d[61]++;
 if (gpu_extra[5][116]) return 0xe01;
 if (g3_faulted()) return 0xe02;
 if (d[61]) return 0xe07;
 if (d[62]) return 0xe06;
 return d[56] ? 0xe0b : 1;
}
static int g4_copy(unsigned panel, uint32_t *d)
{
 unsigned height = panel ? G4_FB_HEIGHT : 256u;
 unsigned fence_off = panel ? 32 : 16, magic = 0xa740b014u + panel;
 d[3] = 1; d[4] = G4_WIDTH; d[5] = height; d[6] = 1024;
 d[7] = panel ? 4320 : 1024; d[8] = G4_WIDTH*height*4;
 d[9] = G4_SRC_VA; d[10] = panel ? G4_FB_VA : G4_DST_VA;
 d[11] = (uint32_t)G4_SRC_PA; d[12] = G4_SRC_PA >> 32;
 uint64_t dst_pa = panel ? G4_FB_PA : G4_DST_PA;
 d[13] = dst_pa; d[14] = dst_pa >> 32;
 d[15] = panel ? G4_FB_X : 0; d[16] = panel ? G4_FB_Y : 0;
 d[22] = d[23] = 0xffffffffu; d[18] = magic;
 d[17] = panel ? G4_FENCE15 : G4_FENCE14;
 d[31] = g3_wptr; d[42] = rsr("cntfrq_el0");
 d[35] = g3_gr(0x806); d[36] = g3_gr(0x807);
 uint32_t status = g3_gr(0x210);
 if (gpu_extra[5][116]) return 0xe01;
 if (!gpu_extra[5][7] || gpu_extra[5][4] != 1 ||
     d[35] != g3_wptr || d[36] != g3_wptr ||
     (status & ~1u) || g3_wptr + G4_RING_BUDGET >= 0x8000/4) return 0xe04;
 if (g3_faulted()) return gpu_extra[5][116] ? 0xe01 : 0xe02;
 uint32_t guard_lo = g3_read(G3_FENCE_PA+4), guard_hi = g3_read(G3_FENCE_PA+0xffc);
 if (gpu_extra[5][116]) return 0xe01;
 if (guard_lo != G3_GUARD || guard_hi != G3_GUARD) return 0xe07;
 if (panel) {
  /* Same exact source-pipe facts as request4e, plus full surface geometry.
   * Safe reads only; never read MDSS/MDP top, never kick display START. */
  d[50] = g3_read(0x0ae05014); d[51] = g3_read(0x0ae05024);
  d[52] = g3_read(0x0ae05000); d[53] = g3_read(0x0ae05030);
  if (gpu_extra[5][116]) return 0xe01;
  if (d[50] != G4_FB_PA || d[51] != 4320 || d[52] != (2400u<<16 | 1080u) || d[53] != 0x237ff) return 0xe08;
 }
 d[3] = 2;
 if (panel) {
  if (!g_map_at(G3_PT, &g3_next, G4_FB_VA, G4_FB_PA, G4_FB_MAP, 1u<<6)) return 0xe09;
 } else {
  if (!g_map_at(G3_PT, &g3_next, G4_SRC_VA, G4_SRC_PA, G4_BYTES, 3u<<6) ||
      !g_map_at(G3_PT, &g3_next, G4_DST_VA, G4_DST_PA, G4_BYTES, 1u<<6)) return 0xe09;
 }
 d[28] = g3_next;
 int err = g4_tlb(d); if (err) return err;
 d[3] = 3;
 for (unsigned y = 0; y < height; y++)
  for (unsigned x = 0; x < G4_WIDTH; x++) {
   g_wr(G4_SRC_PA, (y*G4_WIDTH+x)*4u, g4_pattern(x,y,panel));
   if (!panel) g_wr(G4_DST_PA, (y*G4_WIDTH+x)*4u, 0xdeadbeef);
  }
 g4_guard(panel, 1);
 g_wr(G3_FENCE_PA, fence_off, 0);
 if (gpu_extra[5][116]) return 0xe01;
 if (g3_faulted()) return gpu_extra[5][116] ? 0xe01 : 0xe02;
 d[3] = 4;
 /* Protected NC registers: CPU safe MMIO, as Linux hw_init, never PKT4.
  * A740 HBB16, RGB565 predicator, AMSBC, UAV flag prediction=2. */
 uint32_t cmp = g3_gr(G4_RB_CMP_DBG);
 if (gpu_extra[5][116]) return 0xe01;
 /* Linux A7xx: prevent non-UBWC reads passing outstanding writes. */
 if (!g3_gw(G4_RB_CMP_DBG, cmp | (1u<<11)) ||
     !g3_gw(G4_RB_NC, 0x816) || !g3_gw(G4_TPL1_NC, 6) ||
     !g3_gw(G4_SP_NC, 0x26) || !g3_gw(G4_GRAS_NC, 0x60)) return 0xe01;
 if (!panel) {
  d[44] = g4_clear(d);
  if (d[44] != 1 && d[44] != 0xe0b) return d[44];
  /* Independent copy test: refill only after clear fence AND idle, with
   * its caches cleaned/invalidated. Preserve the clear readback above. */
  for (unsigned off = 0; off < G4_BYTES; off += 4) g_wr(G4_DST_PA, off, 0xdeadbeef);
 }
 d[31] = g3_wptr; /* primary ring/timing fields describe copy, not clear */
 g4_setup();
 if (!panel) g4_blit(G4_SRC_VA, G4_DST_VA, 256, 1024, 0, 0);
 else for (unsigned y = 0; y < height; y++) {
  unsigned off = ((y+G4_FB_Y)*1080u+G4_FB_X)*4u;
  /* Single-row destination: padded pitch is never used to advance a row.
   * Offset both the aligned base and X for the alternating 32-byte phase. */
  g4_blit(G4_SRC_VA, G4_FB_VA+(off & ~63u), 1, 1088, (off & 63u)/4u, y);
 }
 g4_end(d[17], magic);
 d[32] = g3_wptr; d[33] = g3_wptr-d[31];
 for (unsigned i = 0; i < 16 && i < d[33]; i++) d[96+i] = g_rd(G3_RING_PA, (d[31]+i)*4);
 d[3] = 5;
 __asm__ volatile("dsb sy" ::: "memory");
 uint64_t start = g4_counter();
 if (gpu_extra[5][116] || !g3_gw(0x807, g3_wptr)) return 0xe01;
 d[43] |= G4_COPY_SENT;
 err = g4_wait(fence_off, magic, &d[34]);
 uint64_t elapsed = g4_counter()-start;
 d[38] = elapsed; d[39] = elapsed>>32;
 d[35] = g3_gr(0x806); d[36] = g3_gr(0x807);
 d[37] = g3_read(G3_FENCE_PA+fence_off);
 g4_delayed_status(d);
 if (gpu_extra[5][116]) return 0xe01;
 if (err) return err;
 d[43] |= G4_COPY_DONE;
 if (g3_faulted()) return 0xe02;
 d[3] = 6;
 unsigned bad = g4_verify(d, panel, 0);
 d[26] = g4_guard(panel, 0); d[45] = panel ? g4_guard(0, 0) : d[26];
 d[27] = g3_read(G3_FENCE_PA+4); d[48] = g3_read(G3_FENCE_PA+0xffc);
 if (gpu_extra[5][116]) return 0xe01;
 if (bad || d[46]) return 0xe06;
 if (d[26] || d[45] || d[27] != G3_GUARD || d[48] != G3_GUARD) return 0xe07;
 if (!panel && d[44] != 1) return d[44];
 {
  /* Identical rows/target, only after the GPU readback was captured.
   * Baseline is probe.c's volatile byte memcpy, MMU/caches off. */
  d[3] = 7; start = g4_counter();
  for (unsigned y = 0; y < height; y++)
   memcpy((void *)G_PTR_RANGE(panel ? G4_FB_PA+((y+G4_FB_Y)*1080u+G4_FB_X)*4u : G4_DST_PA+y*1024u,1024),
          (const void *)G_PTR_RANGE(G4_SRC_PA+y*1024u,1024), 1024);
  __asm__ volatile("dsb sy" ::: "memory");
  elapsed = g4_counter()-start; d[40] = elapsed; d[41] = elapsed>>32;
  d[43] |= G4_CPU_VALID;
  bad = g4_verify(d, panel, 1); d[49] = g4_guard(panel, 0);
  if (gpu_extra[5][116]) return 0xe01;
  if (bad || d[49]) return 0xe0a;
 }
 if (g3_faulted()) return gpu_extra[5][116] ? 0xe01 : 0xe02;
 d[3] = 8;
 return 1;
}
static void gpu_m4(unsigned limit)
{
 for (unsigned stage = 14; stage <= 15 && stage <= limit; stage++) {
  uint32_t *d = gpu_extra[stage-3];
  if (!d[1]) {
   g_enter(stage); d[0] = 0xa7400009; d[1] = stage;
   d[2] = g4_copy(stage == 15, d); g4_snapshot(d);
   if (d[2] == 1 && gpu_extra[5][116]) d[2] = 0xe01;
   if (d[2] == 1 && g3_faulted()) d[2] = 0xe02;
  }
  if (d[2] != 1) return;
 }
}

#include "gpu-zap.h"

static void gpu_m3(unsigned limit)
{
 uint32_t *e = gpu_extra[5];
 if (!e[1]) { g_enter(11); e[1] = 1; e[2] = g3_memory(); }
 if (e[2] != 1 || limit < 12) return;
 if (!e[5]) {
  g_enter(12); e[5] = 1; e[3] = g3_cp_start();
  if (e[7] && (e[63] & GDSC_PWR_ON) && !(e[62] & 0xc3)) g3_diagnostics(12);
  if (e[3] == 1 && e[116]) e[3] = 0xc05;
 }
 if (e[3] != 1 || limit < 13) return;
 if (!e[6]) {
  g_enter(13); e[6] = 1; e[4] = g3_fence(); g3_diagnostics(13);
  if (e[4] == 1 && e[116]) e[4] = 0xd01;
 }
 if (e[4] != 1) return;
 if (limit == 16) { gpu_m5(); return; }
 /* A failed zap attempt permanently blocks all later 2D work this boot. */
 if (gpu_extra[13][1] && gpu_extra[13][2] != 1) return;
 if (limit >= 14) gpu_m4(limit);
}

static void gpu_m2(unsigned limit)
{
 if (!gpu_snapshot_valid && !g_snapshot()) return;
 if (limit == G_STAGE_8A) { g_live(); return; }
 if (!gpu_m2_attempted) { g_enter(8); gpu_m2_attempted = 1; g_memory_load(); }
 if (!gpu_s8_ok || limit < 9) { g_live(); return; }
 if (!gpu_s9_attempted) { g_enter(9); gpu_s9_attempted = 1; g_start(); }
 if (!gpu_s9_ok || limit < 10) { g_live(); return; }
 if (!gpu_s10_attempted) { g_enter(10); gpu_s10_attempted = 1; g_gx(); }
 if (gpu_extra[0][60] == 1 && limit >= 11) gpu_m3(limit);
 g_live();
}

static void gpu_probe(unsigned limit)
{
 uint32_t *r = gpu_report;
 /* Once stage8 has touched tables or retention, never replay power/fw setup.
  * Stages8->16 may advance once; reads of old stages return the frozen report. */
 if (gpu_snapshot_valid) {
  if (limit >= 8) gpu_m2(limit);
  r[1] = limit; r[2] = gpu_stage; r[3] = gpu_gmu_started;
  return;
 }
 for (int i = 0; i < GPU_REPORT_WORDS; i++) r[i] = 0;
 if (!limit) return;
 if (limit != G_STAGE_8A && limit > 16) limit = 16;
 r[0] = 0xa740;          /* magic */
 r[1] = limit;
 gpu_extra[0][0] = 0xa7400009;

 /* ---- Stage 1: read-only snapshot (always safe: same GCC family the UFS /
  * touch probes already read; GPUCC/SMMU/GPU reads are each gated). ---- */
 g_enter(1);
 uint32_t cfg_ahb = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB);
 uint32_t memnoc  = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_MEMNOC_GFX);
 uint32_t dvm     = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_SNOC_DVM_GFX);
 uint32_t vote    = g_rd(GPU_GCC_VOTE_PAGE, 0);
 r[4] = cfg_ahb; r[5] = memnoc; r[6] = dvm; r[7] = vote;
 uint32_t cx_gdscr = 0, gx_gdscr = 0, cc_ahb = 0, cc_smmu = 0, cc_gmu = 0;
 int cc_read = 0;
 if (cfg_ahb & CBCR_ENABLE) {         /* GPUCC needs gcc_gpu_cfg_ahb_clk enabled */
  cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
  gx_gdscr = g_rd(GPU_GPUCC_BASE, CC_GX_GDSCR);
  cc_ahb   = g_rd(GPU_GPUCC_BASE, CC_AHB_CLK);
  cc_smmu  = g_rd(GPU_GPUCC_BASE, CC_SMMU_VOTE);
  cc_gmu   = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
  cc_read = 1;
 }
 r[8] = cx_gdscr; r[9] = gx_gdscr; r[10] = cc_ahb; r[11] = cc_smmu; r[12] = cc_gmu;
 r[13] = cc_read;
 /* SMMU GR0 only if CX on and the HLOS vote clock runs. */
 if (cc_read && g_gdsc_on(cx_gdscr) && g_clk_on(cc_smmu)) {
  r[16] = g_rd(GPU_SMMU_BASE, SMMU_SCR0);
  r[17] = g_rd(GPU_SMMU_BASE, SMMU_ID0);
  r[18] = g_rd(GPU_SMMU_BASE, SMMU_ID1);
  r[19] = g_rd(GPU_SMMU_BASE, SMMU_ID2);
  r[20] = g_rd(GPU_SMMU_BASE, SMMU_GFSR);
  unsigned n = r[17] & 0xff, valid = 0, first = 0xffff;
  if (n > 128) n = 128;
  for (unsigned i = 0; i < n; i++) {
   uint32_t smr = g_rd(GPU_SMMU_BASE, SMMU_SMR(i));
   if (smr & (1u << 31)) { valid++; if (first == 0xffff) { first = i; r[22] = smr; r[23] = g_rd(GPU_SMMU_BASE, SMMU_S2CR(i)); } }
  }
  r[21] = valid | (first << 16);
  r[15] = 1;   /* smmu read */
 }
 /* GPU RBBM_STATUS only if CX+GX both on (GX-gated on A7XX). */
 if (cc_read && g_gdsc_on(cx_gdscr) && g_gdsc_on(gx_gdscr)) { r[24] = g_rd(GPU_REG_BASE, GPU_RBBM_STATUS * 4); r[14] = 1; }
 if (limit < 2) return;

 /* ---- Stage 2: the one write the gcc-sm8550 probe makes. ---- */
 g_enter(2);
 if (!(cfg_ahb & CBCR_ENABLE)) { g_wr(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB, cfg_ahb | CBCR_ENABLE); r[26] = 1; }
 cfg_ahb = g_rd(GPU_GCC_GPU_PAGE, GCC_GPU_CFG_AHB);
 r[27] = cfg_ahb;
 if (!(cfg_ahb & CBCR_ENABLE)) return;   /* nothing beyond is readable */
 if (limit < 3) return;

 /* ---- Stage 3: CX GDSC + GPUCC AHB + HLOS SMMU vote (gdsc_enable mirror). ---- */
 g_enter(3);
 cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
 r[28] = cx_gdscr;
 if (!g_gdsc_on(cx_gdscr)) {
  g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, cx_gdscr & ~GDSC_SW_COLLAPSE);
  delay(1);
  uint32_t st = 0; unsigned w;
  for (w = 0; w < 200; w++) { st = g_rd(GPU_GPUCC_BASE, CC_CX_GDS_HW); if (g_gdsc_on(st)) break; delay(10); }
  r[29] = st;
  if (g_gdsc_on(st)) {
   uint32_t v = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
   g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, v | GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH); delay(1);
   g_wr(GPU_GPUCC_BASE, CC_CX_GDSCR, v | GDSC_RETAIN_MEM | GDSC_RETAIN_PERIPH | GDSC_RETAIN_FF); delay(1);
  }
 }
 cx_gdscr = g_rd(GPU_GPUCC_BASE, CC_CX_GDSCR);
 r[30] = cx_gdscr;
 if (!g_gdsc_on(cx_gdscr)) return;
 g_set_bit0(GPU_GPUCC_BASE, CC_AHB_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_SMMU_VOTE);
 { unsigned w; for (w = 0; w < 50; w++) { cc_smmu = g_rd(GPU_GPUCC_BASE, CC_SMMU_VOTE); if (g_clk_on(cc_smmu)) break; delay(10); } }
 r[31] = g_rd(GPU_GPUCC_BASE, CC_AHB_CLK);
 r[32] = cc_smmu;
 r[33] = g_clk_on(cc_smmu);
 if (limit < 4) return;

 /* ---- Stage 4: now-powered adreno SMMU GR0 + CB0/CB1 snapshot (read-only). ---- */
 g_enter(4);
 if (g_clk_on(cc_smmu)) {
  r[36] = g_rd(GPU_SMMU_BASE, SMMU_SCR0);
  r[37] = g_rd(GPU_SMMU_BASE, SMMU_ID0);
  r[38] = g_rd(GPU_SMMU_BASE, SMMU_ID1);
  r[39] = g_rd(GPU_SMMU_BASE, SMMU_ID2);
  uint32_t id1 = r[38];
  unsigned pageshift = (id1 >> 31) ? 16 : 12;
  uint64_t gr1 = 1ull << pageshift;
  uint64_t pages = 1ull << (((id1 >> 28) & 7) + 1);
  /* CB0 (GPU user) and CB1 context base + CBAR: read only, no translation touched. */
  for (unsigned b = 0; b < 2; b++) {
   uint64_t cb = (pages + b) * gr1;
   r[40 + b * 4 + 0] = g_rd(GPU_SMMU_BASE, (uint32_t)(gr1 + 4 * b));          /* CBAR */
   r[40 + b * 4 + 1] = g_rd(GPU_SMMU_BASE, (uint32_t)cb);                      /* SCTLR */
   r[40 + b * 4 + 2] = g_rd(GPU_SMMU_BASE, (uint32_t)(cb + 0x58));             /* FSR */
   r[40 + b * 4 + 3] = g_rd(GPU_SMMU_BASE, (uint32_t)(cb + 0x30));             /* TCR */
  }
  r[35] = 1;
 }
 if (!r[35] || limit < 5) return;

 /* ---- Stage 5: GMU clock snapshot (read-only; a6xx_gmu_resume expects XO). ---- */
 g_enter(5);
 uint32_t pll1_mode = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
 uint32_t pll1_op   = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE);
 r[48] = pll1_mode; r[49] = pll1_op;
 r[50] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_L_VAL);
 r[51] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR);
 r[52] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR + 4);
 r[53] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR);
 r[54] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR + 4);
 r[55] = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
 r[56] = g_rd(GPU_GPUCC_BASE, CC_CXO_CLK);
 r[57] = g_rd(GPU_GPUCC_BASE, CC_HUB_CX_INT);
 r[58] = g_rd(GPU_GPUCC_BASE, CC_DEMET);
 r[59] = g_rd(GPU_GCC_GPU_PAGE, GCC_DDRSS_GPU_AXI);
 r[60] = g_rd(GPU_GCC_VOTE_PAGE, 0);
 /* GMU core state words, only if CX on and AHB + cx_gmu clocks already run. */
 if (g_gdsc_on(cx_gdscr) && g_clk_on(g_rd(GPU_GPUCC_BASE, CC_AHB_CLK)) && g_clk_on(r[55])) {
  r[61] = g_rd(GMU_DWORD(GMUD_CM3_SYSRESET), 0);
  r[62] = g_rd(GPU_CXMISC_BASE, CXMISC_TCM_RET_CNTL * 4);
  r[63] = g_rd(GPU_RSCC_BASE, RSCC_SEQ_BUSY_DRV0 * 4);
  r[64] = g_rd(GMU_DWORD(GMUD_HFI_CTRL_STATUS), 0);
  r[79] = 1;   /* gmu core regs were readable in stage 5 */
 }
 if (limit < 6) return;

 /* ---- Stage 6: GMU clock bring-up (a6xx_gmu_resume + gpucc probe order). ---- */
 g_enter(6);
 /* 1. GPLL0 GPU votes */
 vote = g_rd(GPU_GCC_VOTE_PAGE, 0);
 if ((vote & GPLL0_VOTES) != GPLL0_VOTES) { g_wr(GPU_GCC_VOTE_PAGE, 0, vote | GPLL0_VOTES); delay(10); }
 r[66] = 1;
 /* 2/3. Configure + enable PLL1 (and configure PLL0) unless PLL1 already runs. */
 int pll1_running = (pll1_op & PLL_RUN) && (pll1_mode & PLL_OUTCTRL);
 if (!pll1_running) {
  uint32_t m0 = g_rd(GPU_GPUCC_BASE, CC_PLL0 + PLL_MODE), o0 = g_rd(GPU_GPUCC_BASE, CC_PLL0 + PLL_OPMODE);
  if (!((o0 & PLL_RUN) && (m0 & PLL_OUTCTRL))) g_pll_configure(CC_PLL0, 0x1e, 0xbaaa);
  g_pll_configure(CC_PLL1, 0x16, 0xeaaa);
  uint32_t v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE, v | PLL_RESET_N);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE, PLL_RUN);
  unsigned w; int locked = 0;
  for (w = 0; w < 1500; w++) { if (g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE) & PLL_LOCK_DET) { locked = 1; break; } delay(1); }
  r[67] = w;
  if (!locked) { r[65] = 3; return; }
  v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_USER_CTL);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_USER_CTL, v | PLL_OUT_MASK);
  v = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
  g_wr(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE, v | PLL_OUTCTRL);
 }
 /* 4. hub_clk_src 200 MHz (GPLL0 sel5 div5)  5. gmu_clk_src 220 MHz (PLL1 sel3 div3) */
 if (g_rcg_set(CC_HUB_RCGR, 5, 5)) { r[65] = 4; return; }
 if (g_rcg_set(CC_GMU_RCGR, 3, 3)) { r[65] = 5; return; }
 /* 6. branches: CXO_AON, DEMET, AHB, CX_GMU, CXO, GCC axi, GCC memnoc, HUB_CX_INT; wait cx_gmu */
 g_set_bit0(GPU_GPUCC_BASE, CC_CXO_AON);
 g_set_bit0(GPU_GPUCC_BASE, CC_DEMET);
 g_set_bit0(GPU_GPUCC_BASE, CC_AHB_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_CX_GMU_CLK);
 g_set_bit0(GPU_GPUCC_BASE, CC_CXO_CLK);
 g_set_bit0(GPU_GCC_GPU_PAGE, GCC_DDRSS_GPU_AXI);
 g_set_bit0(GPU_GCC_GPU_PAGE, GCC_GPU_MEMNOC_GFX);
 g_set_bit0(GPU_GPUCC_BASE, CC_HUB_CX_INT);
 { unsigned w; uint32_t cg = 0; int on = 0;
   for (w = 0; w < 500; w++) { cg = g_rd(GPU_GPUCC_BASE, CC_CX_GMU_CLK); if (g_clk_on(cg)) { on = 1; break; } delay(1); }
   r[68] = cg; r[69] = w; if (!on) { r[65] = 6; return; } }
 r[70] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_MODE);
 r[71] = g_rd(GPU_GPUCC_BASE, CC_PLL1 + PLL_OPMODE);
 r[72] = g_rd(GPU_GPUCC_BASE, CC_GMU_RCGR + 4) & 0x7ff;
 r[73] = g_rd(GPU_GPUCC_BASE, CC_HUB_RCGR + 4) & 0x7ff;
 /* GMU core now readable: cold-start preconditions (SYSRESET==1, TCM_RET==0, RSC idle, HFI not ready). */
 r[74] = g_rd(GMU_DWORD(GMUD_CM3_SYSRESET), 0);
 r[75] = g_rd(GPU_CXMISC_BASE, CXMISC_TCM_RET_CNTL * 4);
 r[76] = g_rd(GPU_RSCC_BASE, RSCC_SEQ_BUSY_DRV0 * 4);
 r[77] = g_rd(GMU_DWORD(GMUD_HFI_CTRL_STATUS), 0);
 r[65] = 7;   /* all six steps passed */
 if (limit < 7) return;

 /* ---- Stage 7: cmd-db read + parse (read-only DRAM; needed for GMU votes). ---- */
 g_enter(7);
 for (uint64_t i = 0; i < GPU_CMDDB_BYTES / 4; i++)
  { uint32_t w = g_rd(GPU_CMDDB_BASE, (uint32_t)(i * 4)); gpu_cmddb[i*4]=w; gpu_cmddb[i*4+1]=w>>8; gpu_cmddb[i*4+2]=w>>16; gpu_cmddb[i*4+3]=w>>24; }
 r[80] = cdb32(gpu_cmddb);        /* version */
 r[81] = cdb32(gpu_cmddb + 4);    /* magic (expect 0x0c0330db) */
 /* slave table: 16-byte rows from +8; resolve named resources' addresses. */
 static const char names[8][8] = {
  {'g','f','x','.','l','v','l',0},{'m','x','.','l','v','l',0,0},{'c','x','.','l','v','l',0,0},
  {'g','m','x','c','.','l','v','l'},{'S','H','0',0,0,0,0,0},{'M','C','0',0,0,0,0,0},
  {'A','C','V',0,0,0,0,0},{'C','N','0',0,0,0,0,0}};
 unsigned rows = 0; size_t heads[8] = {0}, sizes[8] = {0};
 for (unsigned i = 0; i < 8; i++) {
  const uint8_t *row = gpu_cmddb + 8 + 16 * i; unsigned id = cdb16(row), cnt = cdb16(row + 6);
  if (!id) break;
  heads[i] = 144u + cdb16(row + 2); sizes[i] = (size_t)cnt * 24; rows++;
 }
 r[82] = rows;
 for (unsigned i = 0; i < rows; i++)
  for (size_t at = heads[i]; at + 24 <= heads[i] + sizes[i] && at + 24 <= GPU_CMDDB_BYTES; at += 24) {
   const uint8_t *e = gpu_cmddb + at;
   for (unsigned nm = 0; nm < 8; nm++) {
    int eq = 1; for (unsigned c = 0; c < 8; c++) if (e[c] != (uint8_t)names[nm][c]) { eq = 0; break; }
    if (eq) r[83 + nm] = cdb32(e + 16);   /* resource address */
   }
  }
 if (limit >= 8 && r[65] == 7 && r[80] == 1 && r[81] == 0x0c0330db && r[82]) {
  g_enter(limit == G_STAGE_8A ? G_STAGE_8A : 8);
  gpu_m2(limit);
 }
 r[2] = gpu_stage; r[3] = gpu_gmu_started;
 /* r[83..90] = gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0 addresses (0 = absent). */
}
