# GPU-CLOCK: fixed A740 level and DDR votes

2026-10-25. Offline source audit and opt-in DDR/680 MHz extension. No phone access, deployment,
push, CI/workflow, firmware or reference build. Canonical driver sources remain
in `xnu/fuxi-adreno/` and `shim/fuxi-probe/`; regenerated **0098** overlays the
standalone 0096 baseline. Metal, external metal2vulkan, external Mesa/Turnip,
NativeAdreno ABI 2, admission and display ownership contracts are unchanged.

## What the current path actually requests

`fuxibbgpu=1` admits backboardd; it does not select clocks. With the existing
`fuxigpu=1 fuxigpuabi=1` setup, `fuxi_gpu_start()` runs the shared `gpu_probe()`
cold-start path. Neither Metal nor the ABI submit path subsequently changes
GPU/GMU performance levels. This is fixed startup policy, with no GPU DVFS or
thermal governor. Successful HFI ACKs prove acceptance, not measured execution
frequency or voltage. The supplied frame timings alone cannot identify either.

| Owner / step | Legacy configuration (no level/DDR options) |
| --- | --- |
| GPU HFI PERF_TABLE, stage 9 | OFF, 220, 295, 348, 401, 475, 550, 615, 680, 719 MHz, ascending indices 0..9. ARC levels 0, 0x34, 0x38, 0x3c, 0x40, 0x50, 0x80, 0x90, 0xc0, 0xe0. Top is historically rounded to 719 MHz in this table. |
| GX voltage vote | `g_arc_vote()` resolves `gfx.lvl` and `gmxc.lvl` (otherwise `mx.lvl`) from the actual boot command DB. Packed word is primary level <<16, secondary index <<8, primary index. It is an ARC level/index request, **not a voltage readback**. |
| GPU GX_BW_PERF_VOTE, stage 10 | Blocking HFI command 30, GPU index **1**, DDR index **0**. No later higher vote. |
| DDR BW_TABLE | One **off** row for SH0/MC0/ACV, each data word 0x40000000, DDR wait mask 7. Addresses come from command DB. No active DDR throughput request. Other inherited/shared SoC users can still keep DDR running. |
| CNOC | One CN0 command, two rows: 0x40000000 off, 0x60000001 on, wait mask 1. This is distinct from DDR bandwidth. |
| GMU HFI table | OFF/200000/500000 kHz; cx.lvl/mx.lvl ARC levels 0/0x40/0x80. The host does **not** explicitly select a GMU table index. Advertising an entry does not select it. |
| GMU physical clock, stage 6 | GPUCC PLL1 L=0x16, alpha=0xeaaa, 19.2 MHz XO: about 440 MHz. GMU RCGR source3, HID=3 divides by 2: **about 220 MHz**, despite the 200 MHz HFI low entry. Existing running PLLs are retained. No host switch to the 500 MHz table entry. |
| Other clocks / rails | GPLL0 GPU votes bits15/16; hub source5/HID5 gives 200 MHz from GPLL0/3. Enables AHB, SMMU vote, CX_GMU, CXO, DEMET, HUB_CX_INT and GCC AXI/memnoc branches. CX GDSC is brought up with retention. |
| PLL0 / GX | Cold startup configures PLL0 L=0x1e/alpha=0xbaaa if not already running, about 590 MHz **source** configuration. GMU firmware owns the actual GX clock/rail transition following the HFI GPU vote; the host does not force that PLL configuration as the GPU rate. |
| RPMh / power retention | `g_rpmh_init()` programs RSC sequence/hidden data and enables AOP-owned PDC without replacing its code. HFI supplies ARC/BCM votes for the GMU to issue; no independent host DDR RPMh client is implemented. GPU_SET OOB is held from CP startup through reboot, with no runtime idle-collapse path. ACD/IFPC policy is unchanged. |

Thus **220 MHz was not a stale requested GPU rate**. It was a hardcoded,
unmeasured claim in the ready line, and happened also to describe the separate
GMU physical clock. The replacement says `requested 220000 kHz` (or the chosen
rate); new diagnostics keep these concepts separate. Neither firmware throttling
nor actual GX rail voltage can be established by this offline audit.

## UniXNU comparison and port

