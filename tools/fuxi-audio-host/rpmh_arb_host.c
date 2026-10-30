// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the RSC sequencer arbiter (xnu/fuxi-audio/fuxi_rpmh_arb.h): single-thread semantics, then threads standing in for the
// audio DSP's and the modem's drivers hammering one modelled TCS with and without the arbiter.
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../xnu/fuxi-audio/fuxi_rpmh_arb.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The modelled TCS: a request writes cmd words, "triggers", then cleans up. Another request inside that window is the race. */
static struct {volatile int in_sequence; volatile unsigned cmd, overlaps, sequences;} tcs;
static FuxiRpmhArb arb;
static int use_arb;
enum {THREADS = 4, ROUNDS = 3000};

static void sequence(unsigned id)
{
    if (tcs.in_sequence) tcs.overlaps++;
    tcs.in_sequence = 1; tcs.cmd = id;
    sched_yield();                                   /* the trigger and the wait for idle */
    if (tcs.cmd != id) tcs.overlaps++;               /* someone reprogrammed the TCS under us */
    tcs.in_sequence = 0; tcs.sequences++;
}

static void *worker(void *arg)
{
    const unsigned id = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < ROUNDS; ++i) {
        if (!use_arb) {sequence(id); continue;}
        /* Each thread is its own owner id (an owner re-entering is refused as recursive). */
        int r;
        while ((r = fuxi_rpmh_arb_enter(&arb, 10 + id, 1000000)) == FUXI_RPMH_BUSY) {}
        CHECK(r == FUXI_RPMH_OK);
        sequence(id);
        CHECK(fuxi_rpmh_arb_leave(&arb, 10 + id, 1) == FUXI_RPMH_OK);
    }
    return 0;
}

static unsigned run(int with_arb)
{
    pthread_t t[THREADS];
    use_arb = with_arb; tcs.overlaps = tcs.sequences = 0; tcs.in_sequence = 0;
    fuxi_rpmh_arb_init(&arb);
    for (uintptr_t i = 0; i < THREADS; ++i) pthread_create(&t[i], 0, worker, (void *)i);
    for (int i = 0; i < THREADS; ++i) pthread_join(t[i], 0);
    return tcs.overlaps;
}

int main(void)
{
    FuxiRpmhArb a;
    fuxi_rpmh_arb_init(&a);
    CHECK(fuxi_rpmh_arb_enter(&a, 0, 10) == FUXI_RPMH_BAD_OWNER && fuxi_rpmh_arb_leave(&a, FUXI_RPMH_OWNER_WLAN, 1) == FUXI_RPMH_BAD_OWNER);
    CHECK(fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_WLAN, 10) == FUXI_RPMH_OK && a.lock_owner == FUXI_RPMH_OWNER_WLAN && a.holds == 1);
    CHECK(fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_WLAN, 10) == FUXI_RPMH_RECURSIVE);           /* would deadlock on itself */
    CHECK(fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_AUDIO, 5) == FUXI_RPMH_BUSY && a.timeouts == 1 && a.contended == 1);
    CHECK(fuxi_rpmh_arb_leave(&a, FUXI_RPMH_OWNER_AUDIO, 1) == FUXI_RPMH_BAD_OWNER && a.lock_owner == FUXI_RPMH_OWNER_WLAN);   /* not the holder */
    CHECK(fuxi_rpmh_arb_leave(&a, FUXI_RPMH_OWNER_WLAN, 1) == FUXI_RPMH_OK && a.lock_owner == 0 && !a.poisoned);
    CHECK(fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_AUDIO, 0) == FUXI_RPMH_OK);                  /* spins = 0 still takes a free lock */
    CHECK(fuxi_rpmh_arb_leave(&a, FUXI_RPMH_OWNER_AUDIO, 0) == FUXI_RPMH_OK && a.poisoned == 1 && a.poisons == 1 && a.lock_owner == 0);
    CHECK(fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_WLAN, 10) == FUXI_RPMH_POISONED && a.lock_owner == 0);   /* poisoned: refused and not held */
    fuxi_rpmh_arb_init(&a);
    CHECK(!a.poisoned && fuxi_rpmh_arb_enter(&a, FUXI_RPMH_OWNER_OTHER, 10) == FUXI_RPMH_OK);

    const unsigned raced = run(0);       /* no arbiter: informational, scheduling dependent */
    const unsigned guarded = run(1);
    CHECK(guarded == 0 && tcs.sequences == THREADS * ROUNDS && arb.holds == THREADS * ROUNDS && arb.lock_owner == 0 && !arb.poisoned);
    printf("PASS rpmh arbiter: %u checks (unguarded overlaps seen %u, guarded 0, %u contended entries)\n", checks, raced, arb.contended);
    return 0;
}
