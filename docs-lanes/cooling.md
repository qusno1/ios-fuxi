# Cooling lane: idle heat (P-COOL)

The phone runs warm while iOS sits idle on the home screen. Earlier settled
seconds showed (observed, NOTES): launchd at about 657-969 ms of CPU per
second, mediaserverd about 510 ms/s, 2-4k IPIs per second per CPU, a timer
interrupt storm, and CPU sensors at about 49-55 C with `fuxiperf=1` capping
the clusters. Most of that is work iOS starts on its own: 334 of the 530
daemons in the 16.7 launchd cache start by themselves, and some of them keep
retrying hardware we do not have.

This lane ports the parts of HackiOS (which boots iOS 16.5 on a Snapdragon
778G phone) that cut that work, and adds a per-CPU load log so the effect can
be measured. **Nothing here changes the current boot by default**: the
launchd change is a root-edit tool you run, the kernel parts are boot-args,
and the Metal change only tightens a gate that already returned no GPU.

| Piece | Where | How to turn it on |
| --- | --- | --- |
| Daemons on demand | `tools/launchd-on-demand.py` | run on the mounted root (undo: `--undo`) |
| mediaserverd / SpringBoard never get Metal | `tools/fuxi-metal/ClientGate.hpp` | always, after FuxiMetal is restaged |
| Per-CPU idle log | patch 0105, `osfmk/arm64/fuxi_idle.c` | `fuxicpuload=N` (seconds, 1..3600) |
| Refuse system sleep | patch 0105, `IOPMrootDomain.cpp` | `fuxinosleep=1` |
| xpcproxy keeps its pid version | patch 0105, `kern_exec.c` | `fuxixpcid=1` |
| No L1 flush before each WFI | patch 0105, `osfmk/arm64/cpu.c` | `fuxinoidleflush=1` |
| IPI kinds, idle flush cost, who wakes whom | patch 0105 hooks | with `fuxicpuload=N` |
| A/B summary of runs | `tools/fuxi-thermal-summary.py` | offline, on monitor output |

## 1. Daemons on demand (`launchd-on-demand.py`)

Ported from HackiOS `hackios-xnu-userland` `userland/mkjobs.py` (MIT), the
`--rest` pass. HackiOS writes its own job plists; we edit the compiled cache
`/System/Library/xpc/launchd.plist` in place, like `disable-launchd-jobs.py`.

For every `com.apple.*` job outside the keep list it removes the keys that
make launchd start a job by itself: `RunAtLoad`, `KeepAlive`, `StartInterval`,
`StartCalendarInterval`, `WatchPaths`, `QueueDirectories`, `StartOnMount`
and `LaunchEvents`. The job stays registered, so it still starts the moment a
program looks up one of its Mach services or connects to its socket; a job
with neither never starts (reported as such). The removed keys are saved in
the job under `_FuxiOnDemand`, so `--undo` restores the exact stock cache.
Disabled jobs and non-Apple jobs (`org.fuxi.*`) are never touched.

```sh
python3 -I tools/launchd-on-demand.py MNT --dry-run   # report only
python3 -I tools/launchd-on-demand.py MNT             # apply
python3 -I tools/launchd-on-demand.py MNT --undo      # back to stock
python3 -I tools/launchd-on-demand.py --list-keep     # keep list with reasons
```

`--keep LABEL` keeps one more job (and restores it if an earlier run stripped
it); `--drop LABEL` takes one off the keep list. Unknown labels stop the tool
before it writes anything.