Audited the supplied UniXNU superproject
`fb6aad6ab1f50199a4d99d2da1d0ca403b067633`, NativeAdreno
`cedcaa2949c795e71a47e78ae413c8a41b17d7d0`, AdrenoFirmware
`40fdb8ea87606612ac6f1e80bbb49955d0ce4051`, and the adjacent UniXNU-sheng board
checkout `8a0c9ddab92038a50abc07fffd4882ec4e301f45`. Read-only text inspection;
no reference scripts, imports, tests, builds or firmware were run.

NativeAdreno's `NativeAdrenoService.cpp` gets board-generated startup policy
through `ShengGmuBuildStartup()`. The operational board path in
`package/GpuPowerProbe.cpp` sends `{blocking=1, GPU=1, BW=1}` after cold start:
**220 MHz plus active bandwidth**, not an automatic high GPU clock. Its
`GpuPowerBoard.h` uses the same PLL1/GMU/hub clock initialization as ours.
`package/ShengGmuVotes.c` and `scripts/prepare-sheng-gmu-votes.py` construct the
GPU/GMU tables from their enabled OPPs and active bandwidths. Their shared
`AdrenoHfiVotes.c` decodes BCM aux data, computes proportional SH0/MC0 votes,
sets ACV valid/performance bits and groups commit/wait bits by VCD.

Ported the missing **BCM aux/arithmetic/VCD-grouping** logic, adapted to our
160-word packet, in `gpu-clock.h` (BSD-2-Clause-Patent, source SPDX/provenance
header retained; exact upstream notice in `LICENSE.AdrenoFirmware`). README's
single UniXNU Credits entry includes this port. No GPL implementation or board
OPP generator is copied, and no Mesa/translator source is vendored.

Fuxi policy is deliberately its own: both DT power bins share the 220..680 MHz
frequencies and ARC levels used here; only bin 0 additionally has 719 MHz.
Hardware facts from Fuxi's DT: `qcom,bus-table-ddr` entry1 = **2136718 kB/s**,
entry3 = **6074218 kB/s**; 220 MHz uses entry1 and 295..550 MHz use entry3.
615/680 MHz use DT entries 6/7. Our HFI table deduplicates the active bandwidths
into rows 1 and 2; row 2 carries the selected higher-level policy. SM8550
SH0/MC0 proportional bus widths are 16/4 bytes. All selectable bandwidths are below ACV's performance
threshold (16500000 kB/s), so this bounded profile uses ACV valid-only. No sheng
OPP rates, rail addresses, command-db units, widths, VCDs or captured memory
are embedded. All resource addresses and BCM aux metadata come from this boot's
command DB. Missing/duplicate/malformed BCMs, zero proportional unit/width,
invalid address/slave/VCD or aux lengths refuse startup **before CM3 release**
with HFI stage result `0x90a`; the existing permanent CPU fallback applies.

This ports active bandwidth only when explicitly requested. Absent/zero
`fuxigpulevel` keeps the original one-row bandwidth packet and vote unchanged,
including when `fuxigpuddr` is present. No
ACD/IFPC enable, warm resume, runtime level changes, automatic recovery, new
regulator driver or thermal governor is added.

## Options

Keep existing GPU/ABI/backboardd prerequisites. New arguments do nothing when
`fuxigpu` is disabled.

| `fuxigpulevel=N` | GPU index / requested MHz / ARC level | HFI BW index / default kB/s | DT bus-freq / bus-max |
| --- | --- | --- | --- |
| absent or 0 | 1 / 220 / 0x34, legacy policy | 0 / 0, legacy off row | 1 / 3 (220 MHz DT) |
| 1 | 1 / 220 / 0x34 | 1 / 2136718; bandwidth-only comparison | 1 / 3 |
| 2 | 2 / 295 / 0x38 | 2 / 6074218 | 3 / 3 |
| 3 | 3 / 348 / 0x3c | 2 / 6074218 | 3 / 3 |
| 4 | 4 / 401 / 0x40 | 2 / 6074218 | 3 / 4 |
| 5 | 5 / 475 / 0x50 | 2 / 6074218 | 3 / 5 |
| 6 | 6 / 550 / 0x80 | 2 / 6074218 | 3 / 7 |
| 7 | 7 / 615 / 0x90 | 2 / 10687500 | 6 / 7 |
| 8 | 8 / 680 / 0xc0 | 2 / 12449218 | 7 / 9 (override capped at 8) |
| outside 0..8, including negative numeric values | rejection logged, legacy 0 | legacy 0 | no override |

