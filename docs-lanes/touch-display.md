# Touch + display lane (Fuxi, 2026-10-20)

Patch: `xnu/patches/0093-fuxi-touch-display-polish.patch` (on top of 0091; touches
only `bsd/dev/fuxi_touch.c` and `iokit/Kernel/arm/SweetFramebuffer.cpp`).
Built rc=0 in the Mac tree `ci-lane-touchdisp` (log `logs/kernel-lane-touchdisp.log`).
NOT booted yet.

## Findings (from code and the ios-0091d-01 log)

Observed facts:
- **The display was capped at 10 Hz.** `AppleCLCD::start` defaults `sweetfb_hz` to 10
  (a Sweet-era value from when one A55 core was too slow for the copy). The Fuxi
  cmdline does not set `sweetfb_hz`, so the panel copy ran every 6th 60 Hz tick.
  That adds 50 ms on average (up to 100 ms) between a rendered frame and the panel,
  and is the largest part of touch-to-photon latency.
- The vsync tick re-armed itself with `setTimeoutUS(16667)` *after* the copy, so the
  copy time made the period longer.
- backboardd never enables vsync notifications (no `notifications type` line,
  selector 72 is never called), so CA does not pace on our vsync. The tick matters only
  as the present clock.
- Swaps happen only when content changes: about 2000 swaps over the whole ios-0091d-01
  session (`SWEET FB: 1000/2000 swaps`). The rect is always full-screen
  (`a 0,0 1125x2436 b 0,0 1125x2436`), so it gives no dirty-rect information.
- The panel memory is mapped write-combined (`SweetSurface.cpp`, kIOMapWriteCombineCache);
  ABL autorefresh scans it out on every TE (patch 0087), so no START kick is needed.
- Display off (selector 12, patch 0079) fades out by read-modify-write of every
  pixel in the write-combined memory, 7 times (uncached reads on the workloop).
- Touch mapping is consistent: SweetHIDTouch maps panel (x+22, y+18) into
  1125x2436, the same centered crop the framebuffer copy uses (offsets 22/18). The
  edge flag (48 px, 0071) works: `touch 0 down at 632,2398 edge` → swipe-up worked.
- The touch driver polled every 8 ms (relative sleep, default timer coalescing
  leeway). It read the FIFO one 8-byte event per SPI transaction. It treated hover
  (type 5) as touching. It released fingers only after 10 consecutive errors and
  never recovered from errors. No FIFO errors were seen in ios-0091d-01.
- No brightness path: backboardd looks for a service with `backlight-control`
  (`IOK backboardd: matching ... backlight-control ... -> 0 found`).

Guesses (not measured):
- The FTS scan rate is probably 120-240 Hz, so 8 ms polling (125 Hz) may drop
  samples during fast swipes.
- The 1080x2400 copy into write-combined memory should take about 1-3 ms on the
  SM8550 big cores. Patch 0093 logs the real number.

## Changes in 0093, by expected impact

1. **Present up to 60 Hz on FUXI** (default; `sweetfb_hz=N` still overrides,
   `sweetfb_hz=10` gives the old behaviour for A/B). The tick now uses absolute
   deadlines with zero leeway. A stats line is printed after every 600 presented frames:
   `SWEET FB: stats N frames in T ms, copy avg A us max M us, S swaps, D not shown`
   (D = swaps replaced before they were shown).
2. **Touch polling 4 ms while active.** The poll is 4 ms while a finger is down or
   the IRQ was seen in the last 150 ms, 8 ms when idle and 32 ms while the display
   is off. Deadlines are absolute with critical urgency (no coalescing). The thread
   runs at priority 92. Boot-args: `fuxitouch_poll_us`, `fuxitouch_idle_us`.
3. **The FIFO is drained in 2 SPI reads** (one event, then the queued count from
   byte 7 in one read, as Lineage `fts_event_handler`) instead of one read per event.
4. **Display off:** a single black fill (write-only) instead of the uncached fade.
   Touches are released and ignored while the display is off (`sweet_fb_display_off`),
   so nothing can be tapped on a black screen; the power button wakes the phone.
   `fuxitouch_offsuppress=0` turns this off.
5. **Robustness:**
   - After 2 consecutive FIFO errors, any held finger is released.
   - After 3 errors, and then every 50, the GPII rings are re-initialised.
   - A controller-ready event (0x03) while sensing means the IC reset itself:
     fingers are released and sense-on is sent again.
   - Hover (type 5) counts as lifted, and invalid-type (0) events are ignored.
