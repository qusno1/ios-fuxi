# Kernel H1: AMFI queries, container paths and buttons

Implemented offline, 2026-10-23. No phone access, hosted build, CI dispatch or
push was performed. Device behavior below is a test plan, not a boot result.

## Patch and source ownership

`xnu/patches/0103-fuxi-kernel-h1.patch` is an overlay after 0101. It updates the
stand-ins introduced by 0035/0050, the HID service from 0058, both PMIC key
readers (0060/0092), and the battery SPMI reader from 0062. Earlier patches stay
unchanged. The 0097 input performance notification is preserved.

Canonical final files are in `xnu/fuxi-h1/`. After editing them, run:

```sh
python3 tools/sync-fuxi-h1.py
python3 tools/sync-fuxi-h1.py --check
python3 tools/test-fuxi-h1.py
```

The sync tool reconstructs only affected files by applying every earlier
relevant patch with `git apply`, generates 0102 from those files, and checks
that the overlay applies. This catches changes to earlier patch contexts as
well as drift between canonical source and the shipped patch. Headers are
installed next to their including sources; no new compilation unit is needed.

## AMFI ABI and dispatch

XNU's `security/mac_base.c::__mac_syscall` matches the requested policy name
literally and calls its `mpo_policy_syscall`. The previous policy was named
`SWEET AMFI` and had only a signature hook, so a query to `AMFI` got
`ENOPOLICY`. That is also the error retained for every unknown AMFI request.

H1 adds a second, minimal policy named `AMFI`, containing only the syscall
hook. Keeping the original signature/entitlement policy avoids duplicate vnode
hooks and preserves its registration, root-filesystem platform marking and XML
entitlement handling. With `fuxiamfi=0`, the second policy is not registered;
AMFI queries therefore have exactly their former missing-policy behavior.

The supplied HackiOS reference, `kern_minimac.c`, documents these iOS 16.x
libamfi calls and wire layouts. There were no libamfi request definitions in
this repository's headers or notes to corroborate the private selectors.
Apple's [dyld-1066.8 source](https://github.com/apple-oss-distributions/dyld/blob/dyld-1066.8/dyld/DyldProcessConfig.cpp)
provides the eight output feature bits for this generation and calls
`amfi_check_dyld_policy_self`; the private libamfi header is not published in
that release. The selectors/layouts below are confirmed against the supplied
reference, not a new disassembly or trace of the installed 20H392 userspace.

| Policy/request | Argument at user address | H1 response |
| --- | --- | --- |
| `AMFI`, `0x5a` (90), `amfi_check_dyld_policy_self` | 16 bytes: `uint64_t input_flags`, 64-bit user pointer to output flags | Copy in the structure; copy out a 64-bit `0xff` through its output pointer; return 0 |
| `AMFI`, `0x60` (96), `amfi_interface_query_bootarg_state` | User pointer directly to a 64-bit result | Copy out `0x86`; return 0 |
| `AMFI`, any other request | Not inspected | `ENOPOLICY`, as before |
| `Sandbox`, 4 | 40 bytes: signed 64-bit pid, 64-bit pidversion, user path pointer, 64-bit capacity, 64-bit check-version flag | Look up target process and copy out its assigned container home, including NUL |

`copyin`/`copyout` errors propagate. No request treats a user pointer as a kernel
pointer. AMFI logs each distinct signed request number once per boot with the
first calling process name, including unknown and negative numbers. A mutex
protects a sparse set so concurrent first calls do not duplicate logs; entries
remain until reboot. Unusual distinct request numbers allocate kernel memory;
this diagnostic facility, like the policies, is intended for this research
system rather than hostile multiuser workloads.

For dyld, bits 0 through 7 enable, respectively: `@` paths, path environment
variables, custom shared caches, fallback paths, print variables, failed library
insertion, library interposing, and embedded environment variables. Returning
`0xff` intentionally allows all eight, regardless of input restrictions.
**Security implication:** `DYLD_*` variables, including library insertion and
path overrides, become honored in stock processes subject to dyld's remaining
checks. This permits changing the code those processes load. It is acceptable
for this research device and is default ON, but can be disabled with
`fuxiamfi=0`. That switch restores these query behaviors; it does not restore
Apple's overall security model or undo the older permissive policies.