Parsing uses a 64-bit destination so negative and wide values cannot wrap into
a valid 32-bit level. These are ascending **HFI** indices, not descending
Android KGSL power-level numbers. Level 9 / 719 MHz / ARC 0xe0 remains advertised
in the unchanged PERF_TABLE but cannot be selected: it is SKU-specific.
615/680 MHz are present in both bins. Their new default bandwidths follow DT
`bus-freq` 6/7 because the old DT index 3 floor belongs to 295..550 MHz;
reusing it at these higher levels would understate their DT bandwidth policy.
This is no proof of thermal safety. With absent/zero `fuxigpuddr`, every level
0..6 retains its original PERF_TABLE, BW_TABLE and GX vote bytes.

`fuxigpuddr=N` independently selects a **DT** `bus-table-ddr` index 1..8.
It applies only with a selected level >= 2 and changes HFI BW **row 2**, leaving
row 1, table counts, masks, CNOC, PERF_TABLE and GX BW index unchanged. Absent/0
uses the level's default above; levels 0/1 ignore the override. For a valid
nonzero request, the effective index is `min(N, selected level's bus-max)`.
Requests above 8 (including negative/wide numeric inputs) are rejected and
use the level's default; they never wrap or select index 9.

| DT DDR index | kB/s | Override policy |
| --- | --- | --- |
| 0 | 0 | No override |
| 1 | 2136718 | Allowed |
| 2 | 3000000 | Allowed |
| 3 | 6074218 | Allowed; legacy HFI row 2 value |
| 4 | 6671875 | Allowed, subject to bus-max |
| 5 | 8171875 | Allowed, subject to bus-max |
| 6 | 10687500 | Allowed, subject to bus-max |
| 7 | 12449218 | Allowed, subject to bus-max |
| 8 | 14398437 | Allowed only without a lower bus-max clamp |
| 9 | 16500000 | Forbidden: ACV performance-mode threshold |

For example, level 6 plus DDR 8 clamps to DT index 7 / 12449218 kB/s; level 2
plus DDR 8 clamps to index 3. Smaller explicit indices can reduce the default
floor, so they are experimental too. `ddr_requested` logs the raw requested DT
index; `ddr_clamped` logs the effective override, or 0 for absent/zero, rejected
or ignored requests. `ddr_kBps` always reports the selected bandwidth policy,
including the level 7/8 defaults when `ddr_clamped=0`.

BCM conversion remains integer `peak = floor(kBps * aux_width / bus_width)`,
then `vote = floor(peak * 1000 / unit)`, with minimum nonzero vote 1 and each
AB/IB field capped independently at **0x3fff**. SH0/MC0 bus widths are **16/4**;
aux width/unit come from the boot command DB. For matching aux widths 16/4,
unit 1000000 produces unsaturated votes 2136, 3000, 6074, 6671, 8171, 10687,
12449 and 14398 for DT indices 1..8. The largest index is unsaturated for
unit >= **878811** after integer truncation; smaller units can saturate. A
synthetic unit 1000 saturates even the old rates. Wider aux widths can also
saturate; the existing 64-bit arithmetic and per-field cap remain in force,
without spilling into valid/commit bits. No on-device BCM unit or achieved
DDR rate is claimed: `ddr_kBps` is the policy input, and a saturated vote can
represent less bandwidth. ACV remains valid-only for all active rows; no
performance bit is set and DT index 9 is never voted.

`fuxigpuclock=1` optionally borrows **RBBM counter3** before display/ABI
publication, selects ALWAYS_COUNT (0), enables the global counter bit and
samples once over about 1 ms against the ARM architectural timer. It uses
bounded high/low/high reads and restores selector/global enable on exit. The
startup sample consumes that counter's prior history. It never submits work,
disables HWCG, changes a PLL or adds periodic MMIO. A stopped, unstable,
faulting or implausible sample reports unavailable. Diagnostics use independent
fault results rather than poisoning the driver MMIO-failure latch.

