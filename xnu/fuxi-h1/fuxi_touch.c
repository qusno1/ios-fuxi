/*
 * FUXI touch: ST FTS (FTM5) on QUP1 SE4, SPI through QUP1 GPI DMA (polled).
 * Hardware facts and protocol: research/touch/hw-report.md; first proven in
 * the loader (shim/fuxi-probe/probe.c request 0x55). The loader votes the
 * RPMh rails (ldob12 vdd 1.8 V, ldom6 avdd 3.2 V) before the XNU handoff;
 * this driver does not talk to RPMh. SE4 is GSI-only (GENI_IF_DISABLE_RO),
 * so transfers use GPII 0 (TX channel 0, RX channel 1, one event ring), with
 * apps-smmu SMRs for streams 0xb6 (GPI) and 0xa3 (QUP1) routed to context
 * bank 4 (stage 1 off), as the loader does (research/touch/gpi-recipe.md,
 * ts-12). It takes the IC through its reset,
 * waits for the controller-ready event, enables multi-touch scanning and
 * polls the event FIFO while GPIO25 (IRQ, active low) is asserted.
 * No firmware upload: the FTS keeps its firmware in its own flash.
 *
 * Polling (0093): every 4 ms while a finger is down or the IRQ line was
 * seen recently, every 8 ms when idle and every 32 ms while the display is
 * off (AppleCLCD RequestPowerChange(0)); deadlines are absolute and use the
 * critical timer urgency, so no coalescing leeway is added. Touches are not
 * reported while the display is off (boot-arg fuxitouch_offsuppress=0 keeps
 * reporting). Boot-arg fuxitouch_poll_us sets the active period (default
 * 4000), fuxitouch_idle_us the idle one (default 8000).
 *
 * Tap-to-wake (H1, boot-arg fuxitaptowake=1, default off): a new contact
 * while the display is off asks bsd/dev/fuxi_keys.c for a power press,
 * which it sends only while backboardd's TouchDetectionMode is 0xfe. After
 * such a press the contacts of that off period stay hidden from iOS until
 * they lift. Without a wake press nothing changes.
 * Ported from HackiOS hackios-xnu-userland userland/src/inject.c (MIT):
 * the waking touch is kept from iOS. MIT notice in fuxi_key_state.h.
 *
 * Report batching/counters: adapted from UniXNU/NativeHidInput,
 * 75efe1c, e43c254, 22f2dd4 and ba03e5d (BSD-2-Clause-Patent).
 * Notice: iokit/Kernel/arm/hid/LICENSE.NativeHidInput. fuxitbatch=1
 * sends the complete ID-indexed contact set in one digitizer collection.
 */
#if defined(FUXI)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include "fuxi_key_state.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern int sweet_hid_touch_report(uint32_t mask, const int *x, const int *y);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);
extern int sweet_fb_display_off;       /* iokit/Kernel/arm/SweetFramebuffer.cpp */
extern bool fuxi_keys_touch_wake(void); /* bsd/dev/fuxi_keys.c */
void fuxi_touch_init(void);

#define FT_SE_PA     0x00a90000u
#define FT_GPI_PA    0x00a10000u   /* GPII 0: ee base 0x9f0000 + 0x20000 */
#define FT_SMMU_PA   0x15000000u
#define FT_GCC_PA    0x00100000u
#define FT_TLMM_PA   0x0f100000u   /* upstream tlmm@f100000; 0x0f0xxxxx aborts */
#define FT_GPIO(n)   ((uint32_t)(n) * 0x1000u)
#define FT_RESET     24u
#define FT_IRQ       25u
#define FT_MAX       10u
#define FT_EV_READY  0x03u
#define FT_EV_ENTER  0x13u
#define FT_EV_MOTION 0x23u
#define FT_EV_LEAVE  0x33u
#define FT_TYPE_HOVER 0x05u        /* ftsSoftware.h TOUCH_TYPE_HOVER: not touching */
#define FT_PRIORITY  92            /* BASEPRI_PREEMPT */