`0x60` is a boot-argument-state bitmask, not the dyld feature mask and not a
boolean enforcement result. `0x86` sets bits 1, 2 and 7. The HackiOS reference
reports that ExtensionKit accepts any of these bits (`state & 0x86`) to skip
signature verification of the extension-point table
`/System/Library/xpc/extensions.plist.sig` through the absent AMFI IOKit service.
Its reference explicitly does **not** identify which individual AMFI boot arg
maps to which bit. The available headers and primary source do not resolve
that mapping either; H1 does not invent names for those bits. It reports the
same combined non-enforcing state, without adding an IOKit AMFI service.

## Sandbox container semantics

An extension needs a meaningful container path, not merely a successful syscall
that leaves its buffer unchanged. The pid form and audit-token form of
libsystem_sandbox's container lookup use request 4; the audit-token form supplies
a pidversion and asks for validation to avoid answering for a reused pid.

RunningBoard/PlugInKit already receive the container from containermanagerd and
pass it to the launched process as `CFFIXED_USER_HOME`. H1 reads the target's
saved exec strings through a referenced task/map, using XNU's procargs VM-copy
approach. It skips the executable path and argv, then finds a nonempty absolute
`CFFIXED_USER_HOME` path. A caller's argv cannot supply the answer. This reports
the existing launcher-assigned home; it neither creates a directory nor
establishes sandbox confinement. The environment is mutable, so this is
compatibility metadata, not a trustworthy isolation boundary.

Missing processes or pidversion mismatches return `ESRCH`; absent/invalid homes
return `ENOENT`; short buffers return `ENAMETOOLONG` without a truncated write;
bad user pointers return the copy error. Missing/stale task maps and VM failures
are handled with balanced references and allocations. No global fake container
is returned. This deliberately keeps `ENOENT` for an uncontained process rather
than the reference's success-with-untouched-buffer behavior. `fuxiamfi=0` keeps
the previous unconditional `ENOENT` for request 4. Other Sandbox operations and
privilege handling retain their previous results.

## Buttons and the arbiter

Both Fuxi and legacy Sweet readers now use `fuxi_key_state.h`:

- A key is ignored until two consecutive valid released samples have armed it.
  Startup presses and their eventual release generate no HID pair.
- Every later press/release needs two equal valid samples, 20 ms apart. A failed
  read breaks the pair; it cannot bridge an invalid sample.
- Eight consecutive polling cycles with a failed active peripheral stop the
  polling thread. All previously delivered downs receive balancing releases.
  One stop line replaces periodic error spam. A fully successful cycle resets
  the failure count; an intentionally withheld peripheral does not count.
- A timed-out observer lane is not reused that boot: a late completion must not
  become a new key sample. Subsequent reads fail and lead to bounded shutdown.

Fuxi checks PON type `0x01`, subtype `0x09` (GEN3 HLOS), and GPIO type `0x10`,
subtype `0x10..0x13` before interpreting status/configuration. Unknown peripherals
are withheld, including from opt-in configuration writes. Sweet checks its PON
and GPIO types before interpreting its older register layout.

The arbiter audit found three in-kernel SPMI command issuers: Fuxi keys, Sweet
keys, and Sweet battery (`sweet_spmi_read`). All now acquire the same statically
initialized sleepable mutex, defined in `sweet_spmi.c` and exposed through
`sweet_spmi_bus.h`. It covers command/data writes, completion polling and result
reads on both v5 and v7 readers, including the optional GPIO write. The battery
reader's private IOLock has been removed. Static initialization avoids a race
between battery and keys startup. Loader-side hardware probes run outside XNU
and cannot share this kernel lock.

Every `SweetHIDButtons::key` event now passes `kIOHIDEventOptionIsBuiltIn` to the
keyboard-event constructor. This includes power, volume, home and test events,
with the existing consumer-page usages unchanged.

## Buttons lane P-BTN (2026-10-23, offline)

Written without device access, CI or push. Everything below the "Device test
plan" heading is a plan, not an observation.