**Counter rate can be lower than core clock under clock gating.** A successful
sample measures RBBM count rate over that startup interval, not guaranteed
sustained GPU MHz under load. OOB/GX/CX prerequisites are checked first; an
asynchronous bus fault is still a hardware qualification risk. Without this
argument, only GPUCC readbacks are added: raw PLL0/PLL1 and GMU RCGR, and a
register-derived GMU clock. Unsupported source/running-state readbacks give
`gmu_derived_khz=0` (unknown), not a claimed 0 Hz operating point.

## Expected logs (formats, not observed results)

For `fuxigpulevel=4`, after successful cold startup, where `<...>` denotes a
boot-dependent lowercase hexadecimal readback as printed by `%#x`:

```text
FUXI GPU CLOCK: level=4 gpu_index=4 requested_khz=401000 gx_arc=<vote> bw_index=2 ddr_kBps=6074218 ddr_requested=0 ddr_clamped=0 hfi_ack=1
FUXI GPU CLOCK: GMU table_khz=0/200000/500000 cx_arc=<off>/<low>/<high> (no host GMU perf-index vote)
FUXI GPU CLOCK: PLL0 mode=<mode> l=<l> alpha=<alpha> PLL1 mode=<mode> l=<l> alpha=<alpha> gmu_rcgr=<cfg> gmu_derived_khz=<rate> core_measured_khz=unavailable
FUXI GPU: ready, BGRA strided rects + one fence, 4 cached backings, requested 401000 kHz GPU vote (not measured), async one-in-flight, no timer sleep
```

On the programmed PLL1 L=0x44440016, alpha=0xeaaa, GMU CFG=0x303 path,
`gmu_derived_khz=219999` (integer truncation of about 220 MHz). Inherited clocks
may produce different readbacks. An access failure replaces the PLL line with:

```text
FUXI GPU CLOCK: gpucc_readback=unavailable core_measured_khz=unavailable
```

With `fuxigpuclock=1`, a readable/powered GPUCC snapshot additionally emits one
of these before the ready line; decimal fields depend on the actual sample:

```text
FUXI GPU CLOCK: rbbm_count_khz=<rate> sample_us=<elapsed> (clock-gated sample)
FUXI GPU CLOCK: rbbm_count_khz=unavailable
```

The absent/zero default first line has `level=0 gpu_index=1 requested_khz=220000`,
`bw_index=0 ddr_kBps=0 ddr_requested=0 ddr_clamped=0`; ready says `requested 220000 kHz GPU vote (not measured)`. An invalid example:

```text
FUXI GPU CLOCK: rejected fuxigpulevel=9 (allowed 0..8); legacy level=0
```

For `fuxigpulevel=6 fuxigpuddr=8`, the first line is:

```text
FUXI GPU CLOCK: level=6 gpu_index=6 requested_khz=550000 gx_arc=<vote> bw_index=2 ddr_kBps=12449218 ddr_requested=8 ddr_clamped=7 hfi_ack=1
```

Invalid DDR input, for example index 9, separately logs:

```text
FUXI GPU CLOCK: rejected fuxigpuddr=9 (allowed 0..8); level bandwidth default
```

The existing five-second backboardd periodic line appends one decimal field:

```text
FUXI MTL: backboardd frames=<total> gpu_ms=<mean> late=<total> failures=<total> hz=<rate> window_frames=<count> gpu_busy_est_pct=<percent>
```

`gpu_busy_est_pct` = completed compositor GPU timestamp durations / reporting
wall time, clamped to 100.0; -1.0 means frames completed without timestamp data.
No-frame windows report 0.0. It estimates compositor occupancy, not whole-engine
hardware utilization: timestamp spans include GPU waits, completions can cross
window boundaries, and other clients/unretired work are excluded. It reuses
existing timestamps and introduces no timer, counter reservation or GPU wait.

## Validation and unverified behavior

Host checks (no phone or reference execution):

* `python3 tools/sync-fuxi-adreno-shared.py --check` (also checks 0098/0100).
* `python3 tools/test-fuxi-gpu-probe.py`: actual HFI generation/startup and fixed
  votes for all nine selections crossed with absent/zero, every DDR index,
  rejected 9/negative/wide inputs and two BCM units; complete legacy/active
  packets, unchanged PERF_TABLE, bus-max clamps, VCD groups, proportional
  floor/saturation boundaries and maximum aux width, invalid metadata refusal
  before CM3 release, existing HFI ACK/error/wrap/timeout, CP/SMMU/blit/protocol
  regression.
