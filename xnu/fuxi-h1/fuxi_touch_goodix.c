/* Goodix GT9916 (Berlin) touch over GENI I2C FIFO mode — Redmi K70 (vermeer).
 *
 * Wire protocol per the mainline Linux goodix_berlin driver (GPL-2.0, read as
 * the protocol specification): 32-bit big-endian register addresses, then
 * data; the event buffer lives at a dynamic touch_data_addr reported in the
 * IC info block (offset 95, le32); events are an 8-byte header (status,
 * request_type with the touch count in the low nibble, u16 byte-sum
 * checksum) followed by 8-byte contacts (status: low nibble point type /
 * high nibble id, le16 x, y, w) and a u16 checksum; status is cleared by a
 * zero-byte write at touch_data_addr.
 *
 * Transport: the same GENI serial engine the FTS driver uses (0xa90000,
 * QUP1-SE4), switched to the I2C firmware (vermeer's Goodix is an I2C
 * device; Android leaves the engine in I2C mode across a warm reboot).
 * FIFO-mode transfers like UniXNU's GeniI2c.c (BSD-2-Clause-Patent): no DMA,
 * no GPI, no SMMU — the touch thread drives the engine directly. Board
 * facts measured on the device (dump2 live DTB): irq GPIO 25, reset GPIO 24,
 * panel 1080x2400, address 0x5d. Clock votes and TLMM layout are shared with
 * fuxi_touch.c (the same engine and pins on both boards).
 *
 * Coordinates: the touch panel is 1080x2400 but the display stack presents
 * 1125x2436; scale both axes, clamp, then deliver through
 * sweet_hid_touch()/sweet_hid_touch_report() (patch 0058 SweetHID).
 *
 * Boot-arg fuxitouch_goodix=1 (fuxi_touch_init picks this driver over FTS).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <arm/caches_internal.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);
extern thread_t current_thread(void);
extern void fuxi_perf_input(void);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern int sweet_fb_display_off;       /* iokit/Kernel/arm/SweetFramebuffer.cpp */
extern void fuxi_touch_goodix_init(void);

#define GX_SE_PA     0x00a90000u
#define GX_GCC_PA    0x00100000u
#define GX_TLMM_PA   0x0f100000u
#define GX_GPIO(n)   ((uint32_t)(n) * 0x1000u)
#define GX_RESET     24u
#define GX_IRQ       25u
#define GX_PRIORITY  92            /* BASEPRI_PREEMPT, as the FTS driver */
#define GX_MAX       10u

/* GENI serial engine registers (UniXNU GeniI2c.h, BSD-2-Clause-Patent). */
#define GENI_FW_REVISION 0x068u
#define GENI_IF_DISABLE  0x064u
#define GENI_CLK_CFG     0x048u
#define GENI_CLK_SEL     0x040u
#define GENI_BYTE_GRAN   0x254u
#define GENI_DMA_MODE    0x258u
#define GENI_TX_PACK0    0x260u
#define GENI_TX_PACK1    0x264u
#define GENI_TX_LEN      0x26cu
#define GENI_RX_LEN      0x270u
#define GENI_SCL_COUNTERS 0x278u
#define GENI_RX_PACK0    0x284u
#define GENI_RX_PACK1    0x288u
#define GENI_M_CMD0      0x600u
#define GENI_M_CMD_CTRL  0x604u
#define GENI_M_IRQ_STATUS 0x610u
#define GENI_M_IRQ_EN    0x614u
#define GENI_M_IRQ_CLEAR 0x618u
#define GENI_TX_FIFO     0x700u
#define GENI_RX_FIFO     0x780u
#define GENI_RX_FIFO_STATUS 0x804u
#define GENI_TX_WATERMARK 0x80cu
#define GENI_SE_IRQ_EN   0xe1cu

#define GENI_M_CMD_DONE   (1u << 0)
#define GENI_M_CMD_OVERRUN (1u << 1)
#define GENI_M_ILLEGAL_CMD (1u << 2)
#define GENI_M_CMD_FAILURE (1u << 3)
#define GENI_M_CMD_CANCEL (1u << 4)
#define GENI_M_CMD_ABORT  (1u << 5)
#define GENI_M_GP_IRQ     (0x3fu << 9)   /* NACK, bus error, arbitration lost */
#define GENI_M_TX_WATERMARK (1u << 30)
#define GENI_M_ERRORS (GENI_M_CMD_OVERRUN | GENI_M_ILLEGAL_CMD | \
    GENI_M_CMD_FAILURE | GENI_M_GP_IRQ)