### Press timestamps and the key arbiter (HackiOS false-Siri item)

Observation from HackiOS (`hackios-xnu-userland/userland/src/inject_sb.c`,
`lock_long_press()`): SpringBoard's side-button long-press recognizer
(`-[SBLockHardwareButton longPress:]`, i.e. Siri) is a SpringBoard timer started
when its main thread sees the press. Waking the display from inside that press
takes as long as the first lock-screen frame, which on a software renderer is
longer than the long-press interval, so the timer fires before the release is
processed. HackiOS saw this even for its tap-to-wake press, whose down and up
are sent back to back. Their fix drops a long press in SpringBoard when the
button is no longer held. A kernel cannot do that, and this project has no
SpringBoard code injection; that part is **not ported**.

What the kernel does instead, so the pair it delivers is right:

- **Timestamps.** `fuxi_key_sample()` records when the new level was first
  sampled; the HID event (`sweet_hid_key_at()`) carries that time rather than
  the time of the confirming sample 20 ms later. Timestamps handed to HID never
  go backwards.
- **One arbiter.** All Fuxi key events (PMIC poller, test injection,
  tap-to-wake) pass `fk_deliver()` and the `fuxi_key_arbitrate()` state in
  `fuxi_key_state.h`. A key is HID-down while any source holds it, so every HID
  down gets exactly one HID up; repeated downs and unmatched ups from a source
  produce nothing. The failure shutdown releases through the same path.
- **No stuck down across display off/on.** This system does not suspend; its
  "sleep" is the framebuffer `RequestPowerChange(0)` while the PMIC poller keeps
  running, so a press that turns the display off is released normally. An
  injected down is released automatically after 10 s.
- **Held state for a future userland guard.** `debug.fuxikeys.held` (read-only
  int): bits 0-2 = keys HID has down (power, volume up, volume down), bits 8-10
  held by the PMIC, 16-18 by injection, 24-26 by tap-to-wake. A SpringBoard
  guard like HackiOS's would read bit 0 when the long press begins.
- The up log line now says how long the key was held, as HackiOS does:
  `FUXI KEYS: power up (pon=0 gpio6=0x1) held 143 ms`.

Sweet (`sweet_keys.c`) only gets the new `fuxi_key_sample()` argument; it keeps
its dispatch-time stamps and its direct `sweet_hid_key()` calls.

### Test injection (`fuxikeyinject=1`)

`debug.fuxikeys.inject` is an int sysctl. Writes need root (the oid has no
`CTLFLAG_ANYBODY`; the handler checks `kauth_cred_issuser` again) **and** boot-arg
`fuxikeyinject=1`; otherwise a write returns `ENOTSUP` and changes nothing. A read
returns the keys currently held by injection. Injected events enter the same
arbiter and HID path as the PMIC keys (built-in, not marked injected), so iOS
cannot tell them apart.

Command word: `key | op << 8 | hold_ms << 16`, key 0 power, 1 volume up,
2 volume down; op 1 down, 2 up, 3 click (down, wait `hold_ms`, up; 0 means
100 ms, at most 2000). Bits 28-31 must be zero; a hold is only allowed with a
click. Anything else is `EINVAL`.

| Word | Action |
| --- | --- |
| `0x300` (768) | power click, 100 ms |
| `0x3e80300` (65536768) | power click, 1000 ms (long press: Siri expected) |
| `0x301` / `0x302` | volume up / volume down click |
| `0x100`, then `0x200` | power down, power up (any hold; auto-release at 10 s) |

Stock iOS has no `sysctl` command; a root test program calls
`sysctlbyname("debug.fuxikeys.inject", NULL, NULL, &word, sizeof(word))`.
Each injection logs `FUXI KEYS: inject <key> <down|up> -> hid <down|up|unchanged>`.

### Tap-to-wake (`fuxitaptowake=1`)

Ported from HackiOS `userland/src/inject.c` (MIT). backboardd sets
`TouchDetectionMode` on the touch service; HackiOS observed `0xff`/`0xfe` during
boot, `0x1` with the display on and `0x1fe` after the lock button, low byte
`0xfe` meaning "display off, a tap is to wake it". `SweetHIDTouch::setProperties`
stores that value (`sweet_hid_touch_mode()`, -1 until set) and logs each change
and the first 24 distinct property names the event system sets.