static struct {
	vm_offset_t se, gcc, tlmm, gpi, smmu;
	vm_offset_t dma;
	uint64_t dma_pa;
	unsigned depth, level, wp[2], evrp;
	bool started;
	unsigned batch;              /* opt-in report-wide HID collection */
	bool retry;                  /* latest set not yet constructed by HID */
	uint32_t active;
	int x[FT_MAX], y[FT_MAX];
	bool ic_reset;                 /* controller-ready event while sensing */
	/* Per-contact statistics (first finger down -> last finger up). */
	uint64_t c_start, c_last, c_poll_sum, c_poll_max;
	unsigned c_reports, c_polls, contacts;
	unsigned c_multi, c_max, c_lifts;
	struct fuxi_wake_touch wake;   /* tap-to-wake contact filter */
} ft;
static int ft_chan;

static uint32_t
ft_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
ft_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
ft_sleep_until(uint64_t deadline)
{
	(void)assert_wait_deadline_with_leeway(&ft_chan, THREAD_UNINT,
	    TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
ft_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ft_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* TLMM CTL: pull [1:0] (1 down, 3 up), func [5:2], drive [8:6] = mA/2-1, OE [9]. */
static void
ft_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		ft_wr(ft.tlmm, FT_GPIO(n) + 4, out ? 2u : 0u);
	}
	ft_wr(ft.tlmm, FT_GPIO(n), (ft_rd(ft.tlmm, FT_GPIO(n)) & ~0x3ffu) | pull |
	    (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u));
}

static bool
ft_on(uint32_t cbcr)
{
	return !(ft_rd(ft.gcc, cbcr) & 0x80000000u);
}

static int
ft_clock(void)
{
	/* Vote wrapper1 core/core2x/m_ahb/s_ahb and SE4 only. */
	ft_wr(ft.gcc, 0x52008, ft_rd(ft.gcc, 0x52008) | (0xfu << 18) | (1u << 26));
	for (unsigned i = 0; i < 10000 && !ft_on(0x2327c); i++) {
		delay(1);
	}
	if (!ft_on(0x2327c)) {
		/* SE registers hang the bus without the wrapper AHB clock. */
		printf("FUXI TOUCH: wrapper1 AHB off (vote %#x)\n", ft_rd(ft.gcc, 0x52008));
		return EIO;
	}
	/* S4 is a DFS RCG under hardware control: the SE's level request brings
	 * it up. Level from XO, undivided (19.2 MHz); SE divider 2: 9.6 MHz. */
	unsigned level = 0;
	if (ft_rd(ft.gcc, 0x18504) & 1u) {
		while (level < 8 && (ft_rd(ft.gcc, 0x1850c + 4 * level) & 0x71fu) > 1u) {
			level++;
		}
	} else if ((ft_rd(ft.gcc, 0x184f4) & 0x71fu) > 1u) {
		level = 8;
	}
	if (level == 8) {
		printf("FUXI TOUCH: no XO clock level\n");
		return EIO;
	}
	ft.level = level;
	ft_wr(ft.se, 0x07c, level);
	ft_wr(ft.se, 0x048, (2u << 4) | 1u);
	for (unsigned i = 0; i < 10000 && !ft_on(0x184e4); i++) {
		delay(1);
	}
	if (!ft_on(0x184e4)) {
		printf("FUXI TOUCH: SE4 clock off (cbcr %#x)\n", ft_rd(ft.gcc, 0x184e4));
		return EIO;
	}
	return 0;
}

/* DMA page: event ring (4 KiB, 256 events), TX and RX rings (1 KiB, 64 TREs
 * each), then the TX and RX data buffers. Cacheable: clean before a doorbell,
 * invalidate before reading what the hardware wrote. */
#define FT_EV_OFF    0x0000u
#define FT_RING_OFF(k) (0x1000u + 0x400u * (k))
#define FT_TXB_OFF   0x1800u
#define FT_RXB_OFF   0x1c00u
#define FT_RING      64u
#define FT_EVRING    256u
#define GPI_CH(k)    (0x0000u + 0x80u * (k))
#define GPI_EV       0x1000u
#define GPI_CHDB(k)  (0x2000u + 8u * (k))
#define GPI_EVDB     0x2100u
#define GPI_GPII     0x3000u

static void
ft_dma_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ft.dma_pa + off), len, TRUE);
}

static uint32_t
ft_state(uint32_t ctx)
{
	return (ft_rd(ft.gpi, ctx) >> 20) & 0xf;
}