#define GENI_M_IRQ_WANTED (GENI_M_CMD_DONE | GENI_M_ERRORS | \
    GENI_M_CMD_CANCEL | GENI_M_CMD_ABORT | GENI_M_TX_WATERMARK)
#define GENI_SE_M_IRQ_EN  (1u << 2)
#define GENI_CMD_CTRL_ABORT (1u << 1)
#define GENI_CMD_CTRL_CANCEL (1u << 2)
#define GENI_PROTO_I2C    3u
#define GENI_OP_WRITE     1u
#define GENI_OP_READ      2u
#define GENI_STOP_STRETCH (1u << 2)

/* Goodix Berlin registers and structures. */
#define GX_BOOTOPTION_ADDR 0x10000u
#define GX_FW_VERSION_ADDR 0x10014u
#define GX_IC_INFO_ADDR    0x10070u
#define GX_IC_INFO_READ    128u          /* covers touch_data_addr @ offset 95 */
#define GX_TOUCH_DATA_OFF  95u
#define GX_DEV_CONFIRM     0xAAu
#define GX_TOUCH_EVENT     (1u << 7)
#define GX_REQUEST_EVENT   (1u << 6)
#define GX_REQUEST_RESET   3u
#define GX_COUNT_MASK      0xfu
#define GX_POINT_TYPE_MASK 0xfu
#define GX_TYPE_HOVER      1u
#define GX_TYPE_STYLUS     3u
#define GX_ID_SHIFT        4
#define GX_HDR_SIZE        8u
#define GX_TOUCH_SIZE      8u
#define GX_CHECKSUM_SIZE   2u
#define GX_FIRST_READ      (GX_HDR_SIZE + 2 * GX_TOUCH_SIZE + GX_CHECKSUM_SIZE)
#define GX_RESET_DELAY_MS  100u

#define GX_PANEL_X  1080u
#define GX_PANEL_Y  2400u
#define GX_LOGICAL_X 1125u
#define GX_LOGICAL_Y 2436u

static struct {
	vm_offset_t se, gcc, tlmm;
	uint32_t touch_data_addr;
	bool started;
	int x[GX_MAX], y[GX_MAX];
	uint32_t active;
	unsigned errors;
} gx;

static uint32_t
gx_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
gx_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
gx_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&gx, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
gx_deadline(unsigned us)
{
	uint64_t deadline;
	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	return mach_absolute_time() + deadline;
}

/* TLMM: pull [1:0], func [5:2], drive [8:6], OE [9] — as fuxi_touch.c. */
static void
gx_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		gx_wr(gx.tlmm, GX_GPIO(n) + 4, out ? 2u : 0u);
	}
	gx_wr(gx.tlmm, GX_GPIO(n), (gx_rd(gx.tlmm, GX_GPIO(n)) & ~0x3ffu) | pull |
	    (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u));
}

static bool
gx_on(uint32_t cbcr)
{
	return !(gx_rd(gx.gcc, cbcr) & 0x80000000u);
}

/* Clock votes: identical to fuxi_touch.c ft_clock (the same QUP1-SE4 engine). */
static int
gx_clock(void)
{
	gx_wr(gx.gcc, 0x52008, gx_rd(gx.gcc, 0x52008) | (0xfu << 18) | (1u << 26));
	for (unsigned i = 0; i < 10000 && !gx_on(0x2327c); i++) {
		delay(1);
	}
	if (!gx_on(0x2327c)) {
		printf("GX TOUCH: wrapper1 AHB off (vote %#x)\n", gx_rd(gx.gcc, 0x52008));
		return EIO;
	}
	unsigned level = 0;
	if (gx_rd(gx.gcc, 0x18504) & 1u) {
		while (level < 8 && (gx_rd(gx.gcc, 0x1850c + 4 * level) & 0x71fu) > 1u) {
			level++;
		}
	} else if ((gx_rd(gx.gcc, 0x184f4) & 0x71fu) > 1u) {
		level = 8;
	}
	if (level == 8) {
		printf("GX TOUCH: no XO clock level\n");
		return EIO;
	}
	gx_wr(gx.se, 0x07c, level);
	gx_wr(gx.se, 0x048, (2u << 4) | 1u);
	for (unsigned i = 0; i < 10000 && !gx_on(0x184e4); i++) {
		delay(1);
	}
	if (!gx_on(0x184e4)) {
		printf("GX TOUCH: SE4 clock off (cbcr %#x)\n", gx_rd(gx.gcc, 0x184e4));
		return EIO;
	}
	return 0;
}