`fuxi_touch.c` already reads the FIFO while the display is off and drops the
events. With H1 it tracks those contacts; a new one calls
`fuxi_keys_touch_wake()`, which with `fuxitaptowake=1`, mode low byte `0xfe`,
power not held and no wake press in the last 1.5 s sends a power down and up at
the touch time (source TAP) and returns true. The contacts of that off period
then stay hidden from iOS until each lifts, so the waking finger does not start
a touch mid-contact once the display is on. Without a wake press nothing is
hidden (the previous behavior). Controller resets and repeated FIFO read errors
clear the filter. `fuxitouch_offsuppress=0` reports touches while the display is
off, so tap-to-wake then never triggers.

### HID usage audit

| Key | Page / usage sent | HackiOS (iOS 16.5, works) | iOS consumer |
| --- | --- | --- | --- |
| Side (power) | Consumer 0x0C / Power 0x30 | 0x0C / 0x30 | SpringBoard `SBLockHardwareButton` (lock, wake, Siri) |
| Volume up | 0x0C / Volume Increment 0xE9 | 0x0C / 0xE9 | volume HUD |
| Volume down | 0x0C / Volume Decrement 0xEA | 0x0C / 0xEA | volume HUD |
| Home (sweethidtest only) | 0x0C / Menu 0x40 | 0x0C / 0x40 | home button |
| Ringer switch | none (phone has none) | none | Telephony 0x0B not sent |

The service publishes the Consumer / Consumer Control (0x0C/0x01) usage pair and
`Built-In`, as HackiOS's virtual button service does, and every event carries
the built-in option. No mismatch was found, so no usage changed. This is
checked against HackiOS's working mapping and the IOHIDFamily-1915 usage tables
in patch 0058, not against a disassembly of the installed 20H392 backboardd.

### UniXNU comparison

`research/UniXNU/drivers/NativeHidInput` and `NativeKeyStoreEvents` are empty
submodule directories in the local checkout; nothing was downloaded. Its docs
only say NativeHidInput turns Novatek touch snapshots into digitizer reports. No
button, key-event or wake handling was available to compare, so nothing was
ported from UniXNU in this lane.

| Boot arg | Default | Effect |
| --- | --- | --- |
| `fuxiamfi=0` | H1 compatibility enabled (`1`) | Omit syscall-only AMFI policy and restore Sandbox request 4 to `ENOENT` |
| `fuxikeys=0` | Fuxi buttons enabled (`1`) | Existing Fuxi button driver disable |
| `fuxikeys_gpiocfg=1` | `0` | Existing opt-in GPIO6 input setup, only for a recognized peripheral owned by EE 0 |
| `sweetkeys=0` | Sweet buttons enabled (`1`) | Existing legacy button driver disable |
| `fuxikeyinject=1` | `0` (writes `ENOTSUP`) | Allow root writes to `debug.fuxikeys.inject` |
| `fuxitaptowake=1` | `0` | Touch while the display is off and `TouchDetectionMode` is 0xfe sends a power press |

## Host validation

`tools/test-fuxi-h1.py` compiles the actual production header, Sandbox dispatcher,
and both polling-thread functions natively with `-Wall -Wextra -Werror` and
ASan/UBSan. It tests AMFI layouts/results, unknown operations, copy faults,
disabled behavior, concurrent log-once calls, container argv rejection, missing
homes, pidversion validation, exact/short buffer bounds, malformed string
fuzzing, VM failure cleanup, held keys, release arming, bounce, failed samples,
failure reset, eight-failure shutdown and balancing HID releases. P-BTN adds
press timestamps (first sample of the run), a randomized arbiter pairing check,
timestamp monotonicity, command-word decoding, the tap-to-wake mode/rate policy
and contact filter, and compiles the real `fk_deliver`, inject/expiry, sysctl
handler and `fuxi_keys_touch_wake` functions against mocks: default `ENOTSUP`,
non-root `EPERM`, click hold timing, merging with a PMIC press, unmatched ups,
10 s expiry, tap-to-wake off by default, mode 0xfe only, 1.5 s rate limit and no
press while power is held. The test also
checks source integration and invokes the patch consistency check.

