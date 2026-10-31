# Vermeer touch lane: Goodix GT9916 (Berlin) over GENI I2C

Status: plan + extracted facts. Implementation not started. Everything below is
measured from the device (dump2 live DTB, Android dmesg) or quoted from the
mainline Linux driver (drivers/input/touchscreen/goodix_berlin_*.c, GPL-2.0 —
reference only, the XNU driver is a clean-room reimplementation of the wire
protocol).

## Hardware facts (vermeer, measured)

| Item | Value | Source |
| --- | --- | --- |
| Controller | Goodix **GT9916** (Berlin family) | DTB compatible `goodix,gt9916` |
| Transport | GENI **I2C**, engine `0xa90000` (QUP1, wrapper `0xac0000`) | DTB `i2c@a90000/goodix-berlin@5d` |
| I2C address | `0x5d` (7-bit) | DTB `reg` |
| IRQ GPIO | **25**, level-low (flags 0x2008) | DTB `goodix,irq-gpio` |
| Reset GPIO | **24** | DTB `goodix,reset-gpio` |
| Touch native resolution | **1080 × 2400** (`panel-max-x/y`) | DTB |
| Supplies | avdd (phandle 0x2f3), iovdd (0x2a) | DTB — likely already on from ABL |
| Firmware/config | `goodix_firmware_i2c.bin`, `goodix_cfg_group_i2c.bin` (in /vendor) | DTB |
| Android driver state | probe OK, fw version equal, input `goodix_ts` on event7 | dmesg after our XNU boots |

The engine is the SAME physical SE as sheng's SPI touch (UniXNU TouchBoard.h:
`TB_SE_BASE 0xa90000`), so the GCC clock votes (wrap1 core/2x/m_ahb/s_ahb/s4,
halts 0x23294/0x23284/0x2327c/0x23280/0x184e4, RCG 0x184f0) and TLMM code carry
over; only the pin mux differs (I2C SDA/SCL instead of SPI 4-wire) and the
protocol.

## Berlin wire protocol (from the mainline driver)

- Register addresses are **32-bit big-endian**, sent as the first 4 I2C bytes
  (regmap `reg_bits=32`); data follows (or a repeated-start read).
- GT9916 (i2c match `gt9916`) uses `_D` addresses:
  `FW_VERSION_INFO_ADDR_D = 0x10014`, `IC_INFO_ADDR_D = 0x10070`.
