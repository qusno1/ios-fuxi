#!/usr/bin/env python3
"""P17 host tests: opt-in WCN7850 MODE-silence probes (fuxiwlanmlo bits >= 4).

Part 1 compiles the pure P17 helpers (MLO link choice, PHY TLV 0x17, DMA
alignment, sleep clock plan, error words, RDDM text scan) with ASan/UBSan
and checks exact values and byte sequences. Part 2 reruns the W12 QMI host
harness with extra cases: the HOST_CAP bytes are compared against a
Python encoding that appends vendor TLV 0x33 (u8 1) after 0x27.
Pin sequence, alignment rule and text scan follow UniXNU-sheng WifiProbe.cpp
(BSD-2-Clause-Patent); TLV 0x33/0x17 come from the lane brief and are
unverified against firmware. No device access.
"""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
DRIVER = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()

PURE_TEST = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
%BLOCK%
static unsigned nhits; static char hits[8][100]; static uint64_t offs[8];
static void emit(uint64_t off, const char *t) { assert(nhits < 8); offs[nhits] = off; strcpy(hits[nhits++], t); }
int main(void) {
	/* MLO links: (variant, bits, enabled, num) */
	assert(wl17_mlo_links(0, 0, true, 2) == 2 && wl17_mlo_links(0, 0, true, 1) == 1);
	assert(wl17_mlo_links(2, WL17_MLOALL, true, 2) == 0);
	assert(wl17_mlo_links(1, 0, false, 2) == 0 && wl17_mlo_links(1, 0, true, 0) == 0);
	assert(wl17_mlo_links(1, 0, true, 255) == 0 && wl17_mlo_links(1, 0, true, 3) == -1);
	assert(wl17_mlo_links(1, 0, true, 254) == -1);
	assert(wl17_mlo_links(1, WL17_MLOALL, false, 2) == 2 && wl17_mlo_links(1, WL17_MLOALL, false, 1) == 1);
	assert(wl17_mlo_links(1, WL17_MLOALL, true, 0) == 2 && wl17_mlo_links(1, WL17_MLOALL, true, 255) == 2);
	assert(wl17_mlo_links(1, WL17_MLOALL, true, 3) == 2 && wl17_mlo_links(1, WL17_MLOALL, true, 254) == 2);
	assert(wl17_mlo_links(0, WL17_HOSTMEM, true, 2) == 2);
	/* PHY TLV 0x17 */
	const uint8_t b[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	wl17_phy17_parse(b, 3); assert(wl17_phy17_len == 3 && wl17_phy17_val == 0x030201);
	wl17_phy17_parse(b, 10); assert(wl17_phy17_len == 10 && wl17_phy17_val == 0x0807060504030201ull);
	wl17_phy17_parse(b, 0); assert(wl17_phy17_len == 0 && wl17_phy17_val == 0);
	/* DMA alignment: size rounded up to a power of two */
	assert(wl17_align(0x4000) == 0x4000 && wl17_align(0x4001) == 0x8000 && wl17_align(0x30000) == 0x40000);
	assert(wl17_align(0x800000) == 0x800000 && wl17_align(0x800001) == 0x1000000 && wl17_align(1) == 4096);
	/* Sleep clock: PPID, write order, decision */
	assert(WL17_CLK_PPID == 0x0ba);
	const uint8_t order[10] = {0x42, 5, 0x41, 0, 0x44, 2, 0x40, 1, 0x46, 0x80};
	for (unsigned i = 0; i < 5; i++) { assert(wl17_clk_writes[i].reg == order[2*i] && wl17_clk_writes[i].val == order[2*i+1]); }
	struct wl17_gpio on = {0x10, 0x11, 1, 2, 0x80}, off = {0x10, 0x11, 0, 0, 0}, hi = {0x10, 0x11, 1, 0x12, 0x80};
	assert(wl17_clk_state(&on, 0) == 0xff && wl17_clk_state(&on, 2) == 0xff);
	assert(wl17_clk_state(&hi, 0) == 0xff); /* low nibble of the function byte */
	assert(wl17_clk_state(&off, 0) == 0 && wl17_clk_state(&off, 2) == 3);
	struct wl17_gpio noen = {0x10, 0x11, 1, 2, 0}, nofn = {0x10, 0x11, 1, 3, 0x80}, odd = {0x10, 0x0f, 0, 0, 0}, modein = {0x10, 0x11, 0, 2, 0x80};
	assert(wl17_clk_state(&noen, 0) == 0 && wl17_clk_state(&nofn, 0) == 0 && wl17_clk_state(&modein, 0) == 0);
	assert(wl17_clk_state(&odd, 0) == 4);
	/* Error words */
	char t[160];
	wl17_errword(0xcafecace, t, sizeof t);
	assert(!strcmp(t, "0xcafecace hi16 0xcafe lo16 0xcace bytes(LE) '....' class: 0xcafe-prefixed sentinel word"));
	wl17_errword(0x014feeb3, t, sizeof t);
	assert(!strcmp(t, "0x014feeb3 hi16 0x14f lo16 0xeeb3 bytes(LE) '..O.' class: plain value"));
	wl17_errword(0x496, t, sizeof t);
	assert(!strcmp(t, "0x00000496 hi16 0 lo16 0x496 bytes(LE) '....' class: plain value"));
	wl17_errword(0, t, sizeof t); assert(strstr(t, "class: zero"));
	wl17_errword(0xffffffffu, t, sizeof t); assert(strstr(t, "all-ones"));
	wl17_errword(0xdeadbeef, t, sizeof t); assert(strstr(t, "0xdead-prefixed"));
	wl17_errword(0x41424344, t, sizeof t); assert(strstr(t, "'DCBA'"));
	/* RDDM text scan */
	uint8_t buf[512]; memset(buf, 0, sizeof buf);
	const char *a = "QC: wlan_fw.c:1234 assertion failed", *c = "short.c:1", *f = "err_fatal handler %s ssert", *d = "xx FATAL error in rtos";
	memcpy(buf + 3, a, strlen(a));
	memcpy(buf + 70, c, strlen(c));          /* 9 bytes: below the 10 byte minimum */
	memcpy(buf + 90, f, strlen(f));          /* format string: rejected */
	memcpy(buf + 130, a, strlen(a));         /* duplicate: reported once */
	memcpy(buf + 200, d, strlen(d));
	buf[300] = 'o'; memcpy(buf + 301, "k.c:12 is long enough", 21); buf[322] = 0xff;
	unsigned n = wl17_scan(buf, sizeof buf, 40, emit);
	assert(n == 3 && nhits == 3);
	assert(offs[0] == 3 && !strcmp(hits[0], a));
	assert(offs[1] == 200 && !strcmp(hits[1], d));
	assert(offs[2] == 300 && !strcmp(hits[2], "ok.c:12 is long enough"));
	nhits = 0; assert(wl17_scan(buf, sizeof buf, 1, emit) == 1 && nhits == 1);
	/* a 300 byte printable run is cut at 200 and still bounded */
	uint8_t big[300]; memset(big, 'a', sizeof big); memcpy(big, "rr_fatal: ", 10);
	nhits = 0; wl17_scan(big, sizeof big, 40, emit); assert(nhits >= 1 && strlen(hits[0]) == 84);
	puts("P17 pure helpers passed");
	return 0;
}
'''


def pure():
    block = DRIVER.split('/* ---- P17:', 1)[1].split('/* ---- end P17 pure helpers ---- */', 1)[0]
    block = '/* ---- P17:' + block
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-p17-') as tmp:
        src = Path(tmp) / 'p17.c'
        src.write_text(PURE_TEST.replace('%BLOCK%', block))
        exe = Path(tmp) / 'p17'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-O1', '-g', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def tlv(tag, body):
    return bytes([tag, len(body) & 255, len(body) >> 8]) + body


def integration():
    import fuxi_wlan_mlo_test as mlo
    spec = importlib.util.spec_from_file_location('w12_p17', ROOT / 'tools/test-fuxi-wlan-w12.py')
    w12 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w12)
    base = len(mlo.CASES)
    pb, orig_host = mlo.phy_body, mlo.host_body
    # n >= 100: vendor TLV 0x33 follows 0x27; n % 100 MLO links.
    mlo.host_body = lambda n: orig_host(n % 100) + (tlv(0x33, b'\x01') if n >= 100 else b'')
    wide = (0x0807060504030201).to_bytes(8, 'little')
    mlo.CASES += [
        (4, b'', 102),
        (1 | 4 | 0x10, pb(num=1) + tlv(0x17, b'\x05'), 101),
        (1 | 8, pb(support=0), 2),
        (1 | 8, pb(num=3), 2),
        (1 | 8, pb(num=1, support=0), 1),
        (1 | 8, pb(num=255), 2),
        (8, b'', 2),
        (1 | 0x10, pb(num=2) + tlv(0x17, wide), 2),
        (1 | 0x10, pb(num=1) + tlv(0x17, b''), 1),
        (1 | 4 | 8, pb(num=1, support=0), 101),
        (1 | 0x20 | 0x40 | 0x80 | 0x100, pb(), 2),
        (1 | 0x10, pb(num=1) + tlv(0x17, b'\x07'), 1),
    ]
    assert base == 24, 'existing W12 cases moved; update the P17 case indices'
    src = mlo.tests(w12.PEER)
    src = src.replace('mlo_arg==1', '(mlo_arg&3)==1')
    src = src.replace('wl12_mlo_variant==variants[mlo_case]', 'wl12_mlo_variant==(variants[mlo_case]&3)')
    checks = r'''
        if(mlo_case==25)assert(strstr(logbuf,"P17 phy cap TLV 0x17 len 1 value 0x5") && strstr(logbuf,"vendor TLV 0x33"));
        if(mlo_case==24)assert(strstr(logbuf,"vendor TLV 0x33") && !strstr(logbuf,"phy cap TLV"));
        if(mlo_case>=26 && mlo_case<=30)assert(!strstr(logbuf,"vendor TLV 0x33") && strstr(logbuf,"MLO TLVs present"));
        if(mlo_case==31)assert(strstr(logbuf,"P17 phy cap TLV 0x17 len 8 value 0x807060504030201"));
        if(mlo_case==32)assert(strstr(logbuf,"P17 phy cap TLV 0x17 len 0 value 0"));
        if(mlo_case==33)assert(strstr(logbuf,"vendor TLV 0x33") && strstr(logbuf,"MLO TLVs present"));
        if(mlo_case==35)assert(strstr(logbuf,"P17 phy cap TLV 0x17 len 1 value 0x7"));
        if(mlo_case==34) {
            assert(strstr(logbuf,"P17 dma align") && !strstr(logbuf,"vendor TLV 0x33"));
            for(unsigned i=0;i<wl_target_n;i++)if(wl_target[i].va)
                assert(!(wl_target[i].iova&(wl17_align(wl_target[i].size)-1)));
        }
        if(mlo_case<24)assert(!strstr(logbuf,"P17"));
        if(mlo_case==13)'''
    assert src.count('        if(mlo_case==13)assert') == 1
    src = src.replace('        if(mlo_case==13)assert', checks + 'assert', 1)
    sel = r'''
    /* P17 selector validity: legacy low bits, P17 bits, and pre-W12 handling. */
    {
        const struct {int v, bad;} t[]={{0,0},{1,0},{2,0},{3,1},{4,0},{5,0},{8,0},{9,0},{0xa,1},{0x10,1},{0x11,0},
            {0x12,1},{0x1fd,0},{0x1fc,1},{0x1ff,1},{0x200,1},{0x201,1},{-1,1},{0x100,0},{0x40,0},{0x80,0},{0x20,0}};
        for(unsigned i=0;i<sizeof(t)/sizeof(t[0]);i++) {
            qrtr_arg=2;mlo_present=true;mlo_arg=t[i].v;wl_probe_configure();
            assert(wl_variant_invalid==(bool)t[i].bad);
            if(!t[i].bad)assert(wl17_bits==((unsigned)t[i].v&WL17_MASK) && wl12_mlo_variant==((unsigned)t[i].v&3));
            else assert(!wl17_bits);
        }
        qrtr_arg=1;mlo_arg=0x40;wl_probe_configure();
        assert(!wl_variant_invalid && wl17_bits==0x40 && !wl12_mlo_variant);
        mlo_arg=2;wl_probe_configure();assert(!wl_variant_invalid && !wl17_bits && !wl12_mlo_variant);
        mlo_present=false;wl_probe_configure();assert(!wl17_bits);
        /* IOVA alignment is applied in wl_dma_alloc only with the bit. */
        struct wl_dma d={0}, e={0};
        wl17_bits=0;wl_iova_next=wl_iova_base+PAGE_SIZE;assert(!wl_dma_alloc(&d,0x30000) && d.iova==wl_iova_base+PAGE_SIZE);
        wl_dma_free(&d);
        wl17_bits=WL17_DMAALIGN;wl_iova_next=wl_iova_base+PAGE_SIZE;
        assert(!wl_dma_alloc(&e,0x30000) && !(e.iova&0x3ffff) && e.iova>wl_iova_base+PAGE_SIZE);
        wl_dma_free(&e);wl17_bits=0;wl_iova_next=wl_iova_base;
        /* ERRDEC: four decoded words per diagnostic, none without the bit. */
        wl17_bits=0;loglen=0;logbuf[0]=0;wl_wlfw_diagnostics("T");assert(!strstr(logbuf,"P17"));
        wl17_bits=WL17_ERRDEC;loglen=0;logbuf[0]=0;wl_wlfw_diagnostics("T");
        assert(strstr(logbuf,"P17 T ERRCODE") && strstr(logbuf,"P17 T ERRDBG1") && strstr(logbuf,"P17 T ERRDBG3"));
        wl17_bits=0;
        mlo_present=true;
    }
    const unsigned variants[]='''
    assert src.count('    const unsigned variants[]=') == 1
    src = src.replace('    const unsigned variants[]=', sel, 1)
    src = src.replace('puts("W12 PHY_CAP', 'puts("W12+P17 PHY_CAP', 1)
    w12.TESTS = src
    w12.main()


if __name__ == '__main__':
    assert 'python3 tools/test-fuxi-wlan-w17.py' in (ROOT / 'tools/ci/build.sh').read_text()
    pure()
    integration()
