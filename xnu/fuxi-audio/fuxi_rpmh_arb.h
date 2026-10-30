/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Serialises every user of the apps RSC (DRV2) ACTIVE TCS sequencer. One request is: program the TCS, trigger, wait for idle and
 * per-command status, clean up. Two drivers doing that at once (the audio DSP's and the modem/WLAN drivers' votes) corrupt each other's
 * cmd/enable/wait words, so the whole sequence runs under this one lock, the same cure as hackios's single RSC mutex (idea only, own
 * code). A failed or uncertain sequence leaves the TCS state unknown: leave(ok=0) poisons the arbiter for the session, as the WLAN
 * driver's rh_poisoned does. Freestanding: C11-style __atomic builtins, a bounded spin, no kernel lock or allocator. The caller owns the
 * policy of what to do on FUXI_RPMH_BUSY (retry later; never from a context that may hold the lock). Not wired into fuxi_wlan.c yet:
 * rh_send_request there still uses its unlocked rh_poisoned flag (docs-lanes/audio.md, P-AU1). */
#ifndef FUXI_RPMH_ARB_H
#define FUXI_RPMH_ARB_H
#include <stdint.h>

#define FUXI_RPMH_OWNER_WLAN 1u
#define FUXI_RPMH_OWNER_AUDIO 2u
#define FUXI_RPMH_OWNER_OTHER 3u
enum {FUXI_RPMH_OK = 0, FUXI_RPMH_BUSY = 1, FUXI_RPMH_POISONED = 2, FUXI_RPMH_RECURSIVE = 3, FUXI_RPMH_BAD_OWNER = 4};

typedef struct {
    volatile uint32_t lock_owner;            /* 0 free, else the owner id holding the sequencer */
    volatile uint32_t poisoned;
    volatile uint32_t holds, contended, timeouts, poisons;   /* counters for the log line */
} FuxiRpmhArb;

#if defined(__aarch64__)
#define FUXI_RPMH_RELAX() __asm__ volatile("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define FUXI_RPMH_RELAX() __asm__ volatile("pause" ::: "memory")
#else
#define FUXI_RPMH_RELAX() __asm__ volatile("" ::: "memory")
#endif

static inline void fuxi_rpmh_arb_init(FuxiRpmhArb *a)
{
    __atomic_store_n(&a->lock_owner, 0, __ATOMIC_RELAXED); __atomic_store_n(&a->poisoned, 0, __ATOMIC_RELAXED);
    a->holds = a->contended = a->timeouts = a->poisons = 0;
}

/* Takes the sequencer for owner (nonzero) with at most spins attempts. FUXI_RPMH_OK: held, call leave. Anything else: not held. */
static inline int fuxi_rpmh_arb_enter(FuxiRpmhArb *a, uint32_t owner, uint32_t spins)
{
    if (!owner) return FUXI_RPMH_BAD_OWNER;
    if (__atomic_load_n(&a->lock_owner, __ATOMIC_RELAXED) == owner) return FUXI_RPMH_RECURSIVE;
    int waited = 0;
    for (uint32_t i = 0; ; ++i) {
        uint32_t free = 0;
        if (__atomic_compare_exchange_n(&a->lock_owner, &free, owner, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (!waited) {waited = 1; __atomic_fetch_add(&a->contended, 1, __ATOMIC_RELAXED);}
        if (i >= spins) {__atomic_fetch_add(&a->timeouts, 1, __ATOMIC_RELAXED); return FUXI_RPMH_BUSY;}
        FUXI_RPMH_RELAX();
    }
    if (__atomic_load_n(&a->poisoned, __ATOMIC_RELAXED)) {__atomic_store_n(&a->lock_owner, 0, __ATOMIC_RELEASE); return FUXI_RPMH_POISONED;}
    __atomic_fetch_add(&a->holds, 1, __ATOMIC_RELAXED);
    return FUXI_RPMH_OK;
}

/* Ends a sequence. ok=0 means the TCS state is uncertain: later enters get FUXI_RPMH_POISONED. */
static inline int fuxi_rpmh_arb_leave(FuxiRpmhArb *a, uint32_t owner, int ok)
{
    if (!owner || __atomic_load_n(&a->lock_owner, __ATOMIC_RELAXED) != owner) return FUXI_RPMH_BAD_OWNER;
    if (!ok) {__atomic_store_n(&a->poisoned, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&a->poisons, 1, __ATOMIC_RELAXED);}
    __atomic_store_n(&a->lock_owner, 0, __ATOMIC_RELEASE);
    return FUXI_RPMH_OK;
}
#endif