/* GENI I2C FIFO transfers, blocking in the touch thread. The engine runs the
 * I2C firmware (vermeer's Goodix is I2C; Android leaves it selected). */

#define GX_I2C_TIMEOUT 50000u

static int
gx_i2c_wait_done(void)
{
	for (unsigned i = 0; i < GX_I2C_TIMEOUT; i++) {
		uint32_t st = gx_rd(gx.se, GENI_M_IRQ_STATUS);
		if (st) {
			gx_wr(gx.se, GENI_M_IRQ_CLEAR, st);
			if (st & GENI_M_ERRORS) {
				return EIO;
			}
			if (st & GENI_M_CMD_DONE) {
				return 0;
			}
		}
		delay(1);
	}
	/* deadline: cancel then abort, per the engine docs */
	gx_wr(gx.se, GENI_M_CMD_CTRL, GENI_CMD_CTRL_CANCEL);
	for (unsigned i = 0; i < GX_I2C_TIMEOUT; i++) {
		uint32_t st = gx_rd(gx.se, GENI_M_IRQ_STATUS);
		if (st & (GENI_M_CMD_CANCEL | GENI_M_CMD_DONE)) {
			gx_wr(gx.se, GENI_M_IRQ_CLEAR, st);
			break;
		}
		delay(1);
	}
	gx_wr(gx.se, GENI_M_CMD_CTRL, GENI_CMD_CTRL_ABORT);
	return ETIMEDOUT;
}

/* Write N bytes starting at the 32-bit register address (big-endian prefix). */
static int
gx_i2c_write_reg(uint32_t reg, const uint8_t *data, uint32_t len)
{
	uint32_t total = 4 + len;
	uint8_t buf[4 + 64];
	buf[0] = (uint8_t)(reg >> 24);
	buf[1] = (uint8_t)(reg >> 16);
	buf[2] = (uint8_t)(reg >> 8);
	buf[3] = (uint8_t)reg;
	if (len > sizeof(buf) - 4) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < len; i++) {
		buf[4 + i] = data[i];
	}
	uint32_t words = (total + 3) / 4;
	gx_wr(gx.se, GENI_TX_LEN, total);
	gx_wr(gx.se, GENI_M_CMD0, (GENI_OP_WRITE << 27) | (0x5du << 9));
	gx_wr(gx.se, GENI_TX_WATERMARK, 1);
	for (uint32_t w = 0; w < words; w++) {
		uint32_t word = 0;
		for (uint32_t i = 0; i < 4; i++) {
			uint32_t idx = w * 4 + i;
			if (idx < total) {
				word |= (uint32_t)buf[idx] << (8 * i);
			}
		}
		/* wait for the watermark asking for this word */
		for (unsigned t = 0; t < GX_I2C_TIMEOUT; t++) {
			uint32_t st = gx_rd(gx.se, GENI_M_IRQ_STATUS);
			if (st & GENI_M_ERRORS) {
				gx_wr(gx.se, GENI_M_IRQ_CLEAR, st);
				return EIO;
			}
			if (st & GENI_M_TX_WATERMARK) {
				gx_wr(gx.se, GENI_M_IRQ_CLEAR, GENI_M_TX_WATERMARK);
				break;
			}
			if (st & GENI_M_CMD_DONE) {
				break;
			}
			delay(1);
		}
		gx_wr(gx.se, GENI_TX_FIFO, word);
	}
	int err = gx_i2c_wait_done();
	gx_wr(gx.se, GENI_TX_WATERMARK, 0);
	return err;
}