**Keep list (27).** HackiOS's core list (`IOS_JOBS` in `userland/build.sh`)
plus wifid, labels checked against 16.7: notifyd, both cfprefsd daemons,
logd, logd_helper, syslogd, diagnosticd, distnoted, fseventsd, both
containermanagerd jobs, runningboardd, backboardd, SpringBoard,
mobile.usermanagerd, mobilegestalt.xpc, lsd, powerd, analyticsd,
iconservicesagent, pluginkit.pkd, wifid. Added for this port: configd (USB
Ethernet), UserEventAgent-System (it delivers the LaunchEvents of the kept
jobs), peakpowermanagerd (host of the `sweetexecswap` CA smoke exec), keybagd
(data-protection keybag at boot) and usb.networking.addNetworkInterface.
mediaserverd is deliberately **not** kept: we have no audio, and it is the
second-busiest process at idle. It still starts on demand when a client asks
for one of its services; `--keep com.apple.mediaserverd` brings it back.

On a scratch copy of the real 16.7 cache (observed, host only): `327 jobs on
demand (32 of them never start), 0 restored, 27 kept, 30 disabled left
alone`, and `--undo` gives a cache equal to the original.

## 2. No Metal for mediaserverd and SpringBoard

HackiOS logs show mediaserverd's camera pipeline (CMCapture, AppleCVAPhoto)
preloading shaders and aborting about 28 times a boot, and SpringBoard's Siri
orb prewarm aborting every 8.3 s, each abort followed by a relaunch. Our
FuxiMetal gate already gave both a GPU-less (nil) device by default.
`ClientGate.hpp` now hard-denies both names (`hiddenClient()`) before
`allow`/`census` from the clients file or a boot-arg is considered, and
`stage-fuxi-metal.py` refuses a clients file that names either. There is
nothing to switch on; it takes effect when FuxiMetal is rebuilt and
restaged. HackiOS's `mediamtl=1` (give mediaserverd Metal again) and
`siri_orb=0` hook are not ported: the first would undo the point, the second
is not needed because SpringBoard never gets a Metal device here.

## 3. Kernel patch 0105 (`xnu/patches/0105-fuxi-idle.patch`)

All parts compile only with `FUXI` and act only when their boot-arg
is set exactly as shown; without them the kernel behaves as before (the
patch adds lines only, the stock pid-version bump and idle flush stay in
place). Sources are kept in `xnu/fuxi-idle/`: the two new files plus
`hooks.diff` (the hunks for upstream files); `tools/sync-fuxi-idle.py`
writes the patch from them (`--check` compares, `--tree DIR` also dry-runs
it on a tree with patches up to 0104), `tools/test-fuxi-idle.py` tests it.
Remember the loader command line limit (under
608 bytes, see `build-fuxi-metal-images.sh`) when adding them.

**`fuxicpuload=N`** (from HackiOS `redwood_cpufreq.c` `cpuload=MS` and
`ml_cpu_idle_time()` in `redwood.patch`, APSL 2.0). A kernel thread wakes
every N seconds, with 100 ms timer leeway so it does not add wakeups of its
own, and prints one console line:

```
FUXI IDLE: cpuload every 10s over 8 CPUs (fuxicpuload=10)
FUXI IDLE: cpuload 10s up 600s avg 12% irq 817 ipi 1940 tmr 384 | c0 23% u5 s17 i812 p1934 t377 | c1 1% u0 s0 i5 p6 t7 | ...
```

`avg` is the mean busy share over CPUs; per CPU: busy % (wall time minus
recount idle time, so interrupt time counts as busy), `u`/`s` user and system
run share, then `i`/`p`/`t` interrupts, IPIs and timer interrupts per second
(cpu_stat counters). The header totals are per second over all CPUs. A CPU
not yet registered prints `-`. Use 10 or more: the line is about 400 bytes.
On FUXI the ADT has 7 CPUs (c0 = MPIDR 0x000, c1..c6 = 0x200..0x700);
cpu@100 runs the loader's USB monitor and is never in the list.

Three more lines follow each cpuload line (same period, rates per second):

```
FUXI IDLE: ipirx 10s | c0 n2900 a41 x0 d0 o0 g0 | c1 ... | c6 ...
FUXI IDLE: idleflush 10s on | c0 n3007 a1000 m3 | c1 ... | c6 ...
FUXI IDLE: ipiwake 10s imm 40 idle 9100 def 0 | launchd[1]>mediaserverd[93] 3100 | ... | rest 112
```