* `python3 tools/test-fuxi-adreno.py`: mapping/startup wide/negative level
  and DDR parsing/rejection, absent/zero defaults, bus-max clamps and ready
  rates, existing submit/cache/SCM tests plus `test-fuxi-gpu-clock.py`.
  Clock test compiles the actual diagnostic functions with ASan/UBSan models
  and as AArch64 ELF / arm64 Darwin objects; validates readback derivation,
  counter rollover/deadlines, faults/zero samples, selector/enable restore
  and DDR requested/clamped/default diagnostics. `test-fuxi-gpu-scanout.py`
  also compiles the complete driver/header integration in its host model.
* `python3 tools/test-fuxi-metal.py`: ABI/backend/register/cache/containment and
  real 0098/0100/0102 application against baseline snapshots, canonical bytes.
* `python3 tools/test-fuxi-metal-frontend.py`: portable frontend/boot-gate/storage
  regressions; native Objective-C/Metal cases require macOS and are skipped.
* Actual shared loader `probe.c` compiled as freestanding AArch64 ELF with its
  loader flags and `-Wall -Wextra -Werror`, without linking/packing firmware.

No full XNU or signed Metal/macOS build, device execution, applied active BCM
vote, rail measurement, sustained-frequency result, thermal improvement or
frame-rate improvement is claimed. The original 220 MHz request and missing
active DDR vote are source findings, not proof those explain all observed
8–20 ms GPU frames or late frames. Compile/host arithmetic cannot establish
GMU firmware/BCM ownership, speed-bin correctness, physical rail safety or
counter behavior under real HWCG. Existing OOB retention and absent GPU thermal
control remain significant limits.

## Thermal estimate and later device test plan

**Guess:** sustained 401–680 MHz plus a raised DDR floor will increase GPU/DDR
power and temperature under continuous composition. Dynamic GPU power roughly
scales with frequency times voltage squared, so going from 220 to 401/550 MHz
could cost roughly 2–4 times GPU dynamic power at equal utilization, depending
on rail voltage. Total phone power does not scale by that factor; reduced GPU
execution time may offset part of it. No credible degrees-C estimate exists
without measurements. The fixed level and DDR floor remain requested at idle,
and CPU thermal controls do not constitute a GPU thermal governor. Levels
7/8 request higher ARC corners (0x90/0xc0); shared SKU support and staying below
ACV performance mode do not establish safe sustained temperatures. A DDR floor
can increase idle memory power even if rendering becomes faster.

The supplied device observation at level 6 is **7–11 ms GPU time** for about
100 draws on a 1080x2400 surface. **DDR bandwidth limited is a guess.** Raising
only DDR at fixed 550 MHz is the useful first comparison; it cannot promise a
frame-time improvement, and saturated BCM fields may make different requests
encode identically.

For a separately authorized device session:

1. Build the changed kernel and FuxiMetal using the normal build/signing process;
   keep the existing matching ABI-2 ICD/external translator/root and admission
   setup. Save hashes and the pre-change image for immediate rollback. No root
   or staging action was performed by this task.
2. Boot the existing `fuxibbgpu=1` setup with **no level option**; first establish
   unchanged behavior and the default clock lines. On a separate baseline boot
   add `fuxigpuclock=1` to qualify the optional sample independently.
3. Test `fuxigpulevel=1` first: same 220 MHz, active DDR. Then 2 and 4; try 5/6
   only if earlier runs remain stable and temperature is acceptable. At fixed
   level 6, compare absent DDR, explicit 0, DDR 3, 6 and 7; DDR 8 must clamp to
   7 and match its packet policy. Keep the same ~100-draw 1080x2400 workload.
   Reboot between settings; there is no live retuning API. Repeat each from a
   comparable cool starting state with the same scene and display brightness.
4. Only after stable level-6 runs, compare 615/680 MHz (levels 7/8) using their
   default DT floors. To isolate core frequency, compare levels 6/7/8 with
   explicit DDR 7 across all three. Test level 8 plus DDR 8 separately; it must
   remain below 16500000 kB/s. Keep runs short and check thermal recovery.