/* Read N bytes from the 32-bit register address. */
static int
gx_i2c_read_reg(uint32_t reg, uint8_t *out, uint32_t len)
{
	uint8_t addr[4] = {
		(uint8_t)(reg >> 24), (uint8_t)(reg >> 16),
		(uint8_t)(reg >> 8), (uint8_t)reg,
	};
	uint32_t awords = 1;   /* 4 addr bytes = 1 FIFO word */
	gx_wr(gx.se, GENI_TX_LEN, 4);
	gx_wr(gx.se, GENI_M_CMD0, (GENI_OP_WRITE << 27) | (0x5du << 9) | GENI_STOP_STRETCH);
	gx_wr(gx.se, GENI_TX_WATERMARK, 1);
	for (uint32_t w = 0; w < awords; w++) {
		for (unsigned t = 0; t < GX_I2C_TIMEOUT; t++) {
			uint32_t st = gx_rd(gx.se, GENI_M_IRQ_STATUS);
			if (st & GENI_M_ERRORS) {
				gx_wr(gx.se, GENI_M_IRQ_CLEAR, st);
				return EIO;
			}
			if (st & GENI_M_TX_WATERMARK) {
				gx_wr(gx.se, GENI_M_IRQ_CLEAR, GENI_M_TX_WATERMARK);
				break;
			}
			delay(1);
		}
		uint32_t word = 0;
		for (uint32_t i = 0; i < 4; i++) {
			word |= (uint32_t)addr[w * 4 + i] << (8 * i);
		}
		gx_wr(gx.se, GENI_TX_FIFO, word);
	}
	int err = gx_i2c_wait_done();
	if (err) {
		gx_wr(gx.se, GENI_TX_WATERMARK, 0);
		return err;
	}
	/* the held write completes without a STOP; now clock N bytes in */
	gx_wr(gx.se, GENI_RX_LEN, len);
	gx_wr(gx.se, GENI_M_CMD0, (GENI_OP_READ << 27) | (0x5du << 9));
	err = gx_i2c_wait_done();
	gx_wr(gx.se, GENI_TX_WATERMARK, 0);
	if (err) {
		return err;
	}
	if (!(gx_rd(gx.se, GENI_RX_FIFO_STATUS) & 0x1ffffffu)) {
		return EIO;
	}
	uint32_t words = (len + 3) / 4;
	for (uint32_t w = 0; w < words; w++) {
		uint32_t word = gx_rd(gx.se, GENI_RX_FIFO);
		for (uint32_t i = 0; i < 4; i++) {
			uint32_t idx = w * 4 + i;
			if (idx < len) {
				out[idx] = (uint8_t)(word >> (8 * i));
			}
		}
	}
	return 0;
}

static uint16_t
gx_checksum(const uint8_t *p, uint32_t len)
{
	uint16_t sum = 0;
	for (uint32_t i = 0; i < len; i++) {
		sum += p[i];
	}
	return sum;
}

/* The RX FIFO holds 16 words (64 B): chunk reads beyond that. The Berlin
 * protocol allows a read at any 32-bit address, so chunk boundaries are free. */
static int
gx_read32(uint32_t reg, uint8_t *out, uint32_t len)
{
	while (len) {
		uint32_t n = len > 60u ? 60u : len;
		int err = gx_i2c_read_reg(reg, out, n);
		if (err) {
			return err;
		}
		reg += n;
		out += n;
		len -= n;
	}
	return 0;
}

static int
gx_dev_confirm(void)
{
	uint8_t data[4] = {0};
	int err = gx_read32(GX_BOOTOPTION_ADDR, data, 4);
	if (err) {
		return err;
	}
	if (data[0] != GX_DEV_CONFIRM || data[1] != GX_DEV_CONFIRM ||
	    data[2] != GX_DEV_CONFIRM || data[3] != GX_DEV_CONFIRM) {
		printf("GX TOUCH: dev confirm mismatch %02x %02x %02x %02x\n",
		    data[0], data[1], data[2], data[3]);
		return EIO;
	}
	uint8_t zero = 0;
	return gx_i2c_write_reg(GX_BOOTOPTION_ADDR, &zero, 1);
}

static int
gx_read_version(void)
{
	uint8_t v[19] = {0};
	int err = gx_read32(GX_FW_VERSION_ADDR, v, sizeof(v));
	if (err) {
		return err;
	}
	printf("GX TOUCH: patch pid %.8s vid %.4s sensor %u\n",
	    (const char *)&v[9], (const char *)&v[17], v[21]);
	return 0;
}

static int
gx_read_ic_info(void)
{
	uint8_t info[GX_IC_INFO_READ] = {0};
	int err = gx_read32(GX_IC_INFO_ADDR, info, sizeof(info));
	if (err) {
		return err;
	}
	uint32_t addr = info[GX_TOUCH_DATA_OFF] |
	    (uint32_t)info[GX_TOUCH_DATA_OFF + 1] << 8 |
	    (uint32_t)info[GX_TOUCH_DATA_OFF + 2] << 16 |
	    (uint32_t)info[GX_TOUCH_DATA_OFF + 3] << 24;
	if (!addr || addr > 0x1ffffu) {
		printf("GX TOUCH: bad touch_data_addr %#x\n", addr);
		return EIO;
	}
	gx.touch_data_addr = addr;
	printf("GX TOUCH: touch_data_addr %#x\n", addr);
	return 0;
}