static bool
ft_gpi_cmd(uint32_t reg, uint32_t v, uint32_t ctx, uint32_t want)
{
	ft_wr(ft.gpi, GPI_GPII + reg, v);
	for (unsigned i = 0; i < 50000; i++) {
		uint32_t st = ft_state(ctx);
		if (st == want) {
			ft_wr(ft.gpi, GPI_GPII + 0xa0, ft_rd(ft.gpi, GPI_GPII + 0x90));
			ft_wr(ft.gpi, GPI_GPII + 0xa4, ft_rd(ft.gpi, GPI_GPII + 0x94));
			return true;
		}
		if (st == 0xf) {
			break;
		}
		delay(1);
	}
	printf("FUXI TOUCH: GPI cmd %#x -> state %#x (want %u), error log %#x\n",
	    v, ft_state(ctx), want, ft_rd(ft.gpi, GPI_GPII + 0x200));
	return false;
}

/* Bus-masters without an SMR fault (sCR0.USFCFG) and the hypervisor turns an
 * S2CR bypass into fault: route GPI and QUP1 to CB4 like DWC3. Idempotent. */
static int
ft_smmu(void)
{
	static const uint16_t sid[] = {0xb6, 0xa3};
	uint32_t nsmr = ft_rd(ft.smmu, 0x20) & 0xff;
	for (unsigned j = 0; j < 2; j++) {
		unsigned i, freei = ~0u;
		for (i = 0; i < nsmr; i++) {
			uint32_t smr = ft_rd(ft.smmu, 0x800 + 4 * i);
			if (!(smr & 0x80000000u)) {
				if (freei == ~0u) {
					freei = i;
				}
				continue;
			}
			if (((sid[j] ^ smr) & ~(smr >> 16) & 0x7fff) == 0) {
				break;
			}
		}
		if (i < nsmr) {
			continue;
		}
		if (freei == ~0u) {
			return ENOSPC;
		}
		ft_wr(ft.smmu, 0xc00 + 4 * freei, 4);
		ft_wr(ft.smmu, 0x800 + 4 * freei, 0x80000000u | sid[j]);
		printf("FUXI TOUCH: SMR %u = sid %#x -> S2CR %#x\n", freei, sid[j],
		    ft_rd(ft.smmu, 0xc00 + 4 * freei));
	}
	return 0;
}