- `ipirx`, received IPIs by the `cpu_signal` bits the handler found:
  `n` none (the scheduler's SIGPnop idle kick), `a` AST, `x` cross-call,
  `d` decrementer, `o` other, `g` SGIs with an id other than
  `SWEET_IPI_SGI` (a subset of `n`; non-zero means someone else, e.g. the
  hypervisor, sends SGIs). Every SGI below 16 counts in the cpuload `p`.
- `idleflush`, WFI idle entries per CPU (`n`) and, while the flush is on,
  the average ns of `platform_cache_idle_enter()` before each WFI (`a`) and
  its share of the interval in per mille (`m`). That time is accounted as
  idle by recount, so the cpuload busy % does not show it.
- `ipiwake`, `sched_ipi_action()` results by type (immediate, idle,
  deferred) and the top four (waker task > woken task) pairs, where the
  waker is the task running on the CPU that asked for the IPI (in an
  interrupt this is whoever was interrupted) and `[-]` is no thread
  (rebalance). `rest` is every other pair plus those that did not fit the
  per-CPU 32-entry tables.

**`fuxinoidleflush=1`** (new, not from HackiOS). SWEET is not
`__ARM_COHERENT_IO__`, so `cpu_idle()` runs the non-coherent
`platform_cache_idle_enter()` before every WFI: `FlushPoU_Dcache`, a set/way
clean+invalidate of the whole L1 D-cache (with LoUIS = 0 the loop still does
level 0). Our idle is WFI only, with no PSCI CPU_SUSPEND, so the core keeps
its L1 and stays coherent with the others; the flush costs time on every
idle entry (thousands per second per CPU at idle) and refills after wake.
The arg skips it and logs `FUXI IDLE: idle L1 flush skipped (fuxinoidleflush=1)`.
That this lowers temperature is a **guess**; the `idleflush` line measures
the cost first.

**`fuxinosleep=1`** (from HackiOS `redwood.patch`, which sets
`gSleepDisabledFlag` by default and re-enables sleep with `syssleep=1`).
`checkSystemSleepAllowed()` returns `kPMConfigPreventSystemSleep`, so idle
and demand sleep are refused. Rationale from HackiOS: powerd asks for sleep
once the display is off, and the CPU sleep path then hangs or loops; we have
no sleep/wake path either. It uses its own flag because the device tree
`sleep-disabled` property overwrites `gSleepDisabledFlag` at boot complete.
Logs `FUXI IDLE: system sleep refused (fuxinosleep=1)` at boot and the 1st,
2nd, 4th, 8th ... refusal with its reason. Whether our boot ever reaches a
sleep attempt is not known yet; the refusal count answers that.

**`fuxixpcid=1`** (from HackiOS `redwood.patch` xpcproxy hunk). When the
process that execs is `xpcproxy` (launchd's spawn trampoline that execs the
real daemon in place), the exec keeps the pid version instead of taking a
new one, so the audit token launchd recorded for the spawn still matches the
daemon. HackiOS keeps this on by default; their reason is that launchd
otherwise can treat the daemon as a different instance. That this causes any
of our launchd CPU time is a **guess**; the A/B run below tests it. Logs the
first eight: `FUXI IDLE: xpcproxy pid P exec keeps pidversion V (fuxixpcid=1)`.

## CPU frequency idle votes (`fuxiperfidle=MS`)

**OBSERVED (reported device run, keys74-perf):** `fuxiperf=1` reached
`LUT cool 1228800/1401600/1363200 kHz high 1228800/2323200/2342400 kHz`
and stayed cool on the lock screen, with CPU sensors about 45–47 C and
average busy time 0–4%. Cool still votes roughly 1.2–1.4 GHz.

Add `fuxiperf=1 fuxiperfidle=5000` to select each domain's lowest eligible
EPSS LUT frequency after five seconds without input or swaps. The accepted
timeout is 1000–60000 ms; missing or invalid values leave the feature off,
including the existing log format and polling behavior. Idle is never
entered before the first swap: **OBSERVED** with idle allowed from governor
readiness, boot-time shader compiles ran at 307 MHz silver and backboardd's
first frame came at 14.0 s instead of 7.9 s. With the recorded
LUT fixture, idle selects silver/gold/prime at 307200/499200/595200 kHz;
the ready log reports the actual choices.

Optional `fuxiperfidlekhz=600000` chooses the lowest eligible LUT entry at
or above that floor per domain. Accepted values are 100000–3000000 kHz;
invalid values are ignored. The floor is clamped to each domain's cool
frequency, so idle never votes above cool. This argument alone enables
nothing. Input queues the existing worker and follows the boost path;
a swap wakes idle into cool, including when an older input is still within
the boost hold. Continuous animation swaps prevent idle entry. Thermal and
duty guards still veto high; thermal cuts retain an already lower idle
vote. MMIO stays in the worker, with the existing 100 ms quiet polling
period (the existing faster thermal guard polling also remains).

With idle enabled, each ten-second `FUXI PERF: stats` window includes
`idle N ms entries N` and `state idle` when selected. Transition logs cover
the first eight changes and every 64th thereafter. **GUESS:** lower idle
votes may reduce settled temperature; responsiveness and power savings
need a device A/B run. Compare the baseline with five-second idle, then
with the optional floor; check input wakeups, swap-only wakeups, idle
residency and CPU temperature. Host verification is
`python3 tools/test-fuxi-perf.py`; `python3 tools/sync-fuxi-perf.py --check`
checks the patch 0097 snapshots using the existing sync tooling's format.

## UniXNU idle and thermal code: nothing ported

`drivers/ArmCpu` is empty in our checkout; its test scripts only show CPU
halt parking in WFI with interrupts masked (quiesce), which XNU already does
in its idle loop. UniXNU-sheng's `CpuFreqProbe.cpp` (`sheng-cpufreq`) raises
EPSS to the highest level, the opposite of cooling, and our `fuxi-perf`
already owns EPSS votes. Its `sheng-disable-jobs` boot-arg turns off
identityservicesd, locationd, CommCenter and iconservicesagent; the first
three are already on demand after section 1, and iconservicesagent is kept
for the home screen. No deeper CPU idle states exist in either project.

## Thermal pressure to iOS

Adapted from **HackiOS**, [oosexgang/hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland),
commits `fcc3214` + `a79ff07`, `userland/src/thermald.c` and
`com.redwood.thermald.plist`, **MIT**, copyright 2026 OOSEX GANG. The notice
is retained in `tools/fuxi-thermald/LICENSE.HackiOS` and staged with the
daemon. The governor pressure policy and root-cache integration are Fuxi
adaptations. This update is scoped to the supplied diffs; no
upstream code was executed or built and no newer revisions were fetched.

**OBSERVED (source and host tests):** `fuxiperf=1 fuxitp=1` enables the
read-only integer sysctl `kern.fuxipressure`. `fuxitp` accepts exactly `1`;
missing/other values leave publication off. It requires the existing
governor; it does not start a second MMIO reader or change frequency votes,
sampling deadlines, thermal guards or duty limits. Reads return `ENOTSUP`
when disabled (including failed governor initialization), `EBUSY` before
the first sample, and a pressure value afterward. The existing serialized
worker updates it after applying the thermal guard. A sysctl read only
snapshots state under the governor lock.

No iOS 16.x SDK `OSThermalNotification.h` was available on this Linux host.
These are the **OSThermalPressureLevel wire values documented by the supplied
HackiOS reference**, rather than the four-value `NSProcessInfoThermalState`
enum. Their SDK declarations and consumer behavior remain unverified here:

| Level | Value | Fuxi entry and recovery (CPU sensor temperatures) |
| --- | --- | --- |
| nominal | 0 | Valid quorum, thermal guard recovered, at/below resume (default 58 °C) after a pressure event |
| moderate | 1 | Thermal guard blocked (including predictive/rise cuts), or at/above hot (default 62 °C); falls only when guard recovers and temperature is at/below resume |
| heavy | 2 | At/above limit (default 68 °C), or lost sensor quorum; a valid sample at/below limit minus 4 °C permits recovery |
| trapping | 3 | At/above 77 °C; valid temperature at/below 73 °C permits recovery |
| sleeping | 4 | Reserved, never emitted: no thermal platform sleep policy is implemented |

Higher levels take precedence and can be entered in one sample. Falling
levels can skip directly to the current safe level. The guard already
requires 500 ms of cool, slow-rising samples to recover. Lost quorum raises
pressure to at least heavy and retains trapping if already reached; stale
temperatures never relax it. Existing lowered hot/limit/resume boot args
also lower the corresponding pressure thresholds. The 77 °C classifier
adds no new frequency or hardware trip programming.

`tools/fuxi-thermald/thermald.c` polls once per second, registers with
`notify_register_check`, sets the level with `notify_set_state`, then posts
`com.apple.system.thermalpressurelevel`. It publishes the initial valid
level and subsequent changes only; a failed state update does not post,
and a failed update/post retries next poll. Read errors retain the last
published state; malformed sizes/levels are rejected. Early `EBUSY` waits
for readiness. An absent/disabled service exits once, with no launchd
respawn loop. `thermald show` (also `level`) prints the sysctl value/name
without registering or publishing a notification.

Build/sign on a Mac using the existing Xcode device-tool route:

```sh
bash tools/build-fuxi-thermald.sh
# Also included in the tools target of tools/ci/build.sh.
python3 tools/stage-fuxi-thermald.py MNT                     # inspect cache only
python3 tools/stage-fuxi-thermald.py MNT --enable --binary tools/out/thermald
python3 tools/stage-fuxi-thermald.py MNT --remove             # remove and restore
```

Staging is explicit opt-in. It validates the arm64 iOS 16 executable's
Mach-O/signature command layout using the Metal staging helper (actual
signature verification is performed by `codesign` during the Mac build),
installs `/usr/local/bin/thermald`, its MIT notice and
`/System/Library/LaunchDaemons/com.fuxi.thermald.plist`, and embeds that same
job in `/System/Library/xpc/launchd.plist`. Cache owner/mode and unrelated
jobs are preserved. Use a mounted writable root; this tool performs no
mounting or device access. Reboot with `launchd_unsecure_cache=1` and
`fuxiperf=1 fuxitp=1`. Pair staging with the boot arg: staging alone leaves
the bridge daemon exiting without publication. `fuxitp=1` adds nine bytes
including the separating space; keep the whole loader cmdline below
608 bytes. No default image cmdlines are extended.

**OBSERVED (available 16.7 root-cache snapshots):**
`com.apple.thermalmonitord` is present and enabled, running
`/usr/libexec/thermalmonitord`, with `KeepAlive={SuccessfulExit:false}`,
a profile-change notify launch event and Mach services
`com.apple.thermalmonitord.watchdog` and
`com.apple.thermalMonitor.siriEnabledNote`. It is outside the on-demand
keep list and is not explicitly disabled by our current job lists.
On-demand stripping can still leave it Mach-activated. HackiOS's existing
deny list also names thermalmonitord.

**GUESS:** if Apple thermalmonitord runs, it could overwrite this shared
notification state using missing iPhone sensors. No live competing writes
were measured. To give this opt-in bridge one publisher, `--enable` uses
`disable-launchd-jobs.py`'s shared `disable_jobs` helper to disable only
matching thermalmonitord jobs if present. Their exact prior `Disabled`
keys are saved under `_FuxiThermaldDisabled`; `--remove` restores them.
Already-disabled jobs remain disabled, absent jobs require no change,
and repeated staging preserves the original saved state. Other cooling
on-demand edits are retained. Apply root changes before boot; changing the
cache does not stop a process already running. Removing the boot arg alone
does not undo the root-cache change; use `--remove` as well.

**GUESS (device validation pending):** iOS consumers should react to the
pressure notification, potentially lowering CA frame rate and application
work while hot. This communicates thermal constraints; it makes no claim
to improve frame rate. No phone, Mac cross-build, launchd execution, notify
permissions, ProcessInfo transition or CA response was tested in this lane.
On hardware, compare `thermald show` with governor temperatures/guard
recovery, check initial/change-only daemon logs, verify the Apple job stays
disabled, and observe app thermal state and frame pacing over heat/cool
cycles. Do not force heat to reach trapping for validation.

Host checks: `tools/test-fuxi-perf.py` compiles the actual worker, sysctl
handler and policy with ASan/UBSan; `tools/test-fuxi-thermald.py` compiles
the actual daemon against scripted sysctl/notify calls and tests staging
on synthetic caches. `tools/sync-fuxi-perf.py --check` verifies patch 0097
snapshots. Mac build/signing and hardware checks remain required.

**OBSERVED (Linux verification):** all 47 existing test entrypoints that
passed with default arguments before this change still pass, as does the
new daemon/staging test. The Metal staging policy-only checks also pass.
All 108 XNU patches apply in order to a fresh XNU source tree with locally
available provider sources, and the final governor files match the canonical
snapshots. Staging/removal on a copy of the available 530-job 16.7 cache
restores its decoded contents exactly, using an authored Mach-O layout model
(no executable/signing or device evidence).

The supplied `hackios-kernel.diff` was reviewed and **not ported**: its
1125×2048 ramfb mode addresses QEMU virt/bochs-VBE's height limit, while
Fuxi uses its physical display and its own loader. The 7-Zip ≥23 guard is
for HackiOS's `ios165/fetch.sh` APFS extraction path, which this project
does not use. Neither change affects our thermal governor or iOS pressure
publication. The userland diff's unrelated boot-store changes and thermal
workload probe were also outside this port's scope.

## Left out

- HackiOS's DENY list (jobs removed outright, not made on demand): on demand
  covers them and stays reversible with `--undo`.
- HackiOS's injected userland libraries and Metal/Mesa work: other lanes,
  not cooling.
- `mediamtl=1` and `siri_orb=0`: see section 2.
- `syssleep=1` as the default-on inversion: our default must not change, so
  the sleep refusal is opt-in (`fuxinosleep=1`) instead.

## Expected effect (guesses until measured)

Fewer daemons should mean fewer launchd spawn/exit cycles and fewer timer
and IPI wakeups, so launchd's share and the `irq`/`ipi`/`tmr` rates should
drop, followed by a few degrees on the CPU sensors once settled. Keeping
mediaserverd from starting should remove its ~510 ms/s, if nothing asks for
its services at idle (unknown). The size of each step is not known; measure
each separately.

## How to measure

Each boot with the monitor's thermal log, e.g. `run-ios-thermal.py OUT
--thermal-log 5` (writes `OUT/thermal.jsonl` and `OUT/boot.log`), left on the
home screen for at least 20 minutes without touching it. Then:

```sh
python3 tools/fuxi-thermal-summary.py OUT_A OUT_B OUT_C --settle 300
```

It skips the first 300 s after handoff and prints, per boot, the medians of
the hottest CPU sensor and of the silver, gold/prime, CPUSS, GPU and DDR
groups, median EPSS MHz per domain, the median `FUXI IDLE: cpuload` values
and counts of the cooling log markers, with the difference to the first
(baseline) boot. Room temperature and the case matter: run A/B back to back
and start each from a cool phone.

## Device test plan

Kernel with patch 0105 built by CI; FuxiMetal rebuilt and restaged; the
same ADT and command line as the current boot plus the args named per run.

1. **A, baseline:** current root, `fuxicpuload=10`. Expect the start line
   `FUXI IDLE: cpuload every 10s over 8 CPUs (fuxicpuload=10)` and one
   `FUXI IDLE: cpuload 10s up ...` line every ~10 s, each followed by the
   `ipirx`, `idleflush 10s on` and `ipiwake` lines; no other `FUXI IDLE`
   lines; boot otherwise identical to the current one.
2. **B, daemons on demand:** on the mounted root run the tool with
   `--dry-run`, then for real. Expect a summary line
   `327 jobs on demand (32 of them never start), 0 restored, 27 kept, 30 disabled left alone, ...`
   (counts may differ if the root changed). Boot with `fuxicpuload=10`.
   Expect SpringBoard on screen, USB Ethernet up, and no
   `FUXI MTL: gate allow mediaserverd`; if mediaserverd starts and its
   stderr reaches the log it prints `FUXI MTL: gate deny mediaserverd`.
   Compare A and B with the summary tool.
3. **C, xpcproxy identity:** B plus `fuxixpcid=1`. Expect up to eight
   `FUXI IDLE: xpcproxy pid ... exec keeps pidversion ... (fuxixpcid=1)` lines.
   Watch for launchd errors about job identity or daemons failing to check
   in; if any appear, drop the arg.
4. **D, no sleep:** C plus `fuxinosleep=1`. Expect
   `FUXI IDLE: system sleep refused (fuxinosleep=1)` at boot; let the display
   time out and count `... refused ... reason R options 0x... count N` lines.
5. If B breaks something, `--keep` the missing job or `--undo`, and record
   the job in the keep list with its reason.
6. **E, idle flush off:** the current command line plus `fuxinoidleflush=1`
   (with `fuxicpuload=10`), compared with a run without it. Expect
   `FUXI IDLE: idle L1 flush skipped (fuxinoidleflush=1)` once, then
   `FUXI IDLE: idleflush 10s off | c0 n...` lines; the baseline run prints
   `idleflush 10s on` with `a`/`m` values. A hang, random user crashes or
   data corruption would mean the flush mattered: drop the arg.

Host tests: `tools/test-launchd-on-demand.py`, `tools/test-fuxi-idle.py`,
`tools/test-fuxi-thermal-summary.py`, `tools/fuxi-metal/ClientGateTests.cpp`
and the policy checks in `tools/test-fuxi-metal-stage.py`.

## Thermal slope window (`fuxiperfrate=MS`, default 200)

OBSERVED: the CPU TSENS readings move in 0.4 C steps. The rise guard trips above
55 C at 100 dC/s or more. While the slope came from 20 ms windows, a single
sensor step (200 dC/s) latched `fp_blocked`. Then every compile-boost enter was
refused as "thermal" at 54.5-54.9 C, and the 500 ms recovery kept restarting.
GUESS: the same latch caused the touch-boost vetoes seen at 58-60 C. The slope now
uses a 200 ms window (a single step is 20 dC/s). Absolute hot/limit checks still
run on every sample. `fuxiperfrate` takes `fuxiperffast`..1000 ms.

## Recommended cores (`fuxicores=MASK`)

OBSERVED (use95, user interacting): average CPU load was only 6-13 %, and most of
it ran on logical CPUs 0-1 (Cortex-A510), while CoreAnimation produced 5-20 fps.
Frames took 5 ms in backboardd. The clutch scheduler is homogeneous here and has
no edge (AMP) policy. `fuxicores=0x7c` calls
`sched_perfcontrol_update_recommended_cores(0x7c)` once the governor is ready.
Normal threads then run only on logical 2-6 (gold and prime). The A510s keep
interrupts and bound threads. The mask must not set bits above 6 and must keep
at least two of logical 2-6, otherwise it is ignored. GUESS: UI latency drops;
heat and power impact are unmeasured.

### Falling hold (2026-10-25)

OBSERVED (first device run with `fuxitp=1`): thermald published 11 level changes
in the first 90 s of boot, during the shader-compile burst at 50-64 C. Some
were one log line apart: the rise latch toggled the level between nominal and
moderate. Rises still publish at once. A fall now waits until the level has
held for 10 s (`fp_pressure_hold`). GUESS: fewer notify wake-ups for iOS clients.

## UniXNU SM8550 comparison (2026-10-27, offline)

Read: UniXNU-sm8550-common (`dts/sm8550.dtsi`, `patches/xnu/*.patch`,
`configs`), ArmCpu, UniXNU `docs/porting/{platform,power}.md` and UniXNU-sheng
(`dts/sheng.dts`, `patches/xnu/zzye|zzyf`, `package/{CpuFreqProbe,GpuPowerProbe}.cpp`).
All BSD-2-Clause-Patent; nothing copied here, differences only.

**Core count.** Their tree boots all eight cores because their UEFI keeps no
monitor on any of them. Ours runs 7: the missing one is **MPIDR 0x100 (cpu1,
Cortex-A510)**, which hosts the loader's USB monitor (`tools/mkadt-fuxi.py`
drops `cpus.children[1]`; `SWEET_SMP_MAX_CPUS 7`). **cpu7, the X3 prime core
at MPIDR 0x700, does start**: J163 boot.log has `SWEET CPU: cpu=6 CPU_ON
target=0x700 ... PSCI CPU_ON rc=0 ... mpidr=0x700 came alive; SGI1 OK`, and
`FUXI PERF` reports prime at 1708 MHz under the load. The NOTES guess
"cpu7 is never started (cpus=7 -> cpu0-6)" is wrong: logical 6 is cpu7. Getting the
eighth core means retiring the USB monitor on cpu@100 first (GUESS: PSCI CPU_ON
on a core the loader still runs would fail or collide), so it is not enabled;
it is a loader change, not an ADT or PSCI one. Their PSCI path
is identical to ours (SMC, CPU_ON by MPIDR, GICv3 redistributor stride 0x40000).

**MPIDR to cpu number.** Their `start.s` patch widens the mask to
`0xFFFF` because SM8550 numbers cores in Aff1; ours maps via the ADT `reg`
(logical IDs in DT order, `SweetCPU`), which already works for 0x200..0x700.

**Clusters.** Their ADT tags A510 as `E` and cpu3..cpu7 as `P`, builds with
`__ARM_AMP__`, `MAX_CPU_CLUSTERS 2`, and runs the Edge scheduler with a patch
that keeps UI buckets on P and lets idle P cores steal E work. We keep one
homogeneous cluster (`cluster-type 0`) and the `fuxicores=0x7c` recommendation
mask, which keeps normal threads off the A510s. Equivalent intent; theirs also
lets the scheduler use E cores for background load and place by cluster
load. Not ported: it changes scheduler topology on an XNU we cannot build or
test offline, and we have no AMP perf controller either.

**Frequency governors.** Theirs (CPU: per-CPU idle time every 16 ms, 1.25
margin, 4-window step-down; touch holds P domains at top for 1 s; GPU frames
keep CPUs at 60 % of top for 300 ms) has **no thermal guard** and reaches
2.8/2.96 GHz. Ours (`fuxi-perf`) is event driven with the cool caps
(gold 1.4, prime 1.36 GHz), duty budget and the 62/68 C cuts; nothing in theirs
beats it for the stated goal, so the CPU policy is unchanged. The one idea kept
is on the GPU side, see `gpu-clock.md` (idle governor).

**Not comparable offline.** The SM8550 DTS in their repo carries CPU nodes, PSCI,
GIC, UFS, GPU/GMU/SMMU only: no OPP tables, interconnect, thermal zones or
reserved memory (those come from the pinned sm8550-mainline checkout, which is
not in the mirror). GPU registers match what we already use (`3d00000`,
`gmu 3d6a000`, `gpu_smmu 3da0000`, `adreno-43050a01`); the 220..680 MHz OPPs and
bandwidth rows were already ported in `gpu-clock.h`.
