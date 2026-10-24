# Vermeer (Redmi K70 / POCO F6 Pro) port lane

Status: scaffolding. The XNU kernel builds unchanged (same SM8550 generation as
Fuxi); the port work is loader/ADT geometry, panel, touch and buttons, all of
which need values measured on a live device.

## What is already parameterized (this branch)

Fail-closed, in the repository's style: Fuxi keeps its measured defaults; a
`vermeer` build refuses to start until every measured value is supplied.

| File | Change |
| --- | --- |
| `tools/mkadt-fuxi.py` | `--board {fuxi,vermeer}` + `--phys-base/--bank-size/--fb-base/--fb-size/--model-key`; non-default boards require all four geometry values; `/compatible` gains `xiaomi,vermeer`; the live-FDT model check uses the board keyword |
| `shim/console.h` | `FB_BASE/FB_W/FB_H/FB_STRIDE` overridable with `-D` |
| `shim/fuxi-probe/gpu-handoff.h` | `FG_RAM_BASE/FG_BANK_SIZE/FG_SCRATCH_SIZE` overridable; `FG_RAM_SIZE/FG_SCRATCH` derive from the bank |
| `shim/fuxi-loader/loader.c` | payload RAM-window check uses `XNU_PHYS_BASE` (default 0x8c0000000) |
| `shim/fuxi-loader/build.sh` | `BOARD=fuxi\|vermeer`; vermeer requires `VERMEER_PHYS_BASE/BANK_SIZE/FB_BASE/FB_SIZE/FB_W/FB_H` from the measured live FDT and passes them as `-D`; `ZAP_SHA256` overrides the Fuxi zap pin for a measured device zap (GMU/SQE stay pinned) |
| `tools/vermeer-collect.sh` | read-only on-device collector: live DT, iomem, fb geometry, zap firmware + SHA256, touch/button nodes |

## What is NOT ported yet (needs the device)

- **RAM window**: vermeer has 12/16 GB LPDDR5X; the contiguous bank above
  `0x800000000` and every reserved hole must come from the live FDT
  (`tools/vermeer-collect.sh` dumps it). The ADT validation then checks the
  chosen window against `/memory` and `/reserved-memory` exactly as for Fuxi.
- **Display**: 1440×3200 panel. The loader inherits ABL's splash scanout; the
  SSPP0/CTL0 check in `loader.c` (`0xb8000000`, stride 4320, 2400×1080) is
  Fuxi-measured and intentionally fails closed on vermeer → USB-only console
  for first boots. Re-enable drawing only with measured vermeer values.
- **Touch**: patch 0091 is FTS-over-GENI-SPI with Fuxi's SE instance, GPIOs
  and power sequence. Identify vermeer's touch IC from the dumped DT and port
  the driver; do not reuse Fuxi GPIOs.
- **Buttons**: power (PMK8550 PON) is SoC-common; volume GPIOs must be
  verified against the dump.
- **UFS/root**: SoC controller (0x1d84000) reused; the pinned-file root and
  its extent table are per-installation (`mkadt-fuxi.py --root-extents`).
- **GPU**: same A740. GMU/SQE firmware pins already match
  (sheng-firmware set); `ZAP_SHA256` must be set to vermeer's own zap from
  the dump.

## Bring-up order

1. Kernel: GitHub Actions `fuxi-build.yml` (macos-14, `kernel=metal4`) —
   unchanged, the artifact is board-independent at this stage.
2. Dump the device with `tools/vermeer-collect.sh` (rooted Android or a
   custom recovery; bootloader unlock wipes data — back up first).
3. First boot target: RAM disk root, 1 CPU, no `--smp`, `con_fb=0`
   (automatic), USB console only:
   `mkadt-fuxi.py --board vermeer --phys-base … --bank-size … --fb-base … --fb-size … --ram-layout 1g`
   then `mkpayload.py … --ramdisk init.img` and
   `BOARD=vermeer VERMEER_… bash shim/fuxi-loader/build.sh payload.bin`.
4. `fastboot boot` and read the loader's USB monitor (`tools/fuxi-xnu-monitor.py`).
5. Then: display (measured SSPP/splash), touch, buttons, UFS root, SMP.

## Non-goals / cautions

Same as the main README: temporary-boot only, no daily-driver use. The vermeer
path must never guess geometry: an unmeasured `VERMEER_*` value can corrupt
live Android memory. That is why every one of them is a required environment
variable, not a default.