static int
ft_gpi_init(void)
{
	uint32_t lo = (uint32_t)ft.dma_pa, hi = (uint32_t)(ft.dma_pa >> 32);
	/* The loader leaves both channels started on its own rings: stop, reset, free. */
	for (unsigned k = 0; k < 2; k++) {
		if (ft_state(GPI_CH(k)) == 2) {
			(void)ft_gpi_cmd(0x08, (2u << 24) | k, GPI_CH(k), 3);
		}
		if (ft_state(GPI_CH(k))) {
			(void)ft_gpi_cmd(0x08, (9u << 24) | k, GPI_CH(k), 1);
			ft_wr(ft.gpi, GPI_GPII + 0x08, (10u << 24) | k);
			delay(1000);
		}
	}
	if (ft_state(GPI_EV)) {
		(void)ft_gpi_cmd(0x10, 9u << 24, GPI_EV, 1);
		ft_wr(ft.gpi, GPI_GPII + 0x10, 10u << 24);
		delay(1000);
	}
	bzero((void *)ft.dma, PAGE_SIZE);
	ft_dma_sync(0, PAGE_SIZE);
	ft_wr(ft.gpi, GPI_GPII + 0x88, 0x4f);
	ft_wr(ft.gpi, GPI_GPII + 0xb8, 1);
	ft_wr(ft.gpi, GPI_GPII + 0x98, 3);
	ft_wr(ft.gpi, GPI_GPII + 0x9c, 1);
	ft_wr(ft.gpi, GPI_GPII + 0x108, 1);
	ft_wr(ft.gpi, GPI_GPII + 0x120, 0xf);
	ft_wr(ft.gpi, GPI_GPII + 0x188, 0);
	ft_wr(ft.gpi, GPI_GPII + 0x18c, 0);
	ft_wr(ft.gpi, GPI_GPII + 0x400, 0);
	ft_wr(ft.gpi, GPI_GPII + 0x404, 0);
	ft_wr(ft.gpi, GPI_GPII + 0x180, 1);
	ft_wr(ft.gpi, GPI_GPII + 0x200, 0);
	if (!ft_gpi_cmd(0x10, 0, GPI_EV, 1)) {
		return EIO;
	}
	ft_wr(ft.gpi, GPI_EV + 0x00, 0x10010002u);
	ft_wr(ft.gpi, GPI_EV + 0x04, FT_EVRING * 16);
	ft_wr(ft.gpi, GPI_EV + 0x08, lo + FT_EV_OFF);
	ft_wr(ft.gpi, GPI_EV + 0x0c, hi);
	ft_wr(ft.gpi, GPI_EVDB + 4, hi);
	for (uint32_t o = 0x20; o <= 0x34; o += 4) {
		ft_wr(ft.gpi, GPI_EV + o, 0);
	}
	ft.evrp = 0;
	ft_wr(ft.gpi, GPI_EVDB, lo + FT_EV_OFF + FT_EVRING * 16 - 16);
	for (unsigned k = 0; k < 2; k++) {
		if (!ft_gpi_cmd(0x08, k, GPI_CH(k), 1)) {
			return EIO;
		}
		ft_wr(ft.gpi, GPI_CH(k) + 0x00, k ? 0x10000002u : 0x1000000au);
		ft_wr(ft.gpi, GPI_CH(k) + 0x04, FT_RING * 16);
		ft_wr(ft.gpi, GPI_CH(k) + 0x08, lo + FT_RING_OFF(k));
		ft_wr(ft.gpi, GPI_CH(k) + 0x0c, hi);
		ft_wr(ft.gpi, GPI_CHDB(k) + 4, hi);
		/* SCRATCH_0: pair, SPI protocol 1, SE 4. */
		ft_wr(ft.gpi, GPI_CH(k) + 0x60, k ? 0x00000014u : 0x00010014u);
		ft_wr(ft.gpi, GPI_CH(k) + 0x64, 0);
		ft_wr(ft.gpi, GPI_CH(k) + 0x68, 0);
		ft_wr(ft.gpi, GPI_CH(k) + 0x6c, 0);
		ft_wr(ft.gpi, GPI_CH(k) + 0x5c, 1);
		ft.wp[k] = 0;
	}
	for (unsigned k = 0; k < 2; k++) {
		if (!ft_gpi_cmd(0x08, (1u << 24) | k, GPI_CH(k), 2)) {
			return EIO;
		}
	}
	return 0;
}

static int
ft_se_init(void)
{
	uint32_t fw = ft_rd(ft.se, 0x068);
	if (((fw >> 8) & 0xff) != 1 || (ft_rd(ft.se, 0x040) & 1)) {
		printf("FUXI TOUCH: SE4 fw %#x status %#x: not idle SPI\n", fw, ft_rd(ft.se, 0x040));
		return ENODEV;
	}
	/* geni_se_select_mode(GENI_GPI_DMA): the CONFIG0 TRE sets mode, word
	 * size, packing and clock. */
	ft_wr(ft.se, 0xe18, 0);
	ft_wr(ft.se, 0x618, 0xffffffffu);
	ft_wr(ft.se, 0x648, 0xffffffffu);
	ft_wr(ft.se, 0xc44, 0xffffffffu);
	ft_wr(ft.se, 0xd44, 0xffffffffu);
	ft_wr(ft.se, 0x028, ft_rd(ft.se, 0x028) | 0x7f);
	ft_wr(ft.se, 0xe30, ft_rd(ft.se, 0xe30) | 0xf);
	ft_wr(ft.se, 0x024, 0x7f);
	ft_wr(ft.se, 0x020, 1);
	ft_wr(ft.se, 0xe1c, 0);
	ft_wr(ft.se, 0x614, ft_rd(ft.se, 0x614) & ~((1u << 0) | (1u << 26) | (1u << 27) | (1u << 30)));
	ft_wr(ft.se, 0x644, ft_rd(ft.se, 0x644) & ~1u);
	ft_wr(ft.se, 0x258, 1);
	ft_wr(ft.se, 0xe18, ft_rd(ft.se, 0xe18) | 0xf);
	return 0;
}

