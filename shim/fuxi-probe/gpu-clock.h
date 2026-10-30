/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ported from UniXNU kits/AdrenoFirmware/AdrenoHfiVotes.c
 * (BSD-2-Clause-Patent). See LICENSE.AdrenoFirmware.
 * BCM aux decoding, vote arithmetic and VCD commit/wait grouping adapted to
 * our 160-word HFI packet. Board policy below is Fuxi hardware data, not sheng
 * OPPs. No command-db address, unit, width or VCD is invented.
 */
#ifndef FUXI_GPU_CLOCK_H
#define FUXI_GPU_CLOCK_H
#define G_GPU_LEVEL_MAX 8u /* 680 MHz; 719 MHz is SKU-specific */
#define G_GPU_DDR_MAX 8u /* DT index 9 reaches ACV performance mode */
static const uint32_t g_gpu_freq[10] = {0,220000,295000,348000,401000,475000,550000,615000,680000,719000};
static const unsigned g_gpu_level[10] = {0,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};
/* Legacy HFI rows, distinct from DT bus-table-ddr indices. */
static const uint32_t g_gpu_ddr[3] = {0,2136718,6074218};
/* Fuxi DT facts, shared by both bins through 680 MHz. Exclude DDR index 9
 * (16500000 kB/s): ACV stays valid-only, below performance-mode threshold. */
static const uint32_t g_gpu_ddr_dt[9] = {0,2136718,3000000,6074218,6671875,8171875,10687500,12449218,14398437};
static const unsigned g_gpu_bus_max[9] = {0,3,3,3,4,5,7,7,9};
static unsigned g_gpu_select(uint64_t requested)
{
 return requested <= G_GPU_LEVEL_MAX ? requested : 0;
}
static unsigned g_gpu_bw_index(unsigned selected)
{
 return selected ? (selected == 1 ? 1 : 2) : 0;
}
/* Zero means no effective override, including for legacy/220 MHz policy.
 * Keep wide invalid inputs from wrapping into an accepted DT index. */
static unsigned g_gpu_ddr_clamp(unsigned selected, uint64_t requested)
{
 if (selected < 2 || selected > G_GPU_LEVEL_MAX || requested > G_GPU_DDR_MAX) return 0;
 return requested > g_gpu_bus_max[selected] ? g_gpu_bus_max[selected] : requested;
}
static uint32_t g_gpu_ddr_row2(unsigned selected, uint64_t requested)
{
 unsigned index = g_gpu_ddr_clamp(selected, requested);
 /* Preserve every old row for levels 0..6. Newly selectable 615/680 MHz
  * need their DT bus-freq 6/7 floors, rather than the old DT index 3. */
 if (!index) index = selected == 7 ? 6 : selected == 8 ? 7 : 3;
 return g_gpu_ddr_dt[index];
}
struct g_bcm { uint32_t address, unit; unsigned width, vcd; };
/* Caller supplies exactly SH0, MC0, ACV, in that order, plus actual CN0.
 * Do all validation before touching output. ACV is fixed valid-only here;
 * proportional SH0/MC0 bus widths on SM8550 are 16/4 bytes. */
static int g_gpu_bw_packet(const struct g_bcm b[3], uint32_t cn0, uint32_t row2_kbps, uint32_t out[160])
{
 if (!cn0 || (cn0 & 3) || ((cn0>>16)&7) != 5) return 0;
 for (unsigned j = 0; j < 3; j++) {
  if (!b[j].address || (b[j].address & 3) || ((b[j].address>>16)&7) != 5 ||
      b[j].address == cn0 || b[j].vcd > 15 ||
      (j < 2 && (!b[j].unit || !b[j].width))) return 0;
  for (unsigned k = 0; k < j; k++) if (b[k].address == b[j].address) return 0;
 }
 /* HFI header + counts/masks + CNOC addr[6], data[2][6] + DDR addr[8],
  * data[16][8]. No direct host RPMh transaction; the GMU sends these. */
 for (unsigned i = 0; i < 160; i++) ((volatile uint32_t *)out)[i] = 0;
 out[1] = 3; out[2] = 1; out[3] = 3; out[4] = 1;
 out[6] = cn0; out[12] = 0x40000000; out[18] = 0x60000001;
 for (unsigned j = 0; j < 3; j++) {
  out[24+j] = b[j].address;
  unsigned commit = j == 2 || b[j].vcd != b[j+1].vcd;
  if (commit) out[5] |= 1u<<j;
  for (unsigned i = 0; i < 3; i++) {
   uint32_t data = commit ? 0x40000000 : 0;
   uint32_t kbps = i == 2 ? row2_kbps : g_gpu_ddr[i];
   if (kbps) {
    data |= 0x20000000;
    if (j < 2) {
     uint64_t peak = (uint64_t)kbps*b[j].width/(j ? 4 : 16);
     peak = peak*1000/b[j].unit;
     /* With matching aux widths 16/4, the largest allowed DDR rate fits
      * when unit >= 878811 (integer floor). Smaller boot DB units saturate:
      * cap each field without spilling into valid/commit bits. */
     uint32_t vote = peak > 0x3fff ? 0x3fff : peak ? (uint32_t)peak : 1;
     data |= vote<<14 | vote;
    }
   }
   out[32+i*8+j] = data;
  }
 }
 return 1;
}
#endif
