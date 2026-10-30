# P17: opt-in probes for the silent WLAN_MODE (WCN7850)

Status: host-tested only. Every device effect is **not yet observed**.
Blocker context: firmware reaches AMSS, INI and CFG reply OK, WLAN_MODE
(0x22) is consumed and never answered (see wlan-w16.md, wlan-bt.md).

All behaviour is off by default. The switches are bits of the existing
`fuxiwlanmlo` boot-arg; the low two bits keep their W12 meaning
(1 = PHY_CAP negotiation, 2 = omit MLO). No new boot-arg is used.

| bit | name | effect |
|---|---|---|
| 0x004 | HOSTMEM | HOST_CAP gets vendor TLV 0x33, u8 value 1 ("host memory on demand"), appended after 0x27. Bytes: `33 01 00 01`. Needs W12+ (`fuxiwlanqrtr>=2`). |
| 0x008 | MLOALL | HOST_CAP MLO TLVs (0x21..0x26) are sent whatever PHY_CAP said. Links = PHY num_phy if 1..2, else 2 (num_phy 1 with support 0 gives 1). Without the bit, num_phy > 2 is refused as before. Invalid with low bits 2. |
| 0x010 | PHY17 | PHY_CAP reply TLV 0x17 (width unknown) is kept as length plus up to 8 little-endian bytes and logged. Needs low bits 1 (the PHY_CAP request). |
| 0x020 | DMAALIGN | every `wl_dma_alloc` (not the SMMU tables) puts its IOVA on size rounded up to a power of two. W13+ already did this for targets and M3. |
| 0x040 | CLKCHECK | before any WLAN power write (step 1), read PMK8550 gpio3 (SID 0, PPID 0x0ba) over SPMI arbiter v7 and log it. Read only. Works with plain `fuxiwlan=N`, no QMI needed. |
| 0x080 | CLKON | as CLKCHECK, then if the pin is not an enabled function-2 output, EE 0 owns it and it is an LV/MV pin: write 0x42=5, 0x41=0, 0x44=2, 0x40=1, 0x46=0x80 on its RW channel and read back. Volatile PMIC state. |
| 0x100 | ERRDEC | wherever the WLFW diagnostics print the BHI words (W13 RESPOND_MEM, W15 MODE timeout), also classify ERRCODE/ERRDBG1..3; if the EE is RDDM and the RX sink exists (`fuxiwlanvec=rddm`), wait up to 1 s for RXVECSTATUS complete and scan the dump for assert/fatal text. |

Valid range is 0..0x1ff with low bits != 3. A suggested first run is
`fuxiwlanmlo=0x17d` (PHY_CAP, 0x33, MLOALL, PHY17, DMAALIGN, CLKCHECK, ERRDEC)
at `fuxiwlanqrtr=5`; add 0x80 only after the CLKCHECK line shows state 0.

## Log lines to look for

- `FUXI WLAN: W12 mlo variant N ... P17 bits 0x... (hostmem 1 ...) valid 1` (valid 0 means the combination was refused)
- `FUXI WLAN: P17 sleepclk pmk8550 gpio3 ppid 0xba apid A owner eeO read 1 type 0x10 sub 0x1x mode M func F en E state S`
  state 0xff = already an enabled output of function 2, 0 = off, 3 = not ours, 4 = other layout, 0xfe = unreadable.
  Healthy expectation if the bootloader set it up: `mode 0x1 func 0x2 en 0x80 state 0xff`.
- `FUXI WLAN: P17 sleepclk enable writes ok 1 after: read 1 mode 0x1 func 0x2 en 0x80 state 0xff` (CLKON) or `enable skipped (state ...)`
- `FUXI WLAN: P17 host cap vendor TLV 0x33 host-memory-on-demand = 1 appended after 0x27`
- `FUXI WLAN: W12 host cap MLO TLVs present num_phy N` (with MLOALL, also when PHY_CAP said support 0)
- `FUXI WLAN: P17 phy cap TLV 0x17 len L value 0x...`
- `FUXI WLAN: P17 dma align size 0x.. align 0x.. IOVA 0x.. -> 0x..` (one per allocation; "target allocation/NC/map failed" would mean the pool ran out of space)
- `FUXI WLAN: P17 <label> ERRCODE 0xcafecace hi16 0xcafe ... class: ...` (and ERRDBG1..3), `P17 rddm upload RXVECSTATUS ...`, `P17 rddm text +0x...: ...`, `P17 rddm scan ... messages`, `P17 rddm-rec 000: ...`

The success discriminator is unchanged: a matching successful MODE reply.

## Provenance and caveats

- Sleep-clock pin sequence, power-of-two IOVA rule and the RAM-dump text
  scan are adapted from UniXNU-sheng `package/WifiProbe.cpp` (`pmicPrepare`,
  `pmicSleepClk`, `arenaAlloc`, `rddmScan`, `looksLikeError`), commit
  `08cde20edc3254ed6f7df8d0f5384fb887786e8f`, SPDX BSD-2-Clause-Patent,
  Copyright (c) 2026 UniXNU contributors. Notice kept in the P17 header of
  `xnu/fuxi-wlan/fuxi_wlan.c`. SPMI access mirrors `fuxi_keys.c`
  (layout: docs-lanes/buttons.md) and takes `sweet_spmi_bus_lock`.
- UniXNU-sheng contains no WLFW/QMI code. TLV 0x33 (u8 1) and the PHY_CAP
  TLV 0x17 come from the lane brief, not from a checked source; the 0x17
  width is unknown, hence the width-agnostic decode. The MLO TLV encoding
  is this file's existing HOST_CAP codec (pinned by the W12 tests), not a
  sheng encoding. Nothing was taken from Ath12kWifi or Linux for P17.
- The gpio3 PPID 0x0ba (PMK8550 gpio block 0xb800 + 0x100 per pin) is
  sheng's reading of the shared SM8550 description; the first device
  evidence is the CLKCHECK line.
- The RDDM scan only helps if the device actually enters RDDM. In the
  observed MODE silence the EE stays AMSS and SYSERR 0, so ERRDEC will most
  likely only print the classified words.
- The error word class names state a bit pattern only; the meaning of
  0xcafecace is unknown.

## Host tests

`tools/test-fuxi-wlan-w17.py` (also in `tools/ci/build.sh`) compiles the pure
helpers with ASan/UBSan (link choice table, PHY 0x17 decode, alignment,
sleep-clock decision and exact write order, exact error-word strings,
text-scan hits/dedup/format rejection), then reruns the W12 QMI harness
with 12 added cases whose HOST_CAP bytes (including `33 01 00 01` after
`27 08 00 ...`) are compared exactly, plus selector validity, IOVA alignment
in `wl_dma_alloc` and the ERRDEC log lines. SPMI MMIO and the RDDM wait are
not host-testable. 0099 is regenerated from `xnu/fuxi-wlan/fuxi_wlan.c`
(mirror check inside the m6/m6o tests).