static void
ft_tre(unsigned k, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t off = FT_RING_OFF(k) + 16 * ft.wp[k];
	volatile uint32_t *t = (volatile uint32_t *)(ft.dma + off);
	t[0] = d0;
	t[1] = d1;
	t[2] = d2;
	t[3] = d3;
	ft_dma_sync(off, 16);
	ft.wp[k] = (ft.wp[k] + 1) % FT_RING;
}

static void
ft_ring_db(unsigned k)
{
	__asm__ volatile("dsb sy" ::: "memory");
	ft_wr(ft.gpi, GPI_CHDB(k), (uint32_t)ft.dma_pa + FT_RING_OFF(k) + 16 * ft.wp[k]);
}

/* Drain events until an end-of-transfer (code 2) from chid arrives. */
static int
ft_gpi_wait(unsigned chid, uint32_t *len)
{
	for (unsigned us = 0; us < 5000; us++) {
		uint32_t hw = ft_rd(ft.gpi, GPI_EV + 0x10);
		bool found = false, bad = false;
		while (hw != (uint32_t)ft.dma_pa + FT_EV_OFF + 16 * ft.evrp) {
			uint32_t off = FT_EV_OFF + 16 * ft.evrp;
			ft_dma_sync(off, 16);
			volatile uint32_t *e = (volatile uint32_t *)(ft.dma + off);
			uint32_t d2 = e[2], d3 = e[3];
			ft.evrp = (ft.evrp + 1) % FT_EVRING;
			unsigned code = d2 >> 24, type = (d3 >> 16) & 0xff, ch = d3 >> 24;
			if (code == 16 || type == 0xff || ch >= 2) {
				bad = true;
				printf("FUXI TOUCH: GPI event %#x %#x\n", d2, d3);
			} else if (type == 0x22 && ch == chid && code == 2) {
				found = true;
				*len = d2 & 0xffffff;
			}
		}
		ft_wr(ft.gpi, GPI_EVDB, (uint32_t)ft.dma_pa + FT_EV_OFF +
		    16 * ((ft.evrp + FT_EVRING - 1) % FT_EVRING));
		ft_wr(ft.gpi, GPI_GPII + 0xc0, 1);
		if (bad) {
			return EIO;
		}
		if (found) {
			return 0;
		}
		delay(1);
	}
	return ETIMEDOUT;
}

#define FT_C0()  ft_tre(0, 0x03000004u, 0, (ft.level << 16) | 2u, 0x00220001u)

static int
ft_tx(const uint8_t *tx, unsigned nt, bool frag)
{
	uint32_t len = 0;
	int err;
	memcpy((void *)(ft.dma + FT_TXB_OFF), tx, nt);
	ft_dma_sync(FT_TXB_OFF, nt);
	FT_C0();
	ft_tre(0, (frag ? (1u << 26) : 0) | 1u, 0, 0, 0x00200001u);
	ft_tre(0, (uint32_t)ft.dma_pa + FT_TXB_OFF, (uint32_t)(ft.dma_pa >> 32), nt, 0x00100200u);
	ft_ring_db(0);
	if ((err = ft_gpi_wait(0, &len))) {
		return err;
	}
	return len == nt ? 0 : EIO;
}

/* FTS read: opcode + address (CS held), then one dummy byte and n data bytes. */
static int
ft_read(const uint8_t *cmd, unsigned nc, uint8_t *out, unsigned n)
{
	uint32_t len = 0;
	int err;
	if (nc > 8 || n > 8 * 32) {
		return EINVAL;
	}
	if ((err = ft_tx(cmd, nc, true))) {
		return err;
	}
	ft_dma_sync(FT_RXB_OFF, n + 1);
	ft_tre(1, (uint32_t)ft.dma_pa + FT_RXB_OFF, (uint32_t)(ft.dma_pa >> 32), n + 1, 0x00100200u);
	ft_tre(0, 2u, 0, n + 1, 0x00200900u);
	ft_ring_db(1);
	ft_ring_db(0);
	if ((err = ft_gpi_wait(1, &len))) {
		return err;
	}
	ft_dma_sync(FT_RXB_OFF, n + 1);
	memcpy(out, (const void *)(ft.dma + FT_RXB_OFF + 1), n);
	return 0;
}