5. Record requested GPU/ARC/BW, DDR requested/clamped/kBps, GPUCC and optional
   counter lines, first-frame latency, five-second frame-rate/late/failure/GPU-ms/busy-estimate windows,
   existing CPU clocks/thermal readings, and idle behavior after composition.
   Compare short identical runs and recovery to baseline. A higher frequency
   sample is evidence only for its sampling window; use actual rendering
   timestamp improvements to evaluate the workload.
6. Stop on any new HFI error/0x90a, timeout, fault, hang, visual corruption,
   missing fence, panic, temperature limit or unexpected continuing temperature
   rise. Remove both new arguments and reboot to restore the legacy policy.
   Confirm invalid `fuxigpulevel=9` logs rejection and selects legacy level 0;
   DDR 9 must be rejected and retain the selected level's bandwidth default;
   evaluate thermal and performance limits before considering any default change.

## Opt-in idle governor (`fuxigpulevel` bit 4)

2026-10-27. Offline only: host-tested by `tools/test-fuxi-gpu-dcvs.py`; **not yet
observed on the device** (frequency, heat, frame time).

Idea after UniXNU-sheng `package/GpuPowerProbe.cpp` (BSD-2-Clause-Patent; its
GMU-busy governor votes a level plus that level's bandwidth over HFI
GX_BW_PERF_VOTE, with an idle floor and a wake boost). Source revision read:
UniXNU-sheng working tree under the local research mirror, no commit recorded
there; UniXNU-sm8550-common `ShengGmuPerfVote` pairs each OPP with the
bandwidth row of its `opp-peak-kBps`, which our `g_gpu_bw_index` already does.
No code was copied; `xnu/fuxi-adreno/fuxi_gpu_dcvs.h` is our own policy.

What we were missing: the boot level was the only vote for the whole session,
so GX held the rail voltage and the DDR row of the chosen level even with the
screen static (`GPU_SET` is held; no idle collapse). The governor makes the
boot level a **cap**:

- `fuxigpulevel=L|0x10|(H<<8)`: L = 2..8 (the existing level and DDR rule),
  `0x10` enables it, H is the idle hold in 10 ms units (0 means 10, 100 ms).
  Example: `fuxigpulevel=0x16` = cap level 6, hold 100 ms. No new boot arg.
- Both ring doorbells (scanout copy submit, ABI client submit) call
  `fg_dcvs_kick()`. At the floor that queues one thread call, which votes the
  cap (`GX_BW_PERF_VOTE {0,1,L,bw row}`) and arms a timer for the hold.
- When the hold ends with no doorbell the worker votes level 1 (220 MHz, the
  2.1 GB/s row, lowest ARC level) and **stops**: no timer, no polling at the
  floor. The vote can never exceed the cap, so it cannot add heat over the
  fixed policy.
- Failed vote: the cap stays (down fails) or the floor stays until the next
  doorbell (up fails). Eight failures in a row disable the governor and try to
  put the cap back. Boot HFI record e[56..59] is preserved across votes.
- Cost of the idea, to measure: the first work after an idle gap can start at
  220 MHz for the HFI round trip before the cap lands (a doorbell is the only
  trigger; there is no input pre-wake yet).

Log lines (new): `FUXI GPU DCVS: cap level L floor 1 hold N ms (not yet
observed)`, then at power-of-two change counts `level X cap L ups U downs D
fails F floor_ms M`, `vote level X failed, HFI err`, and `disabled after N
failed votes`.

Device test: add `0x16` style value to the existing `fuxigpulevel` (keep
`fuxigpuddr` as is), compare `floor_ms` share against `FUXI PERF` temperatures
and `FUXI MTL: frame trace` gpu_exec/wall p50/p90 with the J163 baseline. Stop
and drop the bit if wall p90 worsens or any `vote ... failed` appears.

Not ported (no evidence they help at 60 fps, and they trade latency for heat
without a way to measure): the load-based 50 % busy step-up/step-down and the
GMU XO busy counter (`0x1f844`), the 2x-clock/578 MHz wake boost, and the
touch top-level hold. Our cap is already the level the user chose.
