#!/usr/bin/env python3
"""GPU DCVS governor (fuxigpulevel bit 4): the real policy header and the real
driver functions run against a virtual clock, thread_call and HFI stub, offline.
Not a device test: vote effect on frequency, heat and frame time is unobserved."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = (ROOT/'xnu/fuxi-adreno/fuxi_adreno.c').read_text()
BACKEND = (ROOT/'xnu/fuxi-adreno/fuxi_gpu_abi_backend.inc').read_text()

PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gpu-clock.h"
#include "fuxi_gpu_dcvs.h"
typedef void *thread_call_param_t;
typedef struct {int pending; uint64_t at; void (*fn)(thread_call_param_t, thread_call_param_t);} thread_call_data_t;
#define THREAD_CALL_PRIORITY_KERNEL 0
#define THREAD_CALL_OPTIONS_ONCE 0
#define NSEC_PER_MSEC 1000000ull
static uint64_t now_ticks;
static uint64_t mach_absolute_time(void) {return now_ticks;}
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) {*ns = t;}
static void thread_call_setup_with_options(thread_call_data_t *c, void (*fn)(thread_call_param_t, thread_call_param_t), void *p, int a, int b) {(void)p;(void)a;(void)b;c->fn = fn;c->pending = 0;}
static int thread_call_enter(thread_call_data_t *c) {int was = c->pending; c->pending = 1; c->at = now_ticks; return was;}
static int thread_call_enter_delayed(thread_call_data_t *c, uint64_t at) {c->pending = 1; c->at = at; return 0;}
static unsigned g_gpu_selected, fg_access_failed, ready = 1;
static uint32_t gpu_extra[1][128];
static int fuxi_gpu_ready(void) {return ready;}
static unsigned votes, fail_votes, last_level, last_bw;
static int g_hfi_send(unsigned id, uint32_t *msg, unsigned words, unsigned record)
{
 assert(id == 30 && words == 4 && record == 4 && msg[0] == 0 && msg[1] == 1);
 for (unsigned i = 0; i < 4; i++) gpu_extra[0][56+i] = 0xdead0000u + i; /* what g_hfi_send clobbers */
 votes++; last_level = msg[2]; last_bw = msg[3];
 if (fail_votes) {fail_votes--; return 0x903;}
 return 0;
}
'''
CASES = r'''
/* run the pending call if its time has come */
static int step(thread_call_data_t *c, uint64_t to) {
 if (!c->pending || c->at > to) return 0;
 now_ticks = c->at > now_ticks ? c->at : to;
 c->pending = 0; c->fn(0, 0); return 1;
}
static void reset(void) {
 memset(&fg_dcvs_call, 0, sizeof fg_dcvs_call);
 now_ticks = 1000; votes = fail_votes = 0; ready = 1; fg_access_failed = 0;
 fg_dcvs_on = 1; fg_dcvs_cap = 6; fg_dcvs_hold_ms = 100; fg_dcvs_hold = 100*NSEC_PER_MSEC;
 fg_dcvs_level = 0; fg_dcvs_ups = fg_dcvs_downs = fg_dcvs_fails = fg_dcvs_floor_ns = 0; fg_dcvs_streak = 0;
 for (unsigned i = 0; i < 4; i++) gpu_extra[0][56+i] = 0xb007u + i;
}
int main(void) {
 uint64_t base, rearm; unsigned cap, hold;
 /* decode: flag, cap range, hold, stray bits */
 assert(fgd_decode(0x16, 8, &base, &cap, &hold) && base == 6 && cap == 6 && hold == 100);
 assert(fgd_decode(0x1815, 8, &base, &cap, &hold) && base == 5 && hold == 240);
 assert(fgd_decode(0x18, 8, &base, &cap, &hold) && cap == 8);
 assert(!fgd_decode(6, 8, &base, &cap, &hold) && base == 6 && !cap);
 assert(!fgd_decode(0, 8, &base, &cap, &hold) && base == 0);
 assert(!fgd_decode(0x11, 8, &base, &cap, &hold) && base == 0x11);
 assert(!fgd_decode(0x10, 8, &base, &cap, &hold) && base == 0x10);
 assert(!fgd_decode(0x19, 8, &base, &cap, &hold) && base == 0x19);
 assert(!fgd_decode(0x36, 8, &base, &cap, &hold) && base == 0x36);
 assert(!fgd_decode(0x10016, 8, &base, &cap, &hold) && base == 0x10016);
 assert(g_gpu_select(base) == 0 && g_gpu_select(6) == 6);
 /* pure schedule */
 assert(fgd_want(500, 450, 100, 6, &rearm) == 6 && rearm == 50);
 assert(fgd_want(550, 450, 100, 6, &rearm) == FGD_FLOOR && rearm == 0);
 assert(fgd_want(400, 450, 100, 6, &rearm) == 6 && rearm == 100);
 assert(fgd_want(900, 0, 100, 6, &rearm) == FGD_FLOOR);
 /* kick before start (level 0) never touches an unset call */
 reset(); fg_dcvs_kick(); assert(!fg_dcvs_call.pending);
 /* start -> idle hold -> floor vote, no timer left */
 reset(); fg_dcvs_start();
 assert(fg_dcvs_level == 6 && fg_dcvs_call.pending && fg_dcvs_call.at == 1000 + 100*NSEC_PER_MSEC);
 assert(step(&fg_dcvs_call, 1000 + 100*NSEC_PER_MSEC));
 assert(votes == 1 && last_level == FGD_FLOOR && last_bw == 1 && fg_dcvs_level == FGD_FLOOR && !fg_dcvs_call.pending);
 assert(fg_dcvs_downs == 1 && gpu_extra[0][56] == 0xb007u && gpu_extra[0][59] == 0xb00au); /* boot record kept */
 /* doorbell at the floor raises to the cap with the cap's bandwidth row, then re-arms */
 now_ticks += 50*NSEC_PER_MSEC; fg_dcvs_kick(); assert(fg_dcvs_call.pending);
 assert(step(&fg_dcvs_call, now_ticks));
 assert(votes == 2 && last_level == 6 && last_bw == g_gpu_bw_index(6) && fg_dcvs_level == 6 && fg_dcvs_ups == 1);
 assert(fg_dcvs_floor_ns == 50*NSEC_PER_MSEC && fg_dcvs_call.pending);
 /* doorbells at the cap enqueue nothing and defer the drop */
 uint64_t t0 = now_ticks;
 for (unsigned i = 1; i <= 5; i++) {now_ticks = t0 + i*60*NSEC_PER_MSEC; fg_dcvs_kick();}
 assert(step(&fg_dcvs_call, now_ticks + 10*NSEC_PER_MSEC) && votes == 2 && fg_dcvs_level == 6); /* timer fires within the hold: no vote */
 assert(fg_dcvs_call.pending);
 assert(step(&fg_dcvs_call, now_ticks + 200*NSEC_PER_MSEC) && votes == 3 && fg_dcvs_level == FGD_FLOOR && !fg_dcvs_call.pending);
 /* a failed down vote retries after one hold, keeps the cap meanwhile */
 reset(); fg_dcvs_start(); fail_votes = 1;
 assert(step(&fg_dcvs_call, 1000 + 100*NSEC_PER_MSEC) && votes == 1 && fg_dcvs_level == 6 && fg_dcvs_fails == 1 && fg_dcvs_call.pending);
 assert(step(&fg_dcvs_call, now_ticks + 100*NSEC_PER_MSEC) && votes == 2 && fg_dcvs_level == FGD_FLOOR);
 /* a failed up vote leaves the floor and retries on the next doorbell only */
 fail_votes = 1; fg_dcvs_kick(); assert(step(&fg_dcvs_call, now_ticks) && fg_dcvs_level == FGD_FLOOR && !fg_dcvs_call.pending);
 fg_dcvs_kick(); assert(step(&fg_dcvs_call, now_ticks) && fg_dcvs_level == 6 && fg_dcvs_streak == 0);
 /* eight failures in a row disable the governor and restore the cap */
 reset(); fg_dcvs_start(); fg_dcvs_level = FGD_FLOOR; fail_votes = 8; fg_dcvs_streak = 0;
 for (unsigned i = 0; i < 8; i++) {fg_dcvs_kick(); assert(step(&fg_dcvs_call, now_ticks));}
 assert(!fg_dcvs_on && fg_dcvs_level == 6 && votes == 9);
 fg_dcvs_kick(); assert(!fg_dcvs_call.pending);
 /* GPU not ready (failed): no vote, no crash */
 reset(); fg_dcvs_start(); ready = 0; step(&fg_dcvs_call, 1000 + 100*NSEC_PER_MSEC); assert(votes == 0);
 puts("PASS: DCVS decode, hold schedule, floor/cap votes with bandwidth rows, kick coalescing, failure retry/disable, boot record kept");
 return 0;
}
'''


def main():
    start = DRIVER.index('static thread_call_data_t fg_dcvs_call;')
    end = DRIVER.index('static void fg_thread(')
    body = DRIVER[start:end]
    # Both ring writers kick before the WPTR doorbell.
    assert re.search(r'fg_dcvs_kick\(\);\n if \(!fg_ring_doorbell\(\)\) err = 0x602;', DRIVER)
    assert re.search(r'fg_dcvs_kick\(\);\n(?: [^\n]*\n)? if \(!fg_ring_doorbell\(\) \|\| fg_access_failed\) return fg_abi_fault\("client doorbell"\);', BACKEND)
    assert 'fuxi_gpu_dcvs.h' in DRIVER and 'fg_dcvs_start();' in DRIVER and 'fgd_decode(requested_level' in DRIVER
    with tempfile.TemporaryDirectory() as d:
        path = Path(d)/'dcvs.c'
        path.write_text(PREFIX+body+CASES)
        exe = Path(d)/'dcvs'
        subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g',
                        '-fsanitize=address,undefined','-Wno-unused-function','-Wno-unused-variable','-Wno-format',
                        '-I'+str(ROOT/'shim/fuxi-probe'),'-I'+str(ROOT/'xnu/fuxi-adreno'),
                        str(path),'-o',str(exe)],check=True)
        run = subprocess.run([str(exe)],text=True,capture_output=True)
        assert run.returncode == 0, run.stderr[-1500:]
        out = run.stdout
        assert 'PASS:' in out, out
        print(out.strip().splitlines()[-1])
        # The policy header alone must also build freestanding for the kernel targets.
        path.write_text('typedef unsigned int uint32_t;\ntypedef unsigned long long uint64_t;\n'
                        '#include "fuxi_gpu_dcvs.h"\nunsigned t(uint64_t r){uint64_t b,w;unsigned c,h;'
                        'return fgd_decode(r,8,&b,&c,&h)?fgd_want(1,0,2,c,&w):0;}\n')
        for target in ('aarch64-none-elf','arm64-apple-darwin'):
            subprocess.run(['clang','--target='+target,'-ffreestanding','-O2','-Wall','-Wextra','-Werror',
                            '-I'+str(ROOT/'xnu/fuxi-adreno'),'-c',str(path),'-o',str(Path(d)/'o.o')],check=True)
        print('PASS: DCVS policy header compiles for AArch64 ELF and arm64 Darwin')


if __name__ == '__main__':
    main()