/* One poll: read the event header + 2 contacts, extend for more, checksum,
 * parse, deliver through the same path the FTS driver uses. */
static void
gx_poll(bool deliver)
{
	uint8_t e[GX_FIRST_READ + 8 * (GX_MAX - 2)] = {0};
	uint32_t mask = gx.active;
	uint8_t hdr[GX_HDR_SIZE];
	int err = gx_read32(gx.touch_data_addr, hdr, GX_HDR_SIZE);
	if (err) {
		return;
	}
	if (hdr[0] == 0) {
		return;
	}
	if (gx_checksum(hdr, 6) != (uint16_t)(hdr[6] | hdr[7] << 8)) {
		printf("GX TOUCH: header checksum error\n");
		goto out_clear;
	}
	uint8_t req = hdr[2];
	if (req & GX_TOUCH_EVENT) {
		uint8_t count = req & GX_COUNT_MASK;
		if (count > GX_MAX) {
			goto out_clear;
		}
		uint32_t len = count * GX_TOUCH_SIZE + GX_CHECKSUM_SIZE;
		if ((err = gx_read32(gx.touch_data_addr + GX_HDR_SIZE, e,
		    len)) != 0) {
			goto out_clear;
		}
		if (count && gx_checksum(e, len - GX_CHECKSUM_SIZE) !=
		    (uint16_t)(e[len - 2] | e[len - 1] << 8)) {
			printf("GX TOUCH: touch checksum error\n");
			goto out_clear;
		}
		bool changed = false;
		for (uint8_t k = 0; k < count; k++) {
			const uint8_t *t = e + k * GX_TOUCH_SIZE;
			uint8_t st = t[0];
			unsigned id = st >> GX_ID_SHIFT;
			unsigned type = st & GX_POINT_TYPE_MASK;
			if (id >= GX_MAX) {
				continue;
			}
			int x = t[2] | t[3] << 8;
			int y = t[4] | t[5] << 8;
			/* panel 1080x2400 -> logical 1125x2436 */
			x = x * (int)GX_LOGICAL_X / (int)GX_PANEL_X;
			y = y * (int)GX_LOGICAL_Y / (int)GX_PANEL_Y;
			if (x >= (int)GX_LOGICAL_X) {
				x = GX_LOGICAL_X - 1;
			}
			if (y >= (int)GX_LOGICAL_Y) {
				y = GX_LOGICAL_Y - 1;
			}
			bool touching = type != 0 && type != GX_TYPE_HOVER;
			if (deliver) {
				gx.x[id] = x;
				gx.y[id] = y;
				if (touching) {
					mask |= 1u << id;
				} else {
					mask &= ~(1u << id);
				}
			}
			changed = true;
		}
		if (deliver && changed) {
			fuxi_perf_input();
			for (unsigned id = 0; id < GX_MAX; id++) {
				uint32_t bit = 1u << id;
				if (mask & bit && !(gx.active & bit)) {
					sweet_hid_touch((int)id, 1, gx.x[id], gx.y[id]);
				} else if (!(mask & bit) && (gx.active & bit)) {
					sweet_hid_touch((int)id, 0, gx.x[id], gx.y[id]);
				}
			}
			gx.active = mask;
		}
	}
out_clear:
{
	uint8_t zero = 0;
	(void)gx_i2c_write_reg(gx.touch_data_addr, &zero, 1);
}
}