- `GOODIX_BERLIN_BOOTOPTION_ADDR = 0x10000`, dev-confirm value `0xAA`.
- Boot flow: power on → reset pulse (2 ms high) → 100 ms → read fw version
  (0x10014) → read IC info (0x10070, ≤1 KB, **`touch_data_addr` comes from the
  ic_info struct**, field `touch_data_addr` @ the struct's le32 member) →
  dev-confirm (write/read 0x10000 expecting 0xAA) → irq-driven reads.
- Per-event read at `touch_data_addr`:
  - header 8 B: `status, rsv, request_type, rsv[3], checksum(le16)`; checksum =
    byte-sum of the first 6 bytes.
  - `status & BIT(7)` = touch event; `BIT(6)` = request event (request_type 3 =
    ic reset wanted); low nibble of `request_type` = touch count.
  - touches follow: `GOODIX_BERLIN_TOUCH_SIZE = 8` each:
    `status` (low nibble = point type: 0 press... types 1 hover / 3 stylus),
    rsv, `x(le16)`, `y(le16)`, `w(le16)`; touch id = high nibble of `status`.
  - u16 checksum over the touch bytes; read 8+2*8+2 = 26 B first, extend for
    >2 touches (`(n-2)*8` more), then clear status: `regmap_write(touch_data_addr, 0)`.
- Max 10 touches.

## ⚠️ CRITICAL: FIFO mode is hardware-disabled on this SE (measured)

The on-device capture (goodix=1 boot, monitor-root26) shows the engine at
`fw rev 0x102` = **SPI proto selected** + `FIFO_IF_DISABLE=1`. UniXNU's
TouchProbe.cpp documents this exact hardware (v53-v55 SE revision):

> "FIFO_IF_DISABLE=1 and the register that controls it (0x2008) aborts on
> access, so FIFO transfers cannot be used; the SE DMA engine is the
> host-visible path."

The first FIFO-mode driver (fuxi_touch_goodix.c v1, 0122) is therefore
dead on this SE — kept in git history as the protocol reference. The working
path is **GPI DMA mode with I2C TREs**, reusing the FTS driver's proven
machinery (the same engine, the same GPII0 channels, the same SMMU SMRs —
fuxi_touch.c's ft_gpi_init/ft_tx/ft_read, ~80% reusable):

1. SE proto: the engine must be switched to I2C (GENI_CLK_SEL = 3) — the FTS
   driver left it in SPI; a warm reset may or may not restore I2C (measured:
   fw rev 0x102 = SPI after a warm XNU→Android→XNU cycle — the SE proto
   survives warm resets; the driver must switch it explicitly).
2. The GPI DMA TREs change from SPI to I2C format (Linux gpi.c is the
   reference: drivers/dma/qcom/gpi.c, the I2C TRE structures — fetch and
   port; the FTS ft_tre() builder is the pattern).
3. The Goodix Berlin protocol layer stays (32-bit BE reg addresses as I2C
   payload bytes; the event/checksum structures are unchanged).

## Transport work (superseded: UniXNU GeniI2c.c FIFO extension — unusable)

UniXNU `package/GeniI2c.c` (BSD-2-Clause-Patent) is a polled state machine for
**1-byte register read/write** on an SE already in GENI-I2C mode. Needed
extensions for Berlin:

1. **Multi-byte TX**: address phase = 4 big-endian reg bytes + N data bytes,
   fed 4 bytes per FIFO word via the TX watermark (loop instead of the single
   `g->word`).
2. **Multi-byte RX**: 4-byte reg address write with STOP_STRETCH, then
   `RX_LEN = N` and N-byte read; N ≤ 26 fits the 16-word FIFO in one go; the
   1 KB IC-info read chunks at 60+ bytes or raises the RX watermark threshold.
3. Keep the poll model (the touch thread polls; no irq needed for bring-up —
   fuxi's touch thread polls at 4 ms anyway).

Board facts for vermeer (new `TouchBoard` variant): same QUP votes/halts, GPIO
25 IRQ (unused initially), GPIO 24 reset via TLMM (ctl 0x1000*24, out), pin mux
for I2C: `qup1_se4` function on the two I2C GPIOs (numbers from the vermeer DT
pinctrl node — extract from the dump tgz `live-dt` when needed; sheng used
44-47 for SPI4 on the same engine).

## XNU integration (fuxi kernel)

- New file `xnu/fuxi-h1/fuxi_touch_goodix.c` alongside `fuxi_touch.c` (FTS);
  selected by boot-arg (`fuxitouch_goodix=1`) or #ifdef for vermeer builds.
- Deliver events through the existing interface: `sweet_hid_touch(finger, down,
  x, y)` / `sweet_hid_touch_report(mask, x[], y[])` (patch 0058 SweetHID).
- Coordinate mapping: touch native 1080×2400 → logical 1125×2436
  (multiply x by 1125/1080, y by 2436/2400), matching the AppleCLCD logical
  size the display stack reports; verify against the lock screen.
- Poll loop: reuse `fuxi_touch.c`'s thread/timing pattern (4 ms active), state
  machine: power-on → reset → read fw version → ic info (get touch_data_addr)
  → dev-confirm → poll loop (read 26 B, handle, clear status).
- Power: avdd/iovdd are probably enabled by ABL (Android leaves them on); if
  the read at 0x10014 fails, add PM8550 GPIO/regulator enable via the SPMI
  code already in the tree (sweet_spmi.c) before assuming hardware trouble.

## Implementation order

1. GeniI2c multi-byte extension + host-test with the DSC-free model (UniXNU
   has `package/tests/geni_i2c_host.c` as the pattern).
2. Goodix protocol layer (version read → ic info → touch poll) against a host
   I2C mock.
3. `fuxi_touch_goodix.c` integration: thread, boot-arg, coordinate mapping,
   `sweet_hid_touch` delivery.
4. Kernel patch 0122 (`fuxitouch_goodix` boot-arg gates it), CI kernel build,
   deploy, watch `|touch|` lines on the lock screen: raw x/y in the log first,
   then real cursor/press.
5. Calibration: map to the lock screen, then unlock by swipe.

## Risks

- SE mode after ABL: `geni_i2c_prepare` requires FW_REVISION proto = I2C and
  the SE clock on. Android used this SE last, but ABL may have reconfigured it
  for its own splash path. If proto reads wrong, drive the SE init from XNU
  (GENI FW is in the shared region — set proto via GENI_FW_REVISION? sheng's
  loader-side TouchProbe handles this; port that too if needed).
- Supplies off in XNU: then reads at 0x10014 fail with NACK — handle by
  enabling the rails via SPMI (see sweet_spmi.c) with the DTB phandle voltages
  decoded from the dump.
- Coordinate orientation: `invert_xy` exists in the DT — check both.
