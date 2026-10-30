/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Idea after UniXNU-sheng package/GpuPowerProbe.cpp (BSD-2-Clause-Patent, GPU
 * governor: a level vote with that level's bandwidth, wake boost, idle floor).
 * This policy is our own and simpler: the boot level is a CAP, never exceeded.
 * Pure policy shared with offline tests; no kernel includes.
 *
 * fuxigpulevel=L|0x10|(H<<8): L 2..8 is the cap level (and boot vote), 0x10
 * turns the governor on, H is the idle hold in 10 ms (0 = 10, i.e. 100 ms).
 * A doorbell raises the vote to the cap; H after the last doorbell it drops to
 * FGD_FLOOR (220 MHz, 2.1 GB/s row). No polling while at the floor.
 */
#ifndef FUXI_GPU_DCVS_H
#define FUXI_GPU_DCVS_H
#define FGD_FLAG 0x10u
#define FGD_FLOOR 1u
#define FGD_HOLD_DEFAULT_10MS 10u
/* Returns 1 and sets cap and hold_ms when the governor is requested with a valid
 * cap; *base is the value the static level parser must see (flag and hold
 * stripped) either way, so a bad cap is rejected exactly like before. */
static int fgd_decode(uint64_t requested, unsigned max_level, uint64_t *base,
    unsigned *cap, unsigned *hold_ms)
{
 *base = requested;
 *cap = 0;
 *hold_ms = 0;
 if (requested > 0xffffu || !(requested & FGD_FLAG)) return 0;
 uint64_t level = requested & 0xfu, hold = (requested >> 8) & 0xffu;
 *base = level;
 if (level < 2 || level > max_level || (requested & 0xe0u)) { *base = requested; return 0; }
 *cap = (unsigned)level;
 *hold_ms = (hold ? (unsigned)hold : FGD_HOLD_DEFAULT_10MS) * 10u;
 return 1;
}
/* Level to vote now. `last` is the newest doorbell time, `hold` the idle hold,
 * both in the same tick unit as `now`; last 0 means never. *rearm is the remaining hold (0 when
 * the floor is wanted, so no timer needs to run). A clock that stepped back
 * counts as active. */
static unsigned fgd_want(uint64_t now, uint64_t last, uint64_t hold, unsigned cap,
    uint64_t *rearm)
{
 *rearm = 0;
 if (last && now < last) { *rearm = hold; return cap; }
 if (last && now - last < hold) { *rearm = hold - (now - last); return cap; }
 return FGD_FLOOR;
}
#endif