6. **Log trim:** the first-16 FIFO dumps and the every-50th report lines are gone. There
   is one summary per contact instead:
   `FUXI TOUCH: contact N: R reports in T ms (I us apart), P polls, FIFO read avg A us max M us`.

## Not done / next ideas

- **Interrupt-driven touch.** GPIO25 maps to PDC 57, and the TLMM summary IRQ is SPI
  208. XNU's GIC code (pe_fiq.c, SWEET) only handles PPIs/SGIs today, so this needs
  three pieces: GICD SPI setup, PDC/TLMM interrupt configuration (PDC may be
  secure-only) and an IRQ dispatch path. That is too risky without the device;
  4 ms polling gives most of the benefit.
- **Real panel off and brightness.** Both need DCS commands over DSI0: 0x28/0x10 for off, 0x51 for brightness.
  With INTF1 autorefresh on, a command-DMA transfer would have to be interleaved
  with autorefresh (disable AR, send, re-enable), and that was the cause of the static in
  ar-01. Read the Lineage panel driver (dsi-panel-m3 / mi_dsi) for the exact
  sequence before trying it. A software brightness option is to scale pixels during the
  copy (NEON). iOS would also need an IOService with `backlight-control`
  (AppleARMBacklight-like) for the slider to do anything.
- **Copy cost.** If the stats show copy >4 ms: copy only the rows that differ from
  the previous swap's surface (both are cacheable), or split the copy across 2 threads.
- **Cropped edges.** The centered crop hides 22 px left/right and 18 px top/bottom of iOS
  (the bottom of the home indicator). A scaled present would show all of it, but each
  pixel would cost more CPU and look softer.
- SweetHIDTouch originally sent one collection per changed finger. The opt-in
  report collection path below addresses this in the canonical H1 overlay.

## Artifacts (local, not in git)

- Kernel `xnu/out-lane-touchdisp/kernel.development.fuxi.unstripped` sha256 9741f309…
- Payload `shim/out/lane-touchdisp/fuxi-0093-payload.bin` sha256 538aa1db… (0091d cmdline, ADT fuxi-0090.adt)
- Boot image `shim/out/lane-touchdisp/fuxi-0093-boot.img` sha256 4917082d… (payload verified inside)
- Runner `research/run-ios-0093-touchdisp.py` (copy of run-ios-0091d.py with these paths; keeps `--touch-power`)

## Test plan

Run it the same way as 0091d: phone in OrangeFox with /tmp/f2fs_io, then
`sudo python3 research/run-ios-0093-touchdisp.py research/device/ios-0093td-01`.

Boot checks (log):
- `SWEET FB: present up to 60 Hz (every 1 ticks)`
- `FUXI TOUCH: starting in 30 s (poll 4000 us active, 8000 us idle)`, then
  `reset err=0 event 03`, `chip id ... 3640`, `sensing`.

On the phone (user):
1. Unlock with a swipe up. Scroll the home screen pages left and right, and open
   and scroll Settings for about 30 s. Expected: motion visibly smoother than 0091d.
   Proof: `SWEET FB: stats` lines. The frames/T ratio is the real fps. Copy avg should
   be well under 16 ms. "not shown" should be a small part of swaps; at 10 Hz most
   swaps would be dropped.
2. Drag one finger slowly, then quickly, for a few seconds. Proof:
   `FUXI TOUCH: contact` lines with "us apart" ≈ 4000-8000 (it was ≥8000 before), and
   FIFO read avg/max (cost of one poll).
3. Two-finger pinch on a photo/Settings or a two-finger tap. Check that nothing is
   left stuck down: every `SWEET HID: touch N down` has a matching `up`.
4. Press power (lock). Expected: screen goes black at once (no fade), `SWEET FB:
   display off`. Tap and swipe the black screen: nothing happens and no `SWEET HID:
   touch` lines appear. Press power again: `display on`, the lock screen comes back,
   and touch works.
5. Optional A/B in the same image is not possible (the cmdline is in the payload).
   For an A/B, rebuild the payload with `sweetfb_hz=10` or
   `fuxitouch_poll_us=8000 fuxitouch_offsuppress=0`.

Failure signs: `FUXI TOUCH: FIFO read error`, `GPI re-init`, `controller reset
event`, or a panic in AppleCLCD::vsyncFired / present. Fall back to the 0091d image.


## UniXNU NativeHidInput report fixes (2026-10-24)