static uint64_t
ft_us(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

/* One line per contact: how often iOS got a report and what a FIFO read costs. */
static void
ft_contact_end(void)
{
	uint64_t dur = ft_us(ft.c_last - ft.c_start);
	ft.contacts++;
	printf("FUXI TOUCH: contact %u: %u reports in %llu ms (%llu us apart), %u polls, "
	    "FIFO read avg %llu us max %llu us, multi %u max %u lifts %u\n",
	    ft.contacts, ft.c_reports, dur / 1000,
	    ft.c_reports > 1 ? dur / (ft.c_reports - 1) : 0ULL, ft.c_polls,
	    ft.c_polls ? ft_us(ft.c_poll_sum) / ft.c_polls : 0ULL, ft_us(ft.c_poll_max),
	    ft.c_multi, ft.c_max, ft.c_lifts);
}

extern void fuxi_perf_input(void);
static void
ft_report(uint32_t mask)
{
	fuxi_perf_input();
	uint64_t now = mach_absolute_time();
	if (mask && !ft.active) {
		ft.c_start = now;
		ft.c_reports = ft.c_polls = 0;
		ft.c_poll_sum = ft.c_poll_max = 0;
		ft.c_multi = ft.c_max = ft.c_lifts = 0;
	}
	unsigned down_count = 0;
	for (unsigned id = 0; id < FT_MAX; id++) {
		uint32_t bit = 1u << id;
		if (mask & bit) {
			down_count++;
			if (!ft.batch) {
				sweet_hid_touch((int)id, 1, ft.x[id], ft.y[id]);
			}
		} else if (ft.active & bit) {
			ft.c_lifts++;
			if (!ft.batch) {
				sweet_hid_touch((int)id, 0, ft.x[id], ft.y[id]);
			}
		}
	}
	if (ft.batch && (mask || ft.active)) {
		ft.retry = !sweet_hid_touch_report(mask, ft.x, ft.y);
	}
	ft.c_multi += down_count >= 2;
	ft.c_max = down_count > ft.c_max ? down_count : ft.c_max;
	if (mask || ft.active) {
		ft.c_reports++;
		ft.c_last = now;
	}
	bool ended = ft.active && !mask;
	ft.active = mask;
	if (ended) {
		ft_contact_end();
	}
}

/* Retry the latest set, including final all-up, without counting a new sample. */
static void
ft_retry_report(void)
{
	if (ft.retry) {
		ft.retry = !sweet_hid_touch_report(ft.active, ft.x, ft.y);
	}
}

/*
 * Drain the FIFO in two reads (as fts_event_handler): one event, whose
 * byte 7 holds the number of events still queued, then all of those at
 * once. With deliver false (display off) the events are consumed and
 * dropped.
 */
static int
ft_poll(bool deliver)
{
	static const uint8_t fifo[] = {0x87};
	uint8_t e[8 * 32];
	uint32_t mask = ft.active;
	bool changed = false;
	int err = ft_read(fifo, 1, e, 8);
	if (err) {
		return err;
	}
	if (e[0] == 0) {
		return 0;
	}
	unsigned n = 1 + (e[7] & 0x1f);
	if (n > 32) {
		n = 32;
	}
	if (n > 1 && (err = ft_read(fifo, 1, e + 8, 8 * (n - 1)))) {
		return err;
	}
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *ev = e + 8 * i;
		if (ev[0] == 0) {
			break;
		}
		if (ev[0] == FT_EV_READY) {
			ft.ic_reset = true;
			continue;
		}
		unsigned id = ev[1] >> 4, type = ev[1] & 0xf;
		if ((ev[0] != FT_EV_ENTER && ev[0] != FT_EV_MOTION && ev[0] != FT_EV_LEAVE) ||
		    id >= FT_MAX || (type == 0 && ev[0] != FT_EV_LEAVE)) {
			continue;
		}
		bool touching = ev[0] != FT_EV_LEAVE && type != FT_TYPE_HOVER, fresh;
		if (fuxi_wake_touch_event(&ft.wake, !deliver, id, touching, &fresh)) {
			if (fresh && fuxi_keys_touch_wake()) {
				fuxi_wake_touch_woke(&ft.wake);
			}
			if (deliver) {
				continue;      /* a contact that woke the display: not for iOS */
			}
		}
		/* Super-resolution coordinates, 10x the 1080x2400 panel. */
		int x = ((ev[3] << 8) | ev[2]) / 10, y = ((ev[5] << 8) | ev[4]) / 10;
		ft.x[id] = x > 1079 ? 1079 : x;
		ft.y[id] = y > 2399 ? 2399 : y;
		if (ev[0] == FT_EV_LEAVE || type == FT_TYPE_HOVER) {
			mask &= ~(1u << id);
		} else {
			mask |= 1u << id;
		}
		changed = true;
	}
	if (changed && deliver) {
		ft_report(mask);
	}
	return 0;
}