Validation results:

- H1 native tests and all four patch-source sync checks pass.
- 28 existing host scripts pass: the 26 runnable without external inputs
  (Metal staging in `--policy-storage-only` mode, M9i in `--compile-only` mode),
  plus `test-fuxi-vulkan.py --headers /usr/include` and
  `test-fuxi-metal-m6.py --headers /usr/include`.
- The additional `test-fuxi-metal-backend.py --headers /usr/include` fails in
  its existing proxy: it expects `TU_DEBUG=nocb` while the current setup defaults
  to `sysmem,nocb`. With `TU_DEBUG=nocb` supplied, normal submissions pass but
  its `BAD_FILL` injection still fails to trigger the expected error. The same
  failure reproduces using an untouched HEAD archive. No Metal/GPU code or
  tests were changed for H1.
- ARM64 Clang syntax checks pass for all five changed translation units against
  the available XNU 8796 build-tree headers. The C++ check preincludes
  `device/device_types.h` to supply a type omitted by that snapshot's include
  order; it is not a full linked kernel build.

The full Metal staging test needs unavailable signed artifacts; only its
storage-policy portion is claimed. Tests needing signed artifacts, external
translator/source checkouts or image/firmware inputs are not claimed as run.
A full linked XNU build and device verification remain outstanding.

## Device test plan (not executed)

1. Build the complete patch stack for DEVELOPMENT ARM64 FUXI. Keep the current
   root image and command line for an A/B comparison, adding only `fuxiamfi=0`
   for the control run. Do not change the hardware configuration for this test.
2. With H1 enabled expect the existing `SWEET AMFI` and `SWEET Sandbox` policy
   registration lines, followed by:

   ```text
   FUXI AMFI: query policy registered (0), dyld=0xff boot-state=0x86
   FUXI AMFI: <first process> request 0x5a
   FUXI AMFI: <first querying process> request 0x60
   SWEET Sandbox: <first process> op 4
   ```

   Only one AMFI request line per distinct number should appear. Confirm a
   harmless `DYLD_PRINT_*` diagnostic in a controlled launch is honored. Check
   that libamfi receives a 64-bit `0xff` for 0x5a and `0x86` for 0x60, and
   unknown selectors still produce `ENOPOLICY`. The control boot should print
   `FUXI AMFI: disabled (fuxiamfi=0)` and retain the earlier query failures.
3. Inspect extension registration and pkd logs. The hypothesis is that bypassing
   verification through the missing AMFI service lets ExtensionKit load its
   extension-point table, reducing "Unable to find AMFI", unknown extension
   point and invalid-container errors. Compare widgets and extension-based
   wallpaper registration. **Black wallpaper/widgets are a hypothesis to
   verify**, not an established AMFI root cause: rendering, assets, daemons or
   other missing services may still explain them. Record surviving errors.
4. Query a running contained app/extension by pid and audit token: the returned
   path should match its assigned `CFFIXED_USER_HOME`; verify a stale pidversion,
   uncontained daemon and too-small output buffer return the documented errors.
5. Check `FUXI KEYS` init logs: PON type `0x1`/subtype `0x9`, GPIO type `0x10`
   with subtype in `0x10..0x13`, enabled input and readable APIDs. Start with the
   default read-only GPIO behavior. Unknown types must withhold the key without
   trusting status or writing configuration.
6. Boot while holding each key in separate trials, including volume keys used
   to enter the boot mode. Until released there must be no HID down or repeat;
   the first release must not emit an unmatched up. After release, press each
   normally: one down and one up per press, power locks/wakes, volume shows the
   HUD. Check quick taps/bounce and simultaneous keys, and inspect the HID event
   built-in option in the event client. The ordinary logs remain:

   ```text
   FUXI KEYS: power down (pon=0x80 gpio6=0x1)
   FUXI KEYS: power up (pon=0 gpio6=0x1)
   ```