Source: **UniXNU / [NativeHidInput](https://github.com/UniXNU/NativeHidInput)**,
`NativeHidInput.cpp`, audited from `28f9b46` through
[`ba03e5d147452b2166aab7435d219fa10877672b`](https://github.com/UniXNU/NativeHidInput/commit/ba03e5d147452b2166aab7435d219fa10877672b),
**BSD-2-Clause-Patent**, copyright 2026 NativeHidInput contributors.
The full notice is retained in `xnu/fuxi-h1/LICENSE.NativeHidInput` and installed
beside SweetHID by patch **0103-fuxi-kernel-h1.patch**. The supplied upstream
files and log were read only; no upstream code was executed or built.

Our canonical touch sources are `xnu/fuxi-h1/fuxi_touch.c` and
`xnu/fuxi-h1/SweetHID.cpp`: 0091 introduces FTS SPI/GPI, 0093 improves polling,
0097 adds input boost, and 0103 applies the current H1 sources. Patch 0058
provides the iOS 16.7 IOHIDEvent and event-service/user-client implementation.
SweetHID constructs events directly and queues them to backboardd; it does
not use UniXNU's IOHIDDevice descriptor or original IOHIDEventDriver parser.

| Upstream commit | Fuxi adaptation / disposition |
| --- | --- |
| `75efe1c5ee32b2da3ef8dc978adbdc683eef11a8` — lift by identifier | Already correct: FTS IDs index the active bitmap and coordinate arrays, not packed report slots. Retained and tested. The batch path compares the complete new bitmap to SweetHID's previous ID state and emits every vanished ID at its last reported position, even when the down count stays constant. |
| `e43c2541133e25bdb48df564a4283888b64cc231` — multi/lift counters | Adapted to the existing per-contact FUXI TOUCH summary, available with either dispatch mode. |
| `22f2dd454e8742f0639a737065f59ab42927a165` — confident fingers | The descriptor change does not apply. Both SweetHID paths explicitly use `kIOHIDDigitizerTransducerTypeFinger` for every child, including lifts. There is no Touch Valid/confidence event field in our 1915 API and no parser to reinterpret a missing descriptor bit as a palm. The root remains a Hand collection, as before; it is not a palm child. No guessed confidence flag is added. |
| `ba03e5d147452b2166aab7435d219fa10877672b` — one report collection | Adapted behind `fuxitbatch=1`: pass the full report from FTS to SweetHID once, then build one collection containing active IDs and disappeared-ID lifts with a common timestamp. `DigitizerCollectionDispatch` itself is unnecessary without IOHIDEventDriver. |

**Boot argument:** `fuxitbatch=1` enables report batching (12 bytes, or 13 with
a separating space). Absent/zero retains the old per-finger path. It uses
`PE_parse_boot_argn`, like the existing Fuxi options, and logs
`FUXI TOUCH: report collections enabled (fuxitbatch=1)` once when enabled.
No existing command line is lengthened automatically.

Every changed batch includes stationary held fingers, new/moving fingers and
lifts together, with at most ten children indexed by hardware ID. Identity,
range/touch/position masks and the lifetime edge flag remain per finger.
Coordinates keep the native or centered-crop mapping and panel clamps. The
root position is the mean of active fingers (or lifted fingers on final up),
with aggregate range, touch, button and event masks. Unchanged reports are
still suppressed. If collection/child allocation or append fails, no partial
set is dispatched and SweetHID state stays unchanged. A pending flag retries
the latest set at the end of normal polling iterations, including all-up when
the FIFO is empty. New input supersedes the pending set; no old down is replayed
after its lift. Retries do not increment the producer counters, spin, or change
polling periods. This acknowledges local event construction/dispatch only,
not backboardd queue delivery. There is no new allocation retry queue.

The previous Fuxi path already includes held fingers in each collection, so
UniXNU's claim that its old path could never pinch does **not** establish the
same failure here. Batching removes intermediate mixtures of old/new finger
positions and redundant collection dispatches. It is a correctness and event
coherence improvement to test, not a measured fix for rendering stalls.

The FTS FIFO is a stream of enter/motion/leave deltas, not a complete hardware
contact snapshot. Absence from one FIFO drain cannot mean a lift: a stationary
finger may have no event. No speculative timeout or missing-FIFO-ID lift is
added. Existing hover-as-up, explicit leaves, display-off releases, reset
releases, FIFO-error releases, wake-touch suppression and polling stay in
place. No new HackiOS change is needed for this NativeHidInput comparison;
H1's existing MIT tap-to-wake policy remains in use.

The summary now ends with `, multi M max P lifts L`:

- `multi`: report masks with at least two fingers down during this contact.
- `max`: greatest number down in a report during this contact.
- `lifts`: ID bits changing from down to up, including existing forced releases.

A contact runs from first down to last up, and these counters reset on the next
first down. These count producer reports/lifts offered to HID, not successful
queue delivery or UIKit acknowledgements. Existing `reports` can exceed actual
events because SweetHID suppresses unchanged positions. No per-motion log is
added. The lift counter does not distinguish ordinary leaves from recovery.

### Supplied boot-use95 log audit

Observed: 16 transitions (eight downs, eight matching ups), all ID 0; eight
completed contact summaries totaling 510 reports and 1,051 polls. Three
contacts began at an edge and retained the edge suffix on up. There are no
duplicate downs, unmatched ups or contacts left down at the end of the log.
There are no FIFO read errors, GPI re-initializations, controller resets or
touch-driver stops. Controller startup reports `reset err=0`, ready `03`,
chip ID `3640`, sensing and asserted IRQ.

Contact summaries show 6,002–8,889 us mean report spacing; substantial drags
are around 8.3–8.9 ms (roughly 120 Hz). FIFO read averages are 167–205 us, with
a largest reported maximum of 454 us. This is evidence of promptly consumed
single-finger input, not proof of touch-to-display latency. No palm type or
confidence payload is logged. There are no multi-finger contacts in this
capture, so it cannot establish pinch behavior, palm classification or prove
that every controller lift reached UIKit. It provides no evidence of a stuck
contact or dropped lift, and does not identify touch as the cause of lag.

### Host verification and hardware follow-up

```sh
python3 tools/sync-fuxi-h1.py
python3 tools/sync-fuxi-h1.py --check
python3 tools/test-fuxi-touch.py
python3 tools/test-fuxi-h1.py
```

The new host test compiles the actual `ft_poll`, `ft_report`, legacy `touch`,
batched `report` and C HID entry points under ASan/UBSan. SPI, allocation,
locking and event dispatch are stand-ins. It covers both dispatch modes,
simultaneous movement, low-ID removal with a high-ID survivor, same-count ID
replacement, all ten fingers and simultaneous lifts, last-position lifts,
aggregate/child masks and timestamps, Finger type on downs and ups, native and
cropped coordinates, clamps, lifetime edge flags, unchanged reports,
allocation/append failures and final-up retry, invalid/hover/leave FIFO events, two-read failures
and tap-to-wake suppression, plus 2,000 randomized contact sets. An empty FIFO
is tested to retain stationary fingers. The sync check replays every earlier relevant
patch with `git apply`, checks the canonical snapshot and verifies 0103 applies.

Both complete modified translation units also passed an arm64 Mach-O syntax
check against saved XNU 8796 kernel headers and component flags, with
`-mgeneral-regs-only -Werror`. As in the existing UniXNU host check, Apple-only
allocation-type builtins use syntax-check stand-ins and existing allocation
macro initializer warnings are suppressed. Only our production sources are
compiled; no scripts from the saved build tree are executed. Reproduce with:

```sh
python3 tools/test-fuxi-touch.py --xnu-buildtree "$XNU_BUILDTREE"
```

Regression results: **50 Linux host scripts/checks passed**, including the new
touch test, H1, UniXNU, perf, scheduler, idle, backlight, GPU/scanout/Metal,
system, Wi-Fi and WLAN suites. The existing M9i test was compile-only; backend,
M6, Vulkan, M9m/M9n/M9o and frame-trace controls used the system software ICD.
The staging test's `--policy-storage-only` checks also passed. Its full default
invocation requires the missing built `tools/out/FuxiMetal.bundle/Info.plist`;
the same missing-artifact failure was reproduced using the starting commit's
staging/test sources. No passing test regressed. Twenty-one other scripts
requiring external translator/Mesa/NativeMetal inputs, signed artifacts or
kernel/boot outputs were not run. No dependency source was modified.

No phone operation, kernel link or hardware test was performed. Before enabling
by default, compare identical boots with the argument absent and enabled:
exercise two-finger pinch and independent lifts in both ID orders, replace a
finger while another is held, move both fingers, try ten contacts, home-edge
swipes, lock with fingers held, and tap-to-wake. Expect balanced per-ID down/up
logs, `max` matching the held count, `multi` advancing and no stuck UIKit
contacts or lost edge gestures. Confirm on-device serialized event acceptance
and gesture behavior; host event stand-ins cannot establish either. Keep the
option off if it regresses. UI smoothness needs a separate frame-time A/B.