static void
ft_thread(void *arg, wait_result_t result)
{
	static const uint8_t id_cmd[] = {0xfb, 0x20, 0x00, 0x00, 0x00}, fifo[] = {0x87};
	static const uint8_t sense[] = {0xa0, 0x00, 0x01};
	uint8_t ev[8] = {0}, chip[2] = {0};
	unsigned delay_s = 30, errors = 0;
	unsigned poll_us = 4000, idle_us = 8000, offsup = 1;
	bool irq_seen = false, was_off = false;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("fuxitouchdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("fuxitouch_poll_us", &poll_us, sizeof(poll_us));
	(void)PE_parse_boot_argn("fuxitouch_idle_us", &idle_us, sizeof(idle_us));
	(void)PE_parse_boot_argn("fuxitouch_offsuppress", &offsup, sizeof(offsup));
	(void)PE_parse_boot_argn("fuxitbatch", &ft.batch, sizeof(ft.batch));
	poll_us = poll_us < 1000 ? 1000 : poll_us;
	idle_us = idle_us < poll_us ? poll_us : idle_us;
	printf("FUXI TOUCH: starting in %u s (poll %u us active, %u us idle)\n", delay_s,
	    poll_us, idle_us);
	if (ft.batch) {
		printf("FUXI TOUCH: report collections enabled (fuxitbatch=1)\n");
	}
	ft_sleep(delay_s * 1000u);
	ft.gcc = ml_io_map(FT_GCC_PA, 0x1f0000);
	ft.tlmm = ml_io_map(FT_TLMM_PA, 0x30000);
	ft.se = ml_io_map(FT_SE_PA, 0x4000);
	ft.gpi = ml_io_map(FT_GPI_PA, 0x4000);
	ft.smmu = ml_io_map(FT_SMMU_PA, 0x1000);
	if (!ft.gcc || !ft.tlmm || !ft.se || !ft.gpi || !ft.smmu) {
		goto out;
	}
	if (kmem_alloc_contig(kernel_map, &ft.dma, PAGE_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("FUXI TOUCH: no DMA memory\n");
		goto out;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)ft.dma);
	if (!pn) {
		goto out;
	}
	ft.dma_pa = ptoa_64(pn);
	int ierr;
	if ((ierr = ft_clock()) || (ierr = ft_se_init()) || (ierr = ft_smmu()) || (ierr = ft_gpi_init())) {
		printf("FUXI TOUCH: init failed %d\n", ierr);
		goto out;
	}
	printf("FUXI TOUCH: GPI up, DMA page %#llx\n", ft.dma_pa);
	ft_gpio(FT_IRQ, 0, 3, 8, -1);
	ft_gpio(44, 1, 1, 6, -1);
	ft_gpio(45, 1, 1, 6, -1);
	ft_gpio(46, 1, 1, 6, -1);
	ft_gpio(47, 1, 3, 6, -1);
	/* fts_system_reset: reset low 10 ms, high, then poll for controller ready. */
	ft_gpio(FT_RESET, 0, 3, 8, 0);
	ft_sleep(10);
	ft_gpio(FT_RESET, 0, 3, 8, 1);
	int err = EIO;
	for (unsigned t = 0; t < 100; t++) {
		ft_sleep(2);
		if ((err = ft_read(fifo, 1, ev, 8)) || ev[0] == FT_EV_READY) {
			break;
		}
	}
	int cerr = ft_read(id_cmd, 5, chip, 2);
	printf("FUXI TOUCH: reset err=%d event %02x %02x %02x %02x; chip id err=%d %02x%02x\n",
	    err, ev[0], ev[1], ev[2], ev[3], cerr, chip[0], chip[1]);
	if (err || cerr || ev[0] != FT_EV_READY) {
		goto out;
	}
	if ((err = ft_tx(sense, 3, false))) {
		printf("FUXI TOUCH: sense on failed %d\n", err);
		goto out;
	}
	printf("FUXI TOUCH: sensing, polling GPIO%u\n", FT_IRQ);
	/*
	 * IRQ is level low while the FIFO holds events. Check it every poll
	 * period (absolute deadlines, no timer leeway) and read the FIFO every
	 * 32 ms regardless in case the IC does not drive the line.
	 */
	uint64_t poll_abs, idle_abs, off_abs, recent_abs, sweep_abs;
	nanoseconds_to_absolutetime(poll_us * 1000ULL, &poll_abs);
	nanoseconds_to_absolutetime(idle_us * 1000ULL, &idle_abs);
	nanoseconds_to_absolutetime(32 * NSEC_PER_MSEC, &off_abs);
	nanoseconds_to_absolutetime(32 * NSEC_PER_MSEC, &sweep_abs);
	nanoseconds_to_absolutetime(150 * NSEC_PER_MSEC, &recent_abs);
	uint64_t next = mach_absolute_time(), last_irq = 0, last_read = 0;
	for (;;) {
		uint64_t now = mach_absolute_time();
		bool off = offsup && sweet_fb_display_off;
		if (off != was_off) {
			was_off = off;
			fuxi_wake_touch_display(&ft.wake, off);
			if (off && ft.active) {
				ft_report(0);   /* release fingers held when the display went off */
			}
		}
		bool irq = !(ft_rd(ft.tlmm, FT_GPIO(FT_IRQ) + 4) & 1u);
		if (irq) {
			last_irq = now;
			if (!irq_seen) {
				irq_seen = true;
				printf("FUXI TOUCH: IRQ line asserted\n");
			}
		}
		if (irq || ft.active || now - last_read >= sweep_abs) {
			last_read = now;
			int perr = ft_poll(!off);
			uint64_t took = mach_absolute_time() - now;
			if (ft.active) {
				ft.c_polls++;
				ft.c_poll_sum += took;
				ft.c_poll_max = took > ft.c_poll_max ? took : ft.c_poll_max;
			}
			if (perr) {
				errors++;
				if (errors == 1 || (errors % 100u) == 0) {
					printf("FUXI TOUCH: FIFO read error %d (%u in a row)\n", perr, errors);
				}
				/* Do not leave a finger stuck down while reads fail. */
				if (errors >= 2 && ft.active) {
					ft_report(0);
				}
				if (errors >= 2) {
					fuxi_wake_touch_reset(&ft.wake);
				}
				/* A timed-out transfer leaves TREs queued: reset the GPII rings. */
				if (errors == 3 || (errors % 50u) == 0) {
					int rerr = ft_gpi_init();
					printf("FUXI TOUCH: GPI re-init after %u errors -> %d\n", errors, rerr);
				}
			} else {
				if (errors >= 3) {
					printf("FUXI TOUCH: FIFO reads recovered after %u errors\n", errors);
				}
				errors = 0;
			}
			if (ft.ic_reset) {
				/* Controller-ready event while sensing: the IC reset itself. */
				ft.ic_reset = false;
				if (ft.active) {
					ft_report(0);
				}
				fuxi_wake_touch_reset(&ft.wake);
				int serr = ft_tx(sense, 3, false);
				printf("FUXI TOUCH: controller reset event, sense on again -> %d\n", serr);
			}
		}
		ft_retry_report();
		uint64_t period = off ? off_abs :
		    (ft.active || now - last_irq < recent_abs) ? poll_abs : idle_abs;
		next += period;
		now = mach_absolute_time();
		if (next <= now) {
			next = now + period;
		}
		ft_sleep_until(next);
	}
out:
	printf("FUXI TOUCH: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
fuxi_touch_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (ft.started) {
		return;
	}
	ft.started = true;
	(void)PE_parse_boot_argn("fuxitouch", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (kernel_thread_start_priority(ft_thread, NULL, FT_PRIORITY, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* FUXI */