7. With a diagnostic kernel that injects read failures (never by altering PMIC
   power rails), verify eight consecutive failing active polls produce exactly
   one `FUXI KEYS: stopped after 8 consecutive read failures st=...` line, release
   outstanding HID downs and cease SPMI polling. Test recovery before the limit,
   invalid-sample debounce reset, and concurrent battery/key readers. Exercise
   the existing optional GPIO setup only in a separately planned opt-in run.

### P-BTN device test plan (not executed)

Build the stack (0103 now also carries `bsd/dev/fuxi_touch.c`) and boot with the
current command line first: nothing may change except the `held N ms` suffix,
the HID timestamps and, once backboardd sets them, the touch property lines:

```text
SWEET HID: touch property "TouchDetectionMode" set by the event system
SWEET HID: touch detection mode 0x1
```

1. Press each key; per press expect `FUXI KEYS: power down (...)` then
   `FUXI KEYS: power up (pon=0 gpio6=0x1) held <ms> ms` and one `SWEET HID: key 0x30
   down`/`up` pair. Lock the display with power and record the mode lines
   (expected low byte 0xfe while off, if Tap to Wake is on). Wake by power and
   note whether Siri appears and the `held` time; a short press that still
   opens Siri confirms the SpringBoard-side cause.
2. Boot with `fuxikeyinject=1`. As root write `0x300`: expect
   `FUXI KEYS: inject power down -> hid down`, `... up -> hid up`, one HID pair
   and the display toggling. `0x301`/`0x302` show the volume HUD. Write `0x100`
   and wait: after 10 s `FUXI KEYS: inject auto-release mask=0x1 after 10000 ms`.
   Without the boot-arg the write fails with `ENOTSUP`; as mobile, `EPERM`.
   Read `debug.fuxikeys.held` while holding a key (bit 0/8 for power).
3. Boot with `fuxitaptowake=1`. Lock, wait for the fade, tap the panel once:
   `FUXI KEYS: tap-to-wake power click (touch mode 0x1fe)` (or `0xfe`), the lock
   screen appears and the tap does not act as a touch. A quick double tap must
   not lock again. With Tap to Wake off in Settings (mode not 0xfe) a tap must do
   nothing. Holding a finger through the wake and lifting it later must not
   produce a touch.

## Reference credit

[HackiOS kernel](https://github.com/oosexgang/hackios-xnu-kernel), APSL 2.0:
`xnu/files/bsd/kern/kern_minimac.c` supplied the private AMFI selector/layout,
ExtensionKit mask and launcher-home container ideas;
`xnu/files/bsd/dev/redwood_buttons.c` supplied held-at-start, debounce,
peripheral-validation and bus-serialization ideas. H1 was written independently
in this repository's style; no HackiOS source files or verbatim code were pasted.
The device addresses and consumer HID mapping remain this project's existing
Fuxi implementation. No GPL source, firmware or Apple binaries were added.

P-BTN: [HackiOS userland](https://github.com/oosexgang/hackios-xnu-userland),
MIT, `userland/src/inject.c`: TouchDetectionMode values and the tap-to-wake
press with the waking touch withheld (ported into `fuxi_key_state.h`,
`fuxi_keys.c`, `fuxi_touch.c`, `SweetHID.cpp`, with the MIT notice), the
consumer usage map used for the audit; `userland/src/inject_sb.c`: the false-Siri
analysis (not ported, SpringBoard hook). HackiOS kernel `redwood_buttons.c`
(APSL 2.0): press start time and held duration.

### Scripted touch workload (`fuxitouchscript=S`)

A fixed gesture loop for GPU/UI timing without anyone at the phone. S seconds
after the key thread starts it runs `fuxitouchscriptn` loops (default 10). Each
loop wakes the display with an injected Power click if it is off, swipes up from
the bottom edge, swipes three home pages left and three right, opens Spotlight
and goes home, then opens Control Center and goes home. That is about 16 s per
loop. Touches use finger 0 and the normal `sweet_hid_touch` path, and they call
`fuxi_perf_input()` like real contacts. They are synthetic: test boots only.
The log marks `FUXI TOUCH SCRIPT: loop N start/done`.