static void
gx_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 30, poll_us = 4000;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("fuxitouchgoodixdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("fuxitouch_poll_us", &poll_us, sizeof(poll_us));
	poll_us = poll_us < 1000 ? 1000 : poll_us;
	printf("GX TOUCH: starting in %u s (poll %u us)\n", delay_s, poll_us);
	gx_sleep(delay_s * 1000u);
	gx.gcc = ml_io_map(GX_GCC_PA, 0x1f0000);
	gx.tlmm = ml_io_map(GX_TLMM_PA, 0x30000);
	gx.se = ml_io_map(GX_SE_PA, 0x4000);
	if (!gx.gcc || !gx.tlmm || !gx.se) {
		goto out;
	}
	int ierr;
	if ((ierr = gx_clock())) {
		printf("GX TOUCH: clock failed %d\n", ierr);
		goto out;
	}
	/* select the I2C firmware and confirm the engine runs it */
	gx_wr(gx.se, GENI_CLK_SEL, GENI_PROTO_I2C);
	uint32_t fwrev = gx_rd(gx.se, GENI_FW_REVISION);
	if (((fwrev >> 8) & 0xffu) != GENI_PROTO_I2C || (gx_rd(gx.se, GENI_IF_DISABLE) & 1u)) {
		printf("GX TOUCH: engine not I2C (fw rev %#x, disable %#x)\n",
		    fwrev, gx_rd(gx.se, GENI_IF_DISABLE));
		goto out;
	}
	if (!(gx_rd(gx.se, GENI_CLK_CFG) & 1u) || !gx_rd(gx.se, GENI_SCL_COUNTERS)) {
		printf("GX TOUCH: I2C clocking missing\n");
		goto out;
	}
	gx_wr(gx.se, GENI_DMA_MODE, 0);   /* FIFO mode */
	gx_wr(gx.se, GENI_TX_PACK0, 0x7f8fe);
	gx_wr(gx.se, GENI_TX_PACK1, 0xffefe);
	gx_wr(gx.se, GENI_RX_PACK0, 0x7f8fe);
	gx_wr(gx.se, GENI_RX_PACK1, 0xffefe);
	gx_wr(gx.se, GENI_BYTE_GRAN, 0);
	gx_wr(gx.se, GENI_M_IRQ_CLEAR, 0xffffffffu);
	gx_wr(gx.se, GENI_M_IRQ_EN, GENI_M_IRQ_WANTED);
	gx_wr(gx.se, GENI_SE_IRQ_EN, gx_rd(gx.se, GENI_SE_IRQ_EN) | GENI_SE_M_IRQ_EN);
	printf("GX TOUCH: engine up (fw rev %#x)\n", fwrev);

	/* reset: GPIO 24 high 2 ms (active high per the DT flags 0), then 100 ms */
	gx_gpio(GX_RESET, 0, 3, 8, 1);
	gx_sleep(2);
	gx_gpio(GX_RESET, 0, 3, 8, 0);
	gx_sleep(GX_RESET_DELAY_MS);
	gx_gpio(GX_IRQ, 0, 3, 8, -1);

	int err;
	if ((err = gx_dev_confirm()) || (err = gx_read_version()) ||
	    (err = gx_read_ic_info())) {
		printf("GX TOUCH: init failed %d\n", err);
		goto out;
	}
	printf("GX TOUCH: sensing, polling GPIO%u\n", GX_IRQ);

	uint64_t poll_abs, idle_abs;
	nanoseconds_to_absolutetime(poll_us * 1000ULL, &poll_abs);
	nanoseconds_to_absolutetime(32 * 1000ULL * 1000ULL, &idle_abs);
	uint64_t next = mach_absolute_time();
	bool was_off = false;
	for (;;) {
		bool off = sweet_fb_display_off;
		if (off != was_off) {
			was_off = off;
			if (off && gx.active) {
				gx_poll(false);   /* consume and drop */
			}
		}
		bool irq = !(gx_rd(gx.tlmm, GX_GPIO(GX_IRQ) + 4) & 1u);
		if (!off && (irq || gx.active)) {
			gx_poll(true);
		} else if (off || !irq) {
			/* still sweep at 32 ms in case the line does not assert */
			if (!off) {
				gx_poll(false);
			}
		}
		uint64_t period = off ? idle_abs :
		    (gx.active || irq) ? poll_abs : idle_abs;
		next += period;
		uint64_t now = mach_absolute_time();
		if (next <= now) {
			next = now + period;
		}
		/* sleep like fuxi_touch.c: assert on the thread chan address */
		{
			static int chan;
			uint64_t dl;
			clock_interval_to_deadline(poll_us / 1000 + 1, NSEC_PER_MSEC, &dl);
			(void)assert_wait_deadline(&chan, THREAD_UNINT, dl);
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
out:
	printf("GX TOUCH: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
fuxi_touch_goodix_init(void)
{
	thread_t thread;
	if (gx.started) {
		return;
	}
	gx.started = true;
	if (kernel_thread_start_priority(gx_thread, NULL, GX_PRIORITY,
	    &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
