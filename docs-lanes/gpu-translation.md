# GPU translation: first-use UI latency

2026-10-24. Original Fuxi frontend/backend and Fuxi C ABI adapter changes only.
The external metal2vulkan checkout remains clean at
`720ef1ce9ae2a2068999a8675717418b9416283e`; Mesa and NativeMetal are unchanged.
No upstream implementation is copied into this repository. The adapter remains
part of the separately built, dynamically loaded library, never statically
linked into FuxiMetal. No phone access, downloads, Apple inputs or hosted runs
were used for this work.

## OBSERVED: device log and existing code

The supplied `research/xl/use89.log` contains 90 backboardd `translation PASS`
records totaling **26,082.686 ms**, worst **1,637.933 ms**. Nine exceed one
second; 24 are below 50 ms. Interaction includes `hz=3.53 window_frames=35`.
GPU execution and the scanout copy are much smaller than the long translation
stalls. The same log also has two Vulkan pipeline timing outliers of 522.215
and 504.772 ms; Vulkan creation is usually small, but not uniformly 2–17 ms.
Those two outliers should be tracked separately during acceptance.

The boot already loads **33,385,261 bytes / 62 pipeline artifacts**. Later
snapshots contain about 32.9 MB / 49 artifacts. Thus this is a mixed warm/cold
session, and the 32 MiB byte limit is already relevant, well below the 256-entry
limit. The adapter's `translation cache MISS` describes its **eight-product
process cache**, not the persistent artifact cache. It cannot establish that
every miss retranslated AIR: an artifact load can satisfy it.

`Backend::compilePipeline` holds `Backend::mutex` across identity, translation,
product hashing and Vulkan creation. Submissions use the same mutex. Both
render-pipeline completion-handler selectors previously called the synchronous
selector inline. A compile can therefore block the caller and GPU submissions.

The original Fuxi adapter:

- Parses the metallib container at `open`; library/function handles retain the
  input. It does not reopen/reparse the metallib container on each pipeline.
- Constructs a new translator `Device` and a vertex/fragment descriptor for
  every product-cache miss, querying device descriptions each time.
- Calls `compile_render_variant` on two `StageSource::Function` values. The
  pinned library explicitly keeps no compiled-function cache. Shared vertex
  functions, and fragment functions reused with other render state, are
  specialized/lowered again for each uncached pair.
- With persistence enabled, first calls `render_variant_key` with temporary
  `StageEntries`, drops those specialized entries, then compiles again on an
  artifact miss. That repeats decode/specialization within one request.
- Formats all of the 40,360-byte `CompileOptions`, including unused constant
  slots, into a debug-text product key. Its scan reconstructs/copies that text
  for each of at most eight cached products. This is unnecessary bounded CPU
  and allocation work, not an unbounded growing pipeline list.

The backend object LRU is bounded at 32 entries / 16 MiB; adapter product LRU at
8 / 16 MiB of charged product payload; artifact LRU at 256 / 32 MiB. Their linear
scans are bounded. Product payload accounting does not represent total process
RSS (retained descriptors, source references and caller-owned pipelines also
exist). The inspected pinned library has no global growing compiled-stage list.
`StageEntries` has a linear specialized-entry list, but the old adapter creates
and discards it per key request; it cannot grow across this session.

## GUESS: why later translations take longer

The old log does not contain per-stage timings or per-request artifact outcomes.
It does **not** prove that equivalent work gets progressively slower. Later
requests can be more complex shader/constant variants, can lose persistent
artifacts through byte-cap eviction, and can retranslate shared stages. CPU
thermal/governor limits and scheduling on the smaller cores can increase the
wall cost of this repeated work. Memory pressure and allocation costs are also
possible. No claim of a translator leak or a proven thermal cause is made.

The new trace distinguishes these possibilities by recording full upstream
stage keys, artifact outcomes, stage-cache evictions, process CPU time, peak
RSS, and wall-time phases. Compare repeated *equal keys*, not just early/late
session averages. `process_cpu_ms` includes other threads of the process;
`peak_rss_kib` is a high-water mark, not current RSS or evidence of a leak.

## Settings (all absent/off by default)

| Boot argument | Equivalent process environment | Behavior |
| --- | --- | --- |
| `fuxigpustages=1` | `FUXI_MTL_STAGE_CACHE=1` | Reuse compiled functions, retain specialization within a request, and use compact product input keys. |
| `fuxigputrace=1` | `FUXI_MTL_TRANSLATION_TRACE=1` | Detailed translation timing and cache diagnostics. |
| `fuxigpuasync=1` | `FUXI_MTL_ASYNC_PIPELINES=1` | Queue the two render-pipeline completion-handler APIs on a serial worker. |

Only the exact value `1` enables an option. Explicit boot tokens override the
environment, including zero, malformed values and duplicates (all disable).
Settings are sampled once per process. Existing client admission,
`fuxibbgpu=`, `fuximetalbackend=` and `fuxigpucache=` policies still apply.
Use the Fuxi backend. Persistence remains independently controlled by
`fuxigpucache=1`; stage reuse also works with disk caching disabled/unavailable.

Rebuild FuxiMetal **and its original Fuxi adapter** with the existing
`tools/build-fuxi-metal-translator.sh` / `tools/ci/build.sh metal` route.
No metal2vulkan API or source changes are required. The new optional ABI-8
symbol is `fuxi_m2v_translation_scope_v1`; old ABI-8 bridges remain loadable,
log `bridge=0`, and cannot enable stage reuse/internal timing. Adapter source
changes invalidate the existing persistent snapshots through the existing
source digest; allow for that first boot when comparing results.

### Stage reuse and correctness boundaries

The pinned public APIs provide `render_variant_key`, `StageEntries`,
`compile_function_from_entries`, `CompiledFunction::{to_bytes,from_bytes_for_key}`
and `StageSource::Compiled`. Fuxi uses these APIs directly. It first obtains the
**actual** lowering inputs and upstream function keys for the complete pair and
variant, compiles only missing functions, then links their compiled artifacts.
Keys include constants, entry/AIR hash, device profile and lowering facts:
vertex-fetch projection, fragment attachment/varying facts and runtime variant
inputs. A function name alone is insufficient. The original function descriptor
stays in each product for later runtime variants.

The stage LRU stores **encoded artifacts only**, up to **128 entries / 16 MiB**
of artifact bytes plus bounded key/container overhead. Hits validate/decode the
artifact against its exact function key and device. Decoded/specialized AIR is
kept only within the current request. Eviction cannot invalidate active
products. Oversized artifacts are not retained; optional cache encoding failure
does not fail a successful compile. No additional disk cache, AIR input store,
cache-size increase, stage worker threads or upstream patches are introduced.

This removes repeated lowering and the second specialization on an artifact
miss. Computing the exact keys still decodes/specializes `Function` stages on
each new pair, including cache hits; this optimization does not eliminate all
first-use CPU work. Device queries, product hashing and linking remain costs.

### Asynchronous API behavior

Both `newRenderPipelineStateWithDescriptor:completionHandler:` selectors now
optionally copy the descriptor before returning and compile on one serial GCD
queue. The copied block retains the device, descriptor/functions/libraries and
handler. A worker autorelease pool encloses compilation and callback; the
new-method state ownership is released after callback. Callers can retain the
state/error they need beyond callback. Reflection/options failures use the same
NSError path as synchronous calls. The worker does not use the main queue.

Default inline behavior remains. Synchronous methods still return complete,
usable states or errors. No deferred placeholder or skipped-draw mode is added.
Asynchrony avoids doing work on the requesting thread, but **does not guarantee
that an existing frame can submit while compilation runs**: the backend mutex
and adapter product-cache mutex remain serialized. Clients that immediately
wait for completion still wait. No device result establishes whether CA uses
these asynchronous selectors often enough to help; stage reuse is the primary
optimization for synchronous CA requests.

## Log interpretation

- `translation settings stages=1 trace=1 bridge=1`: both adapter capabilities active.
- `translation request id=... vertex=... fragment=... objects=...`: correlate a
  request with its backend timing. The short ID is diagnostic only.
- `translation timing result=... wait_ms=... identity_ms=... compile_ms=... hash_ms=... create_ms=... total_ms=... process_cpu_ms=... peak_rss_kib=...`:
  caller-visible total and backend phases. Results include object-hit,
  product-hit, miss and fail; wait is time before acquiring the backend lock.
- `translation detail phase=... ms=...`: adapter wall timings for product-key,
  product-lock, product-lookup, device-descriptor, keys-specialize, artifact-read,
  vertex/fragment-cache-read, vertex/fragment-lower, stage-cache-write, pair-link,
  pair-compile, artifact-write, product-plan and library-open. The plan phase
  includes product extraction/validation. Detailed phases are subsets of
  `compile_ms`, not extra time to add to it. Library-open precedes pipeline work.
- `translation keys vertex=FunctionCompileKey(...) fragment=...`: full stage keys.
- `translation stage=vertex|fragment hit=... entries=... bytes=... evictions=...`:
  misses report retained capacity and cumulative evictions; hits avoid lowering.
- `translation artifact hit=... rejected=...`: distinguish a real persistent
  hit from the eight-product process cache. Correlate with existing cache-period
  counters and `Vulkan pipelines PASS` / `hz=` frame windows.

Timers describe wall time, including preemption. Trace output adds overhead;
use it for attribution, then repeat the final interaction comparison with trace
off. Async callers may finish their API call before worker timings appear.

## Host measurements and tests

Linux host, authored `metal-m7-control.ll`, external pinned translator, lavapipe,
ASan/UBSan on C++, no Apple libraries/shaders. The new
`TranslationTests.cpp` / `test-fuxi-metal-translation.py` makes 104 distinct
pipeline requests in fresh processes, A/B/B/A. The first 96 vary render state
while reusing functions; the last eight vary constants, vertex offsets and
strides. Every product hash (executable bytes, resources, specialization,
blobs/layout and render state) must match baseline. The runner also requires
stage hits, misses, and 104 backend misses: an object/pair cache hit cannot
masquerade as a stage-cache win. Timing thresholds are not test assertions.

Optimized host-translator results (fresh-process A/B/B/A after the other lane
checks finished, host milliseconds):

| Path | Backend request totals, runs 1 / 2 | Sum of backend `compile_ms` | Worst backend request, runs 1 / 2 |
| --- | --- | --- | --- |
| Default | 2,978 / 2,890 | 1,237 / 1,189 | 54.443 / 56.700 |
| Stage reuse | 2,257 / 2,324 | 479 / 502 | 43.723 / 42.131 |

Each optimized run recorded **201 stage hits / 7 misses**, across 208 stage
requests. Translation time fell about **60%**, backend request time about
**22%**. Pair compile alone totaled 666 / 645 ms in the reference; exact-key
specialization plus remaining stage lowering and linking totaled 261 / 269 ms
with reuse. Product-key plus product-lookup fell from 385 / 374 ms to 6 / 6 ms.
Total harness time was 3,299 / 3,202 ms versus 2,578 / 2,654 ms (about 20% less);
it includes the test's second product-hash calculation, outside backend timing.
Separate phases must not be summed into `compile_ms` a second time. Earlier
runs under concurrent host checks had overlapping worst-request ranges: this
small fixture does not establish a reliable phone worst-stall improvement.
An earlier debug build also passed the exact-hash comparison and showed lower
compile cost. No phone FPS or millisecond device target is inferred.

Commands, using already available external inputs (no downloads):

```sh
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-cache.py --source "$M2V_SOURCE" --build "$BRIDGE_BUILD"
python3 tools/test-fuxi-metal-translation.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-cache.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9g.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9h.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9k.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9l.py --translator "$HOST_TRANSLATOR"
FUXI_MTL_STAGE_CACHE=1 python3 tools/test-fuxi-metal-m9o.py --translator "$HOST_TRANSLATOR"
bash tools/ci/build.sh metal --dry-run
```

The adapter's eight Rust unit tests include LRU recency, replacement, entry/byte
caps and oversize rejection. C++ policy tests cover exact/duplicate/malformed
boot tokens and environment precedence. Existing persistence tests exercise
fresh-process pixels, idle checkpoint, corruption, identity mismatch and
unavailable storage with stages off/on. Pixel/control suites cover constants,
MRT/framebuffer fetch, mip/format variants and topology; the backend loss,
timeout and corruption controls remain applicable.

`AsyncPipelineFrontendTests.mm` exercises the actual frontend with its CPU model:
a suspended worker proves return-before-work, then descriptor mutation and
input/device releases test snapshot/ownership. Success/error callbacks fire
once and retained states survive worker pool drainage. It is wired into the
frontend runner and the `metal` CI build. **Mac Foundation/Metal/iOS SDK builds
and Objective-C execution remain unverified on this Linux host**, including this
new async test. A full Mac `tools/ci/build.sh metal` run is required; only its
Linux dry-run was executed here.

## Device acceptance experiment (not executed here)

Use the same newly built adapter for both A and B so its source digest is equal.
Keep CPU/GPU/thermal settings, display, client admission and the interaction
sequence fixed. Record temperature/governor samples and 27 comparable touch
contacts. Use fresh process state and equivalent empty translation/driver cache
state for the cold A/B comparison; keep a separate warm-cache comparison. Do not
compare an old warm snapshot against a newly invalidated adapter snapshot.

1. A: `fuxigpucache=1 fuxigputrace=1`, stages and async explicitly zero.
2. B: same, plus `fuxigpustages=1`; keep async zero to isolate translation reuse.
3. Optional C: B plus `fuxigpuasync=1`; identify which pipeline API CA calls and
   whether callback users submit or immediately wait. Keep this result separate.
4. Repeat the same unlock/screen/gesture sequence, with the same measured
   interaction interval in each log. Record initial first-use and immediate
   repeat-screen behavior separately. Repeat A/B in reversed order to expose
   thermal/order effects, then repeat the chosen configuration with trace off.

For the **interaction interval only**, report the number and sum of
`translation PASS ... ms`, worst such stall, the sum/max of `compile_ms`, max
`total_ms` (includes lock/Vulkan/hash costs), per-stage hit/miss/eviction counts,
artifact hit/miss/rejection counts, and the `hz=... window_frames=...` windows.
Report Vulkan creation outliers, failure/dropped-draw counts and visual
correctness as well. Do not average boot-idle `hz` windows into interaction FPS;
if combining interaction windows, weight by each window's elapsed duration.

Expected effect: new pair/state combinations sharing function keys should avoid
repeated lowering, with lower translation totals and worst stalls, and higher
interaction Hz. Truly new functions still compile; key specialization may remain
expensive. If keys-specialize dominates or stage evictions thrash, the trace
identifies that next limit. If wall time rises for equal keys while process CPU
time stays flat, investigate scheduling/contention; if both rise, examine CPU
frequency and shader/cache phase cost. This work does not change the governor,
cluster scheduling or upstream compiler algorithms.

## Opt-in compile boost (2026-10-24)

### OBSERVED: supplied baseline and host checks

`research/cb/use89.log` records first-use interactive translation stalls around
0.1–1.6 seconds while the governor normally votes cool. In the cold-cache boot
`research/cb/stages90.log`, the 45 timing records sum to **15,333.829 ms
compile_ms + 19,304.510 ms create_ms = 34,638.339 ms**. Their caller-visible
`total_ms` sum is **35,156.845 ms**, and process CPU sums to **31,605.325 ms**.
These are sums of per-request intervals, not elapsed boot duration. Stage reuse
already hits in this log. Both translation and Turnip Vulkan creation matter.

The scheduler is homogeneous SMP. The device tree reserves physical CPU 1 for
the monitor: physical 0,2,3,4,5,6,7 become XNU logical 0,1,2,3,4,5,6. The new
helper uses `ml_get_cpu_number(physical << 8)`, rather than binding logical 7.
Host tests exercise the actual governor and scheduler helper with MMIO,
thread-reference and runqueue stubs. They establish policy and API sequencing,
not device migration, actual clocks or a performance improvement.

### Interface and settings

Enable **both `fuxiperf=1 fuxicompileboost=1`** in the kernel command line.
Absent/off leaves the compile interface unsupported (`ENOTSUP`) and FuxiMetal
makes no compile-lease calls. FuxiMetal accepts only the exact, unique
`fuxicompileboost=1` token in `kern.bootargs`; there is no environment override.
Keep the existing GPU/client admission settings. Rebuild the kernel and
FuxiMetal; the existing adapter/translator needs no change for this feature.

`sysctlbyname("kern.fuxicompileboost", ...)` takes an exact `int32_t` new value:
**1 enters, 0 leaves** for the calling thread only. An optional old buffer
receives the 24-byte version-1 `fuxi_compile_reply` from
`xnu/fuxi-perf/fuxi_compile_abi.h` (sampled high-vote nanoseconds, physical CPU,
state and nesting depth). This is an action endpoint, not a readable knob.
`CTLFLAG_ANYBODY` permits mobile to reach the handler; the handler independently
requires `proc_selfname()==backboardd` and a platform code signature, following
patch 0102's GPU gate. Root receives no bypass. No caller-selected CPU, thread,
task, frequency, duration or user-client connection can authorize another thread.

Admission samples TSENS synchronously through the serialized governor worker,
then chooses online prime CPU 7, otherwise an online gold CPU 6..3. A failed
EPSS domain is excluded. Existing bindings and realtime threads are refused.
Binding uses `thread_bind_internal` with the same scheduler locking as
`thread_bind`, with eligibility checked under that lock; `thread_block` then
forces migration outside the governor mutex. The vote raises only the selected
fast domain to its existing high LUT row (at most 2.4 GHz); silver is unchanged.
The normal touch vote can independently raise both fast domains.

The FuxiMetal RAII scope starts after an object-cache miss and covers the bridge
compile (including synchronous vertex/fragment stage compiles), product hashing
and Vulkan pipeline creation. It ends before object-cache insertion. Runtime
variants and lazy topology pipeline builds also have scopes; nested scopes share
one lease and exceptions release it. No lock wait before acquiring the backend
mutex is boosted. The existing translation timing fields remain comparable.

### Safety limits and lifetime

- Fresh valid CPU sensor quorum is mandatory. No entry above **60.0 C**; the
  existing predictive/rise guard, **62 C** soft cut, **68 C** maximum ceiling and
  recovery hysteresis still veto high votes. Lower configured limits still win.
- Compile and touch **share the existing rolling duty budget**, **200 ms per
  1000 ms by default** (`fuxiperfbudget` retains its existing 20..500 ms bounds).
  Compile boost cannot replenish it or extend a touch burst through swaps.
  Consequently a long compile usually loses its high vote and binding before
  completion. This feature deliberately does not enlarge the thermal budget.
- Every lease has a **2-second nonrenewable deadline**, at most **16 nested
  enters**, and one global owner. Nested enters cannot restart a cut lease;
  another thread receives `EBUSY`. Initial thermal/duty denial also returns `EBUSY`: XNU's `userland_sysctl()` retries a handler's `EAGAIN` forever, and the first device boot (EAGAIN) spun backboardd in the kernel with no frames.
- Leave at depth zero drops the vote synchronously and unbinds. Leave without
  a matching entry succeeds with `FC_NONE`. Copyout failure rolls back an enter.
  A thread reference prevents owner-address reuse; worker checks remove dead
  owners and release references. Timeout releases even a living hung caller.
- Thermal/duty cuts and offline/failed domains unbind immediately at the worker
  sample and retain a bounded reply until leave, death or the lease deadline.
  Queued threads are removed and reinserted under scheduler locks when unbound.
  Active high votes use the existing fast sampling period (20 ms default),
  shortened for remaining duty/deadline. Cleanup without high runs within the
  normal 100 ms period. These are timer deadlines, subject to scheduler latency,
  not hard realtime guarantees. Hardware thermal protection remains in control.

### Trace and acceptance experiment (device unverified)

With `fuxigputrace=1`, FuxiMetal adds:

```
FUXI MTL: compile boost phase=pipeline enter_errno=0 leave_errno=0 cpu=7 state=4 scope_ms=... voted_ms=...
```

`phase` is pipeline, variant or topology. `scope_ms` measures the compile scope,
including entry/migration overhead; `voted_ms` is the kernel's sampled high-vote
wall interval, **not measured frequency or thread CPU time**. A refused entry or
expired/discarded reply gives `voted_ms=-1` (unknown), never an invented full-scope
boost. `cpu=-1` means admission failed. Other trace phase times still describe
all work, including the portion after a cut. Compare them with the unboosted run;
a single boosted run contains no counterfactual baseline.

Kernel lines are `FUXI PERF: compile enter cpu=... cap_ms=2000 duty_ms=.../1000
state=...` and `FUXI PERF: compile end cpu=... reason=... voted_us=...`.
States/reasons: 0 none, 1 active, 2 leave, 3 thermal, 4 duty, 5 timeout, 6 dead,
7 offline/domain failure. Existing governor stats now include the union of
compile and touch high intervals. The kernel end line retains the reason/time
when a subsequent userspace leave can no longer retrieve the reply.

For device acceptance, use the **same newly built kernel, FuxiMetal and adapter**
in both runs, with stages and trace enabled and async explicitly disabled.
Keep GPU level, CPU/thermal settings, display, workload and cache state equal:

1. A: `fuxiperf=1 fuxicompileboost=0 fuxigpustages=1 fuxigputrace=1`.
2. B: identical except `fuxicompileboost=1`. Begin with equivalent cold empty
   translation and driver caches and a cooled device. Repeat in reversed order.
3. For cold boot, report pipeline counts, sums of `compile_ms + create_ms`,
   `total_ms`, process CPU, first-frame time, cache outcomes and scope/voted
   times. Compare against the **34,638.339 ms / 45** stages90 baseline as well
   as A; changes in shaders, cache hits or stage hits invalidate a direct ratio.
4. Repeat the use89 interaction sequence with comparable touch contacts; report
   median/p95/worst first-use `total_ms` and `wait_ms`, translation/Vulkan phases,
   stalls above 100/500/1000 ms, immediate repeat-screen behavior, frame Hz and
   failures. Separate admission refusals and duty/thermal-cut compiles.
5. Record hottest CPU TSENS, slope/cuts, time above the soft threshold, duty
   utilization and actual EPSS votes/clocks. Check selected physical core,
   migration, gold fallback and that leave/death/timeout clear binding and votes.
   At 58–60 C the existing guards may deny nearly every entry; explicitly report
   that result. Check visual correctness and run long enough to expose heating.

**GUESS:** moving CPU-bound compile work from silver to a fast core and its high
LUT row can shorten first-use stalls. The short shared duty budget and existing
thermal refusal can sharply limit that benefit; worker threads inside an
external compiler would not inherit a caller's binding. No device result yet
establishes migration, sandbox permission for mobile's sysctl, high clocks,
thermal safety under this workload, Turnip worker behavior or any speedup.
Full Mac kernel/FuxiMetal builds and native Objective-C tests remain unverified
on this Linux host; CI build routes were checked with `--dry-run` only.

Host validation for this change: 15 kernel/governor suites and 12 FuxiMetal
suites passed, including `tools/test-fuxi-perf.py` (which runs the scheduler
helper model), frontend policy/scope tests, software Vulkan backend, cache and
M9g–M9o controls. The authored-AIR translation test also verified 104 identical
pipeline product hashes when compile boost is requested against an unsupported
host kernel, with one refusal trace per pipeline and no redundant topology
scope. Patch snapshots match; the scheduler hunks apply in order through the
later idle hooks. Both modified kernel units pass an ARM64 syntax check against
cached XNU headers. These host checks do not replace the device experiment.

### Compile boost device results

- OBSERVED, first boot (refusals returned `EAGAIN`): no frames. XNU's
  `userland_sysctl()` retries `EAGAIN`, so backboardd spun in the kernel.
- OBSERVED, second boot (`EBUSY`): renders, first frame 7.3 s, but all 49
  enters were refused with `cpu=-1`. The governor was ready and below 60 C.
- GUESS: the compiling thread is CoreAnimation's realtime render thread, and
  `fuxi_compile_bind()` refuses realtime threads. Such a caller now gets an
  unbound lease (`cpu=-1`, `state=1`) that votes high on gold and prime without
  migrating the thread. Refusals log
  `FUXI PERF: compile refused reason=N` (1 busy, 2 not ready, 3 thermal,
  4 duty, 5 no online fast core).

## Frame time breakdown

### OBSERVED: supplied interaction and synchronous submission

The supplied `research/ft/use89.log` ends at 502 completed surface frames,
255 late frames (50.8%), and zero recorded failures. Interaction windows include
`hz=4.84 gpu_ms=4.180`, `hz=11.41 gpu_ms=5.516`, and
`hz=22.98 gpu_ms=6.650`. These are completion-window averages, not per-frame
percentiles. Low GPU execution time alone does not establish where the CPU
spent time. The user reports very laggy swiping.

`Frontend.mm` commits synchronously: it acquires surface writer leases, copies
no-copy buffer shadows, submits operation segments around process-local events,
then publishes Completed and delivers handlers. `Backend::submit` holds its
mutex through preparation, queue submission, fence completion and invalidation.
A pipeline compilation or cache checkpoint can contend for that same mutex.

The blocking/cost audit below describes the synchronous/default path at its
original audit snapshot; the later frame-overlap section covers the opt-in
worker path. All paths are conditional unless described as every nonempty submit. Vulkan
object creation and cache maintenance are synchronous CPU calls, not additional
explicit GPU completion waits.

| Location | Blocking wait or synchronous work |
| --- | --- |
| `tools/fuxi-metal/Backend.hpp:758` | Backend mutex, held through the fence wait; shared with resource creation, compilation and cache maintenance. |
| `tools/fuxi-metal/Backend.hpp:1100` | `vkQueueSubmit`; driver CPU work can itself block. No async worker is introduced. |
| `tools/fuxi-metal/Backend.hpp:1104` and `:1106` | The **only explicit Vulkan completion wait**: `vkWaitForFences`, once per nonempty operation segment, production timeout 2 seconds. Host-only macro can extend it. A Metal command with events may have several segments/waits. |
| `tools/fuxi-metal/Backend.hpp:1110` | Timestamp `vkGetQueryPoolResults` after the successful fence wait, without `VK_QUERY_RESULT_WAIT_BIT`; no extra wait requested. |
| `tools/fuxi-metal/Backend.hpp:896`–`:897`, `:1116`–`:1117` | Synchronous whole-allocation flush before queue submission and invalidate after completion for each noncoherent referenced buffer/mapped image. HOST_COHERENT skips these calls. No per-frame map for ordinary persistent resources. |
| `tools/fuxi-metal/Backend.hpp:882`–`:892`, `:322`–`:323` | Alias-copy snapshot and translator neutral buffers call `allocateUnlocked`: buffer creation, `vkAllocateMemory`, bind, `vkMapMemory`, CPU zeroing. Default: transient allocation each applicable submit, destroyed during cleanup. Opt-in scratch reuse covers only the bounded alias buffer. Neutral buffers remain transient. |
| `tools/fuxi-metal/Backend.hpp:900`–`:906` | Command pool/buffer, fence and timestamp query pool are **already reused**. Created on first use or after recovery; reset on subsequent submits, after the prior fence completed. No default per-frame command-buffer creation. |
| `tools/fuxi-metal/Backend.hpp:923`–`:939`, `:990` | Descriptor pool is **already reused**, geometrically grown for new type/set capacity, reset per submit. Descriptor sets are still allocated and updated per draw. |
| `tools/fuxi-metal/Backend.hpp:981`, `:1017`, `:1040` | Framebuffers and sampled image views have bounded caches. Cache misses/overflow create objects; partial empty passes create transient render-pass/framebuffer objects. |
| `tools/fuxi-metal/Backend.hpp:944`, `:1056`, `:1067`, `:1074`, `:1078` | GPU staging: sampled linear shadows refresh buffer-to-image; aliases snapshot image-to-buffer then buffer-to-image; explicit texture uploads/readbacks copy to/from buffers. These commands share the segment's single fence wait. |
| `tools/fuxi-metal/Backend.hpp:857`, `:858`, `:862`, `:735` | Submission can synchronously translate/create runtime pipeline variants and lazy topology pipelines. Not unconditional per-frame pipeline creation. |

There are **no `vkQueueWaitIdle` or `vkDeviceWaitIdle` calls** in this backend.
Resource creation outside submit can also allocate/map memory (buffer import,
image import and ordinary texture allocation); it is not an unconditional
per-submit operation. Frontend IOSurface locks (`Frontend.mm:1230`), shadow
`memcpy` (`Frontend.mm:1237`), and explicit process-local event waits
(`Frontend.mm:1242`, up to 2 seconds) are additional possible stalls.
`waitUntilCompleted` (`Frontend.mm:1280`) can wait for a configured completion
queue's handlers; that wait is outside the measured commit interval.

### Opt-in diagnostics and log format

`fuxigpuframetrace=1` enables the additional line from the existing five-second
backboardd reporter. Default is off, with no environment-variable fallback.
Both new options use `translationOption`: only one exact `=1` token enables;
`=0`, malformed values and duplicate occurrences disable, including duplicate
`=1` tokens. Whitespace tokenization matches the existing translation options.
No diagnostic thread, per-frame heap storage, or per-frame printing is added.

Example **format only**, not a device measurement:

```text
FUXI MTL: frame trace n=100 samples=100 wall=12.000/25.000/80.000 ms cpu_before_commit=4.000/9.000 shadow_copy=0.100/0.300 lease_lock=0.200/0.800 backend_submit=2.000/8.000 gpu_wait=4.000/7.000 gpu_exec=3.000/6.000 draws/frame=8.00 render_passes/frame=8.00 pipelines/frame=0.10 descriptors/frame=8.00 allocations/frame=0.20 cpu_hist=0:10,1:10,2:20,3:20,4:10,5:10,6:20,7:0,unknown:0 encode_counts_unknown=0 gpu_exec_unavailable=0
```

- `n`: successfully completed command buffers storing a defined IOSurface
  render target, matching the existing gate's frame definition. It is a
  **window** count; a command is not necessarily a display refresh. Failures,
  non-surface commands and native-backend commands have no frame-trace sample.
  The existing `FUXI MTL: backboardd frames=...` line is preserved.
- All slash pairs are p50/p90 in milliseconds; `wall` adds the maximum.
  Host phases use `steady_clock` elapsed time and include descheduling.
  `wall` runs from commit to successful synchronous completion, including
  lease release, before user handlers and gate persistence/reporting work.
- `cpu_before_commit`: command-buffer creation to commit, the elapsed CA encode
  interval. It includes scheduling gaps and CPU waits; it is not CPU usage.
  Compilation before command-buffer creation and intervals between commands
  are outside it.
- `shadow_copy`: the frontend resource scan and shadow `memcpy` loop.
  `lease_lock`: surface lease acquisition plus backend mutex acquisition over
  all operation segments. Surface unlock and post-fence retirement contribute
  to wall but are not included in this pair.
  Split (p50/p90 ms, not yet observed on device): `lease_front` is the frontend
  lease, `lease_back` the backend mutex/slot/admission part, `lease_excl_wait`
  the user-level exclusion wait inside `lease_front`, `lease_lock_write` and
  `lease_lock_read` the IOSurfaceLock time for written vs sampled-only surfaces,
  and `lease_surfaces/frame` the surface counts by role.
- `backend_submit`: after acquiring the backend mutex through return from
  `vkQueueSubmit`, including validation, flush, allocation, translation,
  recording and descriptor work. `gpu_wait`: time inside `vkWaitForFences`.
  Explicit Metal event waits and post-fence invalidation/cleanup contribute to
  wall, outside these pairs.
- `gpu_exec`: existing Vulkan top/bottom timestamps, converted with the device
  timestamp period and valid-bit mask, summed across segments. This is GPU
  elapsed time, **overlaps** the CPU fence wait, and must not be added to it.
  `gpu_exec_unavailable` counts frames without timestamp samples; their stored
  value is zero. It is not an engine utilization counter.
- Draw/pass counts are actual recorded Vulkan draws/render passes. A Metal
  encoder with many draws currently becomes many Vulkan passes; full-image
  clear-only operations can instead use `vkCmdClearColorImage` (zero passes).
  Count fields are window arithmetic means per completed surface command.
- `pipelines`, `descriptors`, `allocations`: successful
  `vkCreateGraphicsPipelines` pipeline objects, `vkAllocateDescriptorSets` sets
  (not descriptor writes/pool creation), and `vkAllocateMemory` objects.
  Thread-local counters cover creation-to-completion on the committing thread.
  They exclude work on other threads. Overlapping command-buffer lifetimes can
  attribute the same intervening thread activity to multiple commands. If
  creation and commit occur on different threads, only commit-time counts are
  available and `encode_counts_unknown` increments. These counts are diagnostic
  attribution, not process totals or host malloc counts.
- Percentiles retain the latest 1,024 samples per reporting window in a fixed
  array; `samples` exposes truncation. Maximum wall, means and CPU histogram
  cover **all** `n` samples. Reporting sorts a fixed scratch array, then resets
  window counters, including idle windows (`n=0`).

**CPU convention verified against visible XNU sources:**
`osfmk/arm64/machine_machdep.h:35`–`:43` specifies the logical CPU in the low
12 bits of **TPIDR_EL0**, mask `0xfff`. `libsyscall/os/tsd.h:70`–`:79`
implements `_os_cpu_number()` by reading that register. TPIDRRO_EL0 holds the
cthread pointer in those sources; using its low bits would produce a misleading
histogram. The instrumentation reads TPIDR_EL0 once at commit entry on arm64
Darwin; unsupported/host-test builds report unknown. This is a hint only:
threads can migrate immediately before/after the read. Logical 0–1 are A510,
2–5 A715/A710, 6 X3 in the supplied topology; the histogram neither pins threads
nor establishes residency throughout encode/commit.

### Opt-in conservative speedup and host evidence

`fuxigpupipelined=1` currently enables **alias-copy scratch reuse only**. It
retains one buffer per Backend, up to 1 MiB of requested payload, charged to
existing resource limits (driver alignment can add padding). Larger requests
use the original transient path. Growth destroys the quiescent smaller buffer
before allocation; clean close reclaims it. The existing mutex and fence wait
prove that previous GPU use has finished before reuse or growth. Every copy
still records the full required snapshot and existing barriers. Pre-submit
failure leaves reusable scratch; uncertain queue/fence ownership poisons the
backend and retains resources until process death as before.

This deliberately does not make commit asynchronous: status publication,
completion handlers, CPU readback, event ordering and IOSurface ownership still
follow the original synchronous contract. Retained scratch can consume quota
between alias copies. With the flag absent, invalid or duplicated, allocation
and completion behavior follow the default transient path.

`tools/test-fuxi-metal-frametrace.py` uses the production Backend on lavapipe
with ASan/UBSan. Twelve 128x128 overlapping same-image snapshots with changing
CPU input allocate **12 objects by default versus 1 with reuse**, and full-image
readback matches CPU snapshot references. Controls cover
growth, >1 MiB transient fallback, pre-submit failure/retry, empty submission,
partial empty render passes, scheduled callback after submission, clean close
accounting, and refusal after an injected uncertain queue attempt. Duplicate and
malformed pipeline flags select the default path. `FrameTraceTests.cpp` checks
percentiles, reset, overflow, full-window maximum, log fields, the CPU bit mask,
and nested/thread-local attribution. Traced M9o authored shader controls also
verify pipeline, descriptor, draw and pass counters with unchanged pixels.
Timing output is informational; no noisy wall-time threshold is a correctness
assertion.

Host validation passed 20 script suites: `test-fuxi-metal{,2,2g,2h}`, backend,
frontend (its Linux controls), M6, M9a/M9b, M9g/M9h/M9i/M9k/M9l/M9m/M9n/M9o,
cache, translation, and the new frametrace suite. M9a ran through M9b, including
eight offline Rust adapter unit tests and SPIR-V validation. Available C++
controls use ASan/UBSan. Native macOS frontend/defaults builds, real signed
artifact staging/certification, and Apple-library suites were not run here.

### GUESS / unverified

Running encode/commit on A510 cores, pipeline compilation, shared-mutex
contention, surface locking, shadow/staging copies, or CPU scheduling gaps could
explain low completion Hz despite short GPU execution. The supplied log does
not distinguish them, and this change makes no scheduler/affinity adjustment.
The new trace must be collected during equivalent interaction to rank them.
Alias reuse helps only commands using that path; its frequency and benefit to
backboardd are unmeasured. No asynchronous commit change is justified by the
host evidence alone. macOS Objective-C++ build, target register values, Turnip
performance, actual frame percentiles and perceived UI improvement remain
unverified; no device or hosted execution was performed.

## Translation cache hits and capacity

### OBSERVED: supplied device logs and code attribution

In `research/ac/use95.log` (interaction), 52 artifact hits total **3,986.742 ms
compile + 1,264.411 ms initial mutex wait + 389.900 ms product hash + 185.137 ms
Vulkan creation**. That is 76.668 ms of translation per hit, before those other
costs. The 27 artifact misses total **5,713.941 ms compile + 7,009.517 ms create**.
`boost94.log` is boot-only: its 45 artifact hits total 2,301.242 ms compile,
232.481 ms hash and 98.091 ms create. These are per-request sums, not elapsed
session times or an interaction FPS measurement. The old 32 MiB byte cap, rather
than its 256-entry count cap, explains the reported 48 surviving pipelines.

The supplied logs truncate Rust detail lines to `phase=` and cannot attribute
all 77 ms to individual functions. Reading the pinned code establishes:

- Even a persistent hit first called `render_variant_key`, with fresh
  `StageEntries`. This can decode/specialize AIR to derive exact stage keys.
  It is not merely hashing the persisted artifact's index key.
- `from_bytes_for_key` checked the upstream frame SHA-256, decoded every word
  through its generic codec and performed a linear SPIR-V header/instruction
  length scan. It did **not** run the compiler's owned verifier or spirv-val.
- Product construction rebuilt bounded reflection and binding plans, hashed
  constant blobs and copied SPIR-V/blob vectors already owned by the pipeline.
  `product_cache_key` also allocated strings while searching retained products.
- The separately reported product hash formatted the entire compile-options
  structure, including unused constant slots, and fed executable words to SHA
  one at a time. The pinned `sha2` recipe selects software SHA on ARM64 without
  its optional `asm` feature; this change needs no new dependency or feature.
- Rust held its global product-cache mutex across translation. Stage-cache
  deserialization held another global mutex. Backend held its device/queue
  mutex across all translation, hashing, Vulkan creation and cache maintenance.

The disk encoder stored each stage's SPIR-V and constant blob once, plus typed
metadata. AIR/library inputs and the Product's duplicate vectors were not disk
payloads. The pinned SPIR-V serializer does not emit the OpName/OpLine debug
instruction family. The authored two-stage host control had **11,544 bytes
SPIR-V, zero blob bytes and zero debug bytes**; its first old artifact occupied
14,279 bytes. The supplied logs contain no actual device artifact payload, so
these observations do not establish the byte breakdown of a 700 KB CA artifact.
New trace lines report `translation sizes stored=... spirv=... blobs=... debug=...`
to make that breakdown measurable without logging executable bytes.

### Implemented behavior and integrity

These changes are default-on within the existing persistent-cache setting
`fuxigpucache=1`. Stage reuse retains its existing opt-in policy. No shader
instruction, constant blob, reflection field or bind requirement is removed.

A baseline persistent lookup now uses the full Fuxi input identity (library
content hashes, function names, typed constants and all bounded options) plus
upstream device-profile fingerprint. A hit skips AIR key specialization and the
stage cache. Runtime variants still derive their upstream exact keys. Product
lookups compare a precomputed input key; immutable executable/blob allocations
are shared with `Arc`. Product/module/blob digests are memoized, and product
hashing uses typed options rather than formatting unused constant slots. The
internal product-hash version changes; it is not a shader-output change.

The bridge's storage envelope contains small upstream metadata, bulk SPIR-V and
blob bytes, and their digests. Its original bounded lossless byte codec uses
literal and backward-copy tokens; incompressible bodies are stored verbatim.
Bulk bytes appear once. Metadata encodes placeholder SPIR-V headers only during
storage, then the decoder replaces them with the exact scanned executable bytes
before a Product can escape. Upstream metadata decoding/profile checks remain.
There is no debug stripping and no Mesa/translator dependency change.

The optional ABI-8 `artifact_scope_v2` / `artifact_verify_v2` handshake avoids
repeated large SHA operations:

1. Load checks private directory/file ownership, exact modes, regular-file and
   link count, no-follow opens, identity, bounded lengths, unique keys, exact
   record coverage and whole-file CRC. It verifies each bridge envelope's full
   SHA-256, including its request key, compressed body and stored digests, using
   at most one entry-sized scratch allocation. No index becomes visible until
   the whole file has passed its outer checks.
2. The index retains each entry's CRC and verified status. Every lazy `pread`
   recomputes that CRC before returning bytes and the verified flag. This detects
   in-place modification or truncation after load. Atomic replacement cannot
   invalidate a reader: shared descriptors pin the old inode.
3. A verified hit skips the large envelope SHA, bounds every decompression token
   and allocation, checks the request identity, decodes/checks the small upstream
   metadata frame, and scans each real SPIR-V module's header, word counts and
   ID bound. Saved module/blob hashes are covered by the verified envelope.
4. A v1 callback, or an envelope that failed verification, takes the full SHA
   check on read. A repaired outer CRC cannot conceal a bad inner envelope.
   Cache errors become misses; malformed data never reaches Vulkan.

As before, checksums provide integrity and identity, not authentication against
an attacker able to rewrite the private cache and recompute every checksum.

Rust product/stage locks now protect lookup and insertion only; pinned immutable
values are decoded/cloned outside them. Baseline translation runs outside the
Backend mutex and rechecks device liveness after reacquiring it. Exceptions
reacquire the lock before backend activity bookkeeping is destroyed. The new
`relock_ms` timing field records that second acquisition separately. Translation
checkpoint IO also runs outside the Backend mutex; store snapshot IO and payload
reads/writes run outside its index lock. **Vulkan creation and Vulkan cache
maintenance still serialize with the device/queue.** A hit can still wait there.

### Capacity and resident memory

The outer v1 snapshot format remains readable: **256 entries / 256 MiB**, with
an **8 MiB single-entry limit** including the storage envelope. Files larger than
these limits or malformed old files are safely discarded. Updating the bridge
changes its existing source identity, so an old adapter's snapshot is normally
discarded once; an unchanged older adapter can still use the v1 callback path.

Only keys, offsets, lengths, CRCs, verification bits and shared file descriptors
remain resident, about 20 KiB for 256 entries. Payloads are read on demand into a
per-call buffer and released with its scope. Dirty products append to an
immediately unlinked private spool, capped at 256 MiB; if compaction cannot make
space, optional writes are skipped. No collection of dirty artifact vectors
accumulates in backboardd. Checkpoint streams 64 KiB chunks to an exclusive
private temporary file, checks source CRCs again, fsyncs, renames and fsyncs the
directory. Writer leasing and orphan replacement are retained. Concurrent writes
remain dirty and survive snapshot remapping; hits alone do not dirty a snapshot.

Load scratch is at most 8 MiB plus a 64 KiB chunk; checkpoint scratch is 64 KiB
plus small index copies. Each concurrent decode has bounded entry/raw/module
buffers, and live products retain the executable data they need. Existing
process, stage and Vulkan-object caches keep their separate entry/byte budgets;
these numbers are not a bound on total backboardd RSS or the OS file cache.

### OBSERVED: host checks and measurements

Linux x86-64 Xeon E5-2680 v4, optimized Rust/C++, pinned external dependency,
locally authored AIR and lavapipe; no device access or Apple inputs. Timings are
host wall times, with trace on for replay. They are not phone estimates.

| Measurement | Before | After |
| --- | ---: | ---: |
| Warm authored replay, 104 artifact hits: mean `compile_ms` | 2.314 ms | 1.324 ms |
| Same replay: mean backend `hash_ms` | 2.252 ms | 0.010 ms |
| Same replay: all persisted records, 103 unique requests | 1,543,743 B | 985,673 B |
| First authored record | 14,279 B | 9,149 B |
| Synthetic 717,873 B upstream artifact decode + scan | 5.546 ms | 0.562 ms verified bulk path |

The warm replay has zero key-specialization/lowering/stage-read phases in the new
path. Its small metadata decode totals 6.189 ms, reflection/product-plan 12.614 ms
and device-description setup 74.208 ms across 104 requests. In the old path,
key specialization alone totals 104.562 ms. The control has one repeated input
after earlier products leave the process/object caches, hence 103 unique disk
records and one legitimate artifact hit during the cold 104-request pass.

The synthetic 700 KiB SPIR-V scaling case appends OpNop words and is never
executed. Full SHA alone took 4.119 ms and copying 0.048 ms. The verified bulk
path includes decompression, small metadata SHA/decode and the real module scan;
it reuses verified module hashes. It stored 18,777 B. A separate 700 KiB constant
blob case took 0.145 ms and stored 17,356 B, with exact decoded bytes and digests.
These deliberately repetitive cases **do not predict CA compression ratios**.
The real authored replay's total snapshot shrank about 36%.

The production store also round-tripped **256 uncompressed 700 KiB records**:
183,510,144 B including headers, roughly 0.3 s index validation, about 0.6 ms
per `pread` plus CRC, and no additional peak RSS at the host's KiB measurement
resolution during index load. This tests capacity independently of compression.
Tests exercise pinned old snapshots, concurrent read/save/write generations and
post-load corruption. A paused real artifact miss allows another object hit and
another backend's Rust product hit to finish before that miss is released.

Validation includes the existing 12 translation/cache/frontend/backend and
M9g–M9o host suites, ten normal bridge Rust tests plus the explicit large-artifact
benchmark, sanitizer cache/security and concurrency controls, and the new
`test-fuxi-metal-cache-hits.py` replay/capacity runner. Native Objective-C tests
are skipped on Linux. Reproduce without downloads using cached inputs:

```sh
python3 tools/test-fuxi-metal-cache.py --source "$M2V_SOURCE" --build "$BRIDGE_BUILD"
python3 tools/test-fuxi-metal-cache-hits.py --translator "$HOST_TRANSLATOR"
# Optional --baseline "$OLD_HOST_TRANSLATOR" compares the preceding adapter.
FUXI_MTL_BENCH_AIR="$AUTHORED_AIR" FUXI_MTL_BENCH_PROFILE="$CACHED_PROFILE" \
  cargo +1.98.0 test --offline --release --manifest-path "$BRIDGE_BUILD/Cargo.toml" \
  benchmark_large_artifact -- --ignored --nocapture
```

### GUESS and remaining acceptance work

The device artifacts are likely dominated by executable SPIR-V and/or constant
blobs, rather than removable debug instructions or duplicate encoded AIR. Their
actual breakdown and compression ratio remain unmeasured. The supplied logs
cannot prove which old phase dominated 77 ms; code and host measurements show
that key specialization, large software SHA, option formatting and lock scope
were all avoidable contributors. Reflection and small device setup remain.

The **under-5-ms phone hit target is unverified**, as are phone RSS, startup
validation time, UI latency, storage latency, Turnip worker contention and real
interaction FPS. Mac/iOS builds and native Objective-C execution are also
unverified. Compare newly rebuilt artifacts on device when independently
permitted, using the same interaction sequence with trace on for attribution and
then off for latency. Include `relock_ms`, remaining Vulkan waits, integrity
rejections, size traces and peak RSS. New translations still lower shaders and
create Vulkan pipelines synchronously; this change does not make those stalls
disappear.

## Mip feedback passes (Spotlight blur)

**OBSERVED (supplied device evidence):** two boots with the same scripted
Spotlight gesture reached a BGRA8Unorm, 2D, 704×96 texture with ten mip levels,
usage `0x5`. Color attachment zero wrote level 1 over 352×48, load 2/store 1;
the fragment binding was the same texture with view base 0/count 10. The
bind-time `sampling reject attachmentFeedback` immediately preceded the
persisted backboardd runtime-failure/CPU-respawn gate. The supplied session
measurements then showed roughly 20 fps and all five fast cores around 50%
user time. This establishes the rejection and fallback sequence; the descriptor
log alone does **not** establish the shader's LOD or sampler clamps.

**Implemented:** feedback validation runs against the final translated runtime
variant and draw bindings, after samplers and constants are available. The
original host-owned `MipAccess.hpp` examines both stages' SPIR-V image accesses,
matches descriptor set/binding to Metal texture/sampler slots, and conservatively
unions possible mip levels. Nonmip samplers read view-relative level zero;
mipmapped samplers bound implicit or unknown LODs with their min/max clamps.
Plain explicit LODs can be narrowed from scalar constants, immutable uniform
buffer/push bytes, constant-index access chains, scalar copies and simple vector
component extraction. Linear/nearest filtering conservatively includes both
neighboring integer levels. The original Metal view base is added exactly once.
Reads of GPU-written uniform bytes are not inferred from stale host data.

Unknown provenance, unsupported image operations (including fetches that bypass
sampler clamps), arithmetic, dynamic indexing and unrecognized LOD expressions
widen the range; they never infer `L-1` from the destination level or a blur
function name. Any resulting range containing an attached mip is refused.
Different images sharing memory/host ranges and equal IOSurface identities
remain refused. The supported texture model has one slice; array/slice support
is not added by this change. Multiple color attachments are checked individually.

**Views and layouts:** retain the original Metal view's Vulkan base/count. For a
view overlapping an attachment but with proven disjoint **accesses**, bind that
view in `GENERAL`, and use `GENERAL` for the attachment inside the subpass too.
This deliberately keeps the full view instead of rebasing an upsample view:
explicit LODs, implicit derivatives, dimensions and level queries retain their
existing meaning, including nonzero-base Metal views. There is no sampler bias
or SPIR-V rewrite. Other views of that image are promoted to `GENERAL` when
needed so their descriptor layouts agree. An already disjoint Metal view uses
`SHADER_READ_ONLY_OPTIMAL` on its sampled levels and `GENERAL` on the attached
level. Layout state and barriers are tracked separately for each mip, including
source/destination transfers, mip generation and pre-submit rollback. Level-zero
`layout` remains available to existing import/completion code. `GENERAL` is not
permission for same-level feedback; the access proof is still required.

**Opt-in gate policy:** `fuxigpuskip=1` omits an unsupported attachment-feedback
shader draw, preserving its pass load/store actions and subsequent command work.
It covers same-IOSurface refusals in the frontend and unproven/overlapping mip
ranges or image aliases in the backend. It does not suppress resource, shader,
Vulkan, device-loss or fence failures. Default behavior still fails through the
existing gate. Duplicate, malformed and zero-valued keys disable skipping.
Skips increment `skippedSamplingDraws` (also exported as
`fuxi_metal_sampling_skipped_draws`); logs show the first eight and then powers of
two. The periodic backboardd report includes `sampling_skipped`.

**OBSERVED (host verification):** `test-fuxi-metal-mipfeedback.py` exercises the
production backend with authored GLSL on software Vulkan under ASan/UBSan. The
exact reported texture/pass shape reads level zero while writing level one;
pixels, full-view upsampling, rebased views, same-level refusal, active gate
survival with skipping, exact-token policy, mixed per-mip layouts/barrier order
and recording-failure rollback are checked. `MipAccessTests.cpp` covers constant,
uniform and push LODs, unknown/fetch refusal and range unions. The M9k suite also
exercises the exact pass shape through the separately built AIR translator with
this tree's authored AIR, including same-level refusal and rebased upsampling.
The backend fill/copy and injected fence/device-loss/corruption controls, plus
M9o authored line/shadow/trace regression controls, also passed. The skip test
checks cumulative counts and the first-eight/powers-of-two log limit.
Linux runs the frontend suite's portable policy/state controls; the native
`MipFrontendTests.mm` additions (encoding the reported descriptor and same-
IOSurface default/skip behavior) require Mac Foundation/Metal/IOSurface. The
native frontend, defaults and Metal-SDK fixture suites require a Mac and their
existing build outputs.

**GUESS / unverified:** the private CA downsample reads `L-1`, and its upsample
reads `L+1`. The shader/sampler data needed to prove those specific accesses was
not supplied. These host controls establish supported access patterns, not that
every private blur shader is recognized by the conservative interpreter. Device
Turnip (`TU_DEBUG=sysmem,nocb`), actual Spotlight/Control Center/notification
pixels, performance, compiler floating-point behavior and continued GPU rendering
after blur remain unverified. Software Vulkan success is not an on-device result;
Vulkan validation-layer coverage was unavailable in this host environment.

## Frame overlap and submit cost

### OBSERVED: supplied trace and code

The supplied interaction excerpt after `e7d9133` has one trace row with
`wall=19.055/27.153/32.887`, `cpu_before_commit=8.757/15.676`,
`backend_submit=7.099/9.152`, `gpu_wait=11.335/17.216`,
`gpu_exec=10.255/16.092`, and 140.57 draws/descriptor sets per surface command.
Slash triples are p50/p90/max; pairs are p50/p90. Encode time is **outside**
`wall`, and GPU timestamps overlap the fence wait. Percentiles of separate
phases cannot be added to derive a total percentile. The reported session stays
on the GPU and displays about 24–26 Hz; this is supplied device evidence, not a
new device experiment.

Previously `Frontend.mm` called `Backend::submit` on the committing thread.
That method held the device mutex across `vkWaitForFences` (two-second timeout),
invalidation and cleanup. Its single command pool, descriptor pool, fence and
query pool could only be reset after that fence. The stack IOSurface lease,
resource release and completion handlers also depended on synchronous return.
Scheduled handlers were delivered after the wait. This explains the synchronous
`gpu_wait`; `fuxigpupipelined` only reused alias-copy scratch.

Per draw, submit validates bindings/bounds, calls the translator's draw planner,
looks up or creates a runtime variant when requested, checks sampled access and
attachment aliases, resolves render-pass/framebuffer/view caches, and records
barriers, a render pass, pipeline/bindings and the draw. The old descriptor path
allocated one set per draw and called `vkUpdateDescriptorSets` once **per
binding**. Runtime variant lookup formatted every slot into an allocating
`std::string`, then searched at most eight variants. The expensive mip
interpreter **already skipped non-aliasing samples**; their whole-view definition
checks remain. Image barriers and the per-operation memory barriers remain.

### Opt-in settings and completion behavior

| Boot argument | Behavior |
| --- | --- |
| `fuxigpuoverlap=1` | Serial submission worker, separate serial fence-retirement worker, two admitted commands per Metal queue and at most two Vulkan submissions per backend. |
| `fuxigpusubmit=1` | Exact descriptor-set reuse within each submission, batched descriptor updates on misses, and fixed typed runtime-variant keys. |
| `fuxigpuframetrace=1` | Existing trace; with overlap also reports `overlap_depth` and `completion_ms` p50/p90. |

All are sampled once, absent/off by default, with no new environment fallback.
Only one exact `=1` enables; zero, malformed values and duplicates disable.
New names are 14 and 13 characters respectively. Existing admission, backend,
feedback, cache and scratch policies remain independent. Rebuild FuxiMetal;
this change requires no bridge, Mesa, metal2vulkan or NativeMetal modification.

With overlap, commit snapshots no-copy shadow bytes and queues the command,
then returns without waiting for its GPU fence. Explicit `enqueue` reserves
queue order even when commits arrive in reverse order. Admission backpressure
can block a third enqueue/commit until an earlier command retires. There is no
unbounded internal work queue. An explicitly enqueued command must eventually
be committed; reserving both slots without committing either prevents progress.
Each backend lane owns its resettable pools, fence, timestamps and scratch.
Queue submission/recording uses the device mutex; deferred fence waiting does
not. A ticket pins buffers, textures, pipelines, samplers and transient Vulkan
objects until retirement, independent of caller/cache references.

`waitUntilScheduled` waits for successful submission or its error outcome.
Scheduled handlers run before completed handlers. Per-queue retirement and
handler delivery are serial, including when a caller sets a completion queue.
`waitUntilCompleted` includes completion-handler delivery; calling it on the
same command from its completed handler remains nonblocking. Handlers added
while a fence is pending join its completion; late completed handlers run once
on the delivery queue. Resources remain retained through GPU retirement.
Presentation is delivered only after successful completion. IOSurface leases
are released after the fence and invalidation; reuse of the same surface waits
for its prior lease. Surface IDs are acquired in sorted order to avoid cycles.
The original synchronous commit body remains the default path, with inline
default handlers.

Process-local event boundaries still use synchronous segments **on the
submission worker** so signals follow preceding GPU writes. The final segment
can retire asynchronously. Shared shadow refreshes and CPU index inspection
drain previous flights. Noncoherent host allocations shared with an earlier
flight drain before whole-allocation flush/invalidation can race that flight.
Shadow refresh and recording are serialized together by the backend mutex;
client bytes are snapshotted before commit returns. Mip proofs refuse to infer
uniform LODs from host bytes while an earlier submission could have written them.
These conservative cases can limit overlap.

Fence timeout/device loss or an in-flight poison fails completion, releases
admission slots, and quarantines GPU objects and producer leases. Device loss
from another backend API also poisons pending submissions before cleanup.
No uncertain DMA allocation is reset or destroyed, and failed overlap commands
do not present. Quarantine intentionally persists until process/kernel teardown.

Descriptor reuse is bounded to 256 exact entries per submission. Keys include
set-layout identity, binding/type, buffer handle/offset/range and image
view/sampler/layout; comparisons do not hash padding or omit dynamic ranges.
Sets are immutable after creation and belong to their lane's pool until its
fence. Push constants remain separate commands. Pools still size for the
conservative per-draw maximum and reset only after retirement. The typed variant
key preserves the old specialization facts, including exact float bits, without
formatting strings. Extension discovery already uses
`vkEnumerateDeviceExtensionProperties`; this implementation uses portable
Vulkan descriptor sets and does not enable `VK_KHR_push_descriptor`. Actual
Turnip extension availability was not queried on a device.

With trace enabled, `overlap_depth` is admitted depth (including the command)
at reservation; `completion_ms` is commit entry through fence retirement,
invalidation, lease release and scheduled-handler delivery, before completed
handlers. `wall` ends at fence retirement and lease release, so a slow scheduled
handler can make completion latency larger than wall. Depth is not GPU utilization. `gpu_wait` now
belongs to the retirement worker and overlaps CPU encoding/recording; it must
not be added to CPU work to estimate throughput. Counts combine the creating
thread's encode interval when known with the submission worker's own counters;
GPU time comes from the command's own ticket, avoiding overlapping process-total
attribution. Shadow snapshot allocation/copy is included in wall; `shadow_copy`
measures the worker's refresh. Trace-off measurements are still needed.

### OBSERVED: Linux verification and host measurements

`test-fuxi-metal-overlap.py --glslang "$GLSLANG"` uses only local Vulkan headers,
lavapipe, authored GLSL and the existing test adapter. It records 140 sampled
draws per frame, alternates two texture bindings, warms three frames, then
measures 40 frames (5,600 draws) in fresh-process A/B/B/A order. It checks final
pixels; timing excludes fence waiting/JIT and is not a pass/fail threshold.
Descriptor, object and recording subphases are measured by the existing backend
counters. The fixture does not exercise runtime variant creation.

Optimized C++ host run, timings in microseconds per draw (two runs per path):

| Path | Submit CPU | Descriptor API/cache phase | Object API/cache phase | Remaining recording | Sets/frame |
| --- | ---: | ---: | ---: | ---: | ---: |
| Default | 14.154 / 17.795 | 5.292 / 6.555 | 5.142 / 6.301 | 2.976 / 3.819 | 140 / 140 |
| `fuxigpusubmit=1` | 7.283 / 8.596 | 0.303 / 0.415 | 1.695 / 2.045 | 4.305 / 4.915 | 2 / 2 |

Submit CPU for 140 draws was 1.982 / 2.491 ms versus 1.020 / 1.203 ms.
There is substantial host scheduling variance; earlier runs also favored reuse,
but these numbers establish neither a phone speedup nor a stable percentage.
The descriptor phase includes the exact-cache search; binding normalization is
in remaining recording. Validation, draw planning, barriers and driver submission
remain costs. Different per-draw bindings may have much less reuse; the bounded
linear search can itself become a cost.

ASan/UBSan controls submit two real Vulkan commands before either ticket is
retired, block a third submission until a lane retires, check GPU copy ordering
and pixels, and verify no buffer destruction while pinned. Sampled controls
drop caller texture/sampler/pipeline references and the object cache with both
flights outstanding; all GPU allocations survive until the last retirement.
Injected fence timeout, device loss and a device loss from another API fail both
pending tickets without destroying their DMA objects. An ICD wrapper hides
HOST_COHERENT to verify shared-range draining and flush/invalidate sequencing;
this is a deterministic host control, not Turnip cache qualification. Portable
surface tests block same-surface reuse and wake poisoned waiters without handoff.
Trace tests cover overlap depth/latency fields and preserve default log fields.

The existing frontend portable controls, software-Vulkan backend, frame-reuse,
cache, translation, M6 sampled and M9g/M9h/M9i/M9k/M9l/M9m/M9n/M9o suites passed.
M9g/H/I/K/L/O were also run with `fuxigpusubmit=1`; mip-feedback pixels/refusals
passed with submit reuse and its feedback/skip combinations. The old M6 GLSL
adapter needed its current identity/product-hash exports and MRT color-zero mask
initialized; only that authored test adapter changed. The metal CI dry-run and
Python syntax checks passed. Reproduction uses cached external inputs only:

```sh
python3 tools/test-fuxi-metal-overlap.py --glslang "$GLSLANG"
python3 tools/test-fuxi-metal-mipfeedback.py --glslang "$GLSLANG"
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"
python3 tools/test-fuxi-metal-frametrace.py
python3 tools/test-fuxi-metal-m6.py --headers "$VULKAN_HEADERS" --glslang "$GLSLANG"
FUXI_MTL_TEST_BOOTARGS=fuxigpusubmit=1 \
  python3 tools/test-fuxi-metal-m9k.py --translator "$HOST_TRANSLATOR"
bash tools/ci/build.sh metal --dry-run
```

`OverlapFrontendTests.mm` is wired into the existing frontend runner and thus
the metal build. It pauses retirement, checks commit return, explicit enqueue
order under reversed commits, two-command backpressure, scheduled/completed
handlers, pending/late handlers, self-wait, completion-queue routing, presentation,
resource lifetime, no-copy snapshots and in-flight poison. **Native Objective-C
execution, Foundation/Metal/IOSurface integration and the full Mac/iOS build
require a Mac and were not run on Linux.** Defaults/SDK fixture suites likewise
remain Mac-only. Host C++ tests do not establish native GCD/Objective-C behavior.

### GUESS and risks requiring device acceptance

Overlapping CA encoding/recording with GPU work should remove the old serial
sum from steady-state throughput. It does not reduce the GPU's own roughly
10 ms workload or guarantee 60 Hz. Same-surface reuse, shared shadows,
noncoherent aliases, indexed CPU validation, event waits, translation and
backend contention can still serialize work. Two lanes increase retained pools,
scratch and in-flight resources; shadow snapshots also add host memory/copy
cost. Scheduled handlers run on the submission worker and must return for that
queue to progress. Completion handlers must not synchronously wait for later
handlers on the same serial delivery queue.

Actual Turnip behavior with `TU_DEBUG=sysmem,nocb`, driver CPU cost, private CA
resource reuse, IOSurface/scanout timing, pixels under Spotlight/Control Center,
thermal behavior, memory pressure, interaction Hz and the 60 Hz target are
**unverified**. No device access, hosted runs, downloads or upstream source
changes were used. Compare identical warm interactions with overlap and submit
reuse individually, then together, preserving all other boot settings. Inspect
depth/latency and failures with trace on, then repeat with trace off; a two-deep
queue can improve throughput while increasing input-to-display latency.

## GMEM trial (`fuxigpugmem=1`)

OBSERVED (scripted gestures, `fuxigpuoverlap=1 fuxigpusubmit=1`): two frames
stay in flight, and blur frames take 17/28/32 ms end to end (median/p90/max of
~300-frame windows; gpu_exec ~10 ms median). CoreAnimation presents at ~26-30 Hz,
which is about every other 60 Hz vsync. GUESS: per-frame latency, not
throughput, keeps CA at half rate, and Turnip's sysmem rendering makes the
bandwidth-heavy blur passes slow.

`fuxigpugmem=1` sets the default `TU_DEBUG=nocb` (tiled GMEM rendering) instead of
`sysmem,nocb`. An explicit TU_DEBUG environment value still wins. UniXNU saw
GMEM hangs on A740, so this is a diagnostic opt-in only.

## Render pass merging

**OBSERVED (supplied device measurements):** CoreAnimation blur frames have
about 115 draws, with `render_passes/frame == draws/frame`. Median `gpu_exec`
is about 9.6 ms, or 83 microseconds per draw. The GMEM trial did not change
that result. These are supplied measurements; no device was accessed here.
The pre-merge backend recorded a separate `vkCmdBeginRenderPass`/`vkCmdEndRenderPass`
and attachment/sample transitions plus a memory barrier for every draw.

**OBSERVED (supplied follow-up, merge + overlap + submit + feedback):** about
300 frames per window show 114–119 draws and 71–73 passes per frame, with
`gpu_exec` median 8.6 ms versus the supplied unmerged 9.6 ms. Window split
totals were `encoder:2176,fetch:4599,shadow:2194,non_draw:301`, all other
reasons zero. Ordered framebuffer fetch was enabled through
`VK_EXT_rasterization_order_attachment_access`. CA still presented around
28 Hz; backboardd reported 1,757 late frames out of 5,435. These measurements
precede the fetch/shadow extension below; they are not results of this change.
Split reasons report the first blocker: removing fetch/shadow isolation may
expose encoder or other boundaries. Subtracting those totals from passes is
not a valid prediction of the new pass count.

**GUESS:** repeated sysmem render-pass boundaries trigger Turnip CCU
flush/invalidate/wait work and now dominate the GPU interval, contributing to
CA's roughly 30 Hz presentation. Equal pass/draw counts and the unchanged GMEM
result motivate this experiment; they do not measure CCU costs or establish
that merging will reach 60 Hz. The follow-up identifies fetch and shadow
refresh isolation as removable boundaries; legitimate encoder boundaries
remain. Whether their removal improves CA latency or presentation is still
a device experiment.

Enable **`fuxigpumerge=1`**, independently of feedback, overlap, descriptor reuse
and GMEM. Only one exact `=1` boot token enables it; absent, zero, malformed and
duplicate tokens disable it. There is no environment fallback. Rebuild
FuxiMetal; no translator or upstream driver changes are required. Default
recording and the default frame-trace format retain their previous behavior.

The frontend assigns a command-local identity when it creates each Metal
render command encoder. Its existing first-load/later-load and final-store
records remain authoritative; `endEncoding` still sets every final attachment's
store action. Consecutive encoders always have different identities, including
when they reuse identical attachments and start with Load. Identity zero means
unknown and cannot merge. Command submissions and event segments are also
boundaries; runs never cross them.

The fetch extension retains the **existing** pipeline render-pass layout. On
an ordered-capable device, every pipeline (ordinary, fetch, topology and runtime
variant) already uses one subpass with all present colors also referenced as
inputs, `VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT`,
and `GENERAL` layouts. Sparse slots retain `VK_ATTACHMENT_UNUSED` in both
reference arrays; actual framebuffer views are compact. This common layout is
essential: input attachment references participate in render-pass compatibility.
Changing loads/stores does not change that compatibility. Input descriptors
bind the current operation's target view at the fetched slot; merged runs require
identical targets/levels, so those are the active merged framebuffer's views.
The color-blend pipeline state also enables rasterization-order attachment access.

No new pipeline mode or `fuxigpumerge2` is needed: pipeline creation remains
unchanged with merge off/on. The device's enabled ordered-access value is already
in `CompileOptions`, translator identities/product keys and device cache identity.
Runtime variants belong to their original pipeline and inherit its options;
topology variants use its render pass. The render-pass action cache is local to
a pipeline with those fixed formats and ordered mode. There is no cache entry
shared between incompatible merge/nonmerge layouts. Without ordered support,
the old ordinary merge behavior and per-operation fetch fallback/refusal remain;
this change does not admit unordered fetch.

The opt-in planner runs after runtime variants, descriptor plans, sampling
access proofs and skipped draws have been resolved. A run requires identical
nonzero encoder identity, attachment mask/count, texture objects and mip
levels, render-area fields, and compatible color formats. The admitted model
already has one slice/depth plane and sample count one; nonzero slices and
multisampling remain refused by the frontend. Every removed boundary must have
Store on its left and Load on its right, for every present attachment.
The Vulkan pass uses the first draw's loads/clear colors and the final draw's
stores. Intermediate logical attachment definition checks and final
`setDefined` updates retain their original semantics, including store DontCare.
Viewport, scissor, pipeline, descriptors, push constants, vertex/index bindings
and the draw command are still recorded individually.

A texture shadow is a read-only optimal image used when a buffer-backed linear
sampled texture cannot satisfy Vulkan's format/memory/layout requirements.
`readShadow` records a backing-buffer-to-optimal-image refresh before use. This
GPU transfer is separate from the frontend's CPU copy of no-copy buffer snapshots
on the submission worker. CPU writes must precede commit; overlap's existing
snapshot, shared-range drain, flush/invalidate and retirement rules still apply.

All refresh transfers for a run now execute **once per distinct shadow before
any sampling/attachment transitions**, then all image/mip transitions execute
before begin. Read-layout transitions are deduplicated separately from refresh
transfers; otherwise a repeated sample could copy in a shader-read layout.
Deduplication ends with the run, so a later frame or run sees new backing bytes.
A later shadow consumer splits if its backing buffer shares memory or host
ranges with the unchanged attachments written by earlier draws in that run.
A refresh needed only on the first draw can precede the run, even with such an
alias. Shader storage writers and explicit transfers already form boundaries,
so their dependent refreshes stay after the write. Ordinary read-only shadows
no longer split.

Ordered input reads can follow earlier color writes inside this one subpass;
ordinary sampled attachment feedback still splits. The per-operation memory
barrier remains at the end of the run. Singleton operations retain their old
transitions and barriers. Texture layout/definition snapshots, including refreshed
shadows, still roll back together on a pre-submit failure; deferred retirement
keeps the existing lane ownership.

Splits are deliberately conservative:

- `encoder`: different or unknown encoder identity.
- `attachments`, `area`: changed attachments, levels, formats or render area.
- `actions`: an internal clear, DontCare load or discard store.
- `attachment_sample`: any sampled attachment image or memory/host-range alias,
  even proven-disjoint mips and GENERAL views. This isolates both producer and
  consumer, including `fuxigpufeedback=1` draws, preserving inter-draw barriers.
- `storage`: writable or unproven storage descriptors. Storage buffers can merge
  only when the final SPIR-V explicitly decorates that variable NonWritable in
  every referencing stage (including the translator's vertex-read buffers).
  Member-only/unknown contracts stay isolated. An earlier shader's buffer write
  cannot lose its barrier.
- `buffer_alias`: an index buffer sharing attachment memory or host ranges.
- `shadow`: a later refresh reads backing storage written by an earlier
  attachment draw in the run (memory or host-range alias).
- `fetch`: framebuffer fetch/input attachments without enabled ordered access;
  the existing pipeline contract normally refuses these before recording.
- `non_draw`: uploads, image/buffer copies, mip blits, fills and empty passes,
  including render-area-limited `clearOnly` passes and skipped shader draws.

Existing refusals, such as GPU copy destinations used as draw buffers in the
same submission, remain refusals. This feature does not broaden resource or
feedback admission. In particular, the feedback opt-in still cannot define
same-draw same-texel feedback results; splitting only preserves ordering
between draws.

With `fuxigpuframetrace=1`, existing `draws/frame` and `render_passes/frame`
count actual recorded draws and Vulkan begins. Merge mode appends
`merged_draws/frame` (continuations, equivalently saved pass begins) and
`merged_splits=encoder:N,attachments:N,...`. Split counts are window totals for
adjacent operation boundaries involving a draw, classified by the first
blocking reason; a hazard can count on both sides. They are neither counts of
failed draws nor per-frame averages. The default trace has no appended fields.

The `test-fuxi-metal-merge.py --glslang "$GLSLANG"` suite runs production Backend
A/B pixel controls on local lavapipe with authored GLSL, ASan/UBSan and a Vulkan
recording proxy. It compares every defined output byte with merging off/on
for blending, first Clear/Load, discard followed by clear, partial render areas,
encoder/action boundaries, uploads/copies/mip blits, empty partial passes,
mip producer/consumer and GENERAL-view hazards. A storage-buffer read/write
control requires two ordered single-fragment increments. Feedback and
submit/overlap combinations run in fresh processes; true feedback compares
only an untouched mip. Discard checks verify Vulkan storeOp and undefined
bookkeeping, not unspecified discarded pixels. The proxy checks real
pass/draw counts, actual load/store operations and the absence of barriers
inside a pass. It also compares pipeline/pass attachment-reference compatibility,
checks input descriptor views against the active framebuffer, and verifies that
buffer-to-image copies use their most recently transitioned layouts outside passes.
Recording rollback and two outstanding retirement tickets are also exercised.

**OBSERVED (host correctness controls):** lavapipe exposes enabled ordered color
access. Nineteen interleaved programmable-blend fetch/ordinary blend draws reduce
from 19 passes to one with byte-identical output. A cleared first fetch checks
an independently calculated color/alpha as well as A/B identity. Mip-target
changes and two outstanding fetch flights check descriptor-view selection.
The proxy forces production linear-shadow fallback by hiding linear sampling
support: eleven draws alternating two shadows reduce from 11 passes / 11 copies
to one pass / two copies. Three frames change CPU backing bytes and require new
output each frame. Backing-buffer fills and attachment-memory aliases prove
refreshes stay after producers; a first-draw-only alias refresh still merges.
Shadow rollback and two outstanding shadow flights pass; the latter changes
CPU bytes between submissions using independent per-frame backing allocations,
so it exercises overlap without racing bytes still read by the earlier flight.
All controls run with feedback off/on and with overlap/submit reuse combined. Hiding the ordered
extension tests the fallback separately. When ordered color access is absent,
fetch pixel controls print an explicit SKIP; ordinary/shadow controls still run.

M9l adds byte-identical mixed-format 2/4/8 MRT runs, sparse slots, and ordered
four-input fetch merges across fetch/ordinary pipeline changes in both directions.
The portable `OperationAPITests` compiles the Mac model's `TestBackend.hpp` and
checks encoder identity plus sparse attachment helpers/copies; no Operation/API
fields changed in this extension. Native frontend assertions across `endEncoding`
and full Objective-C compilation still require a Mac.

**OBSERVED (host benchmark):** optimized C++, local lavapipe, 140 sampled draws
in one encoder identity, two alternating bindings, three warm-up frames and 40
measured frames per fresh process. Each setting runs A/B/B/A; times are means
per frame within each run, in milliseconds. Submit CPU excludes fence waiting;
GPU timestamps cover the software Vulkan execution interval. Descriptor reuse
is held constant within each comparison. Pixel and pass-count assertions run
in every benchmark; timings have no pass/fail threshold.

| Descriptor reuse | Merge | Passes/frame | Submit CPU ms/frame, runs 1 / 2 | GPU timestamp ms/frame, runs 1 / 2 |
| --- | --- | ---: | ---: | ---: |
| Off | Off | 140 | 1.941 / 3.236 | 4.061 / 6.871 |
| Off | On | 1 | 2.359 / 1.751 | 5.057 / 4.540 |
| On | Off | 140 | 0.931 / 1.016 | 5.512 / 4.939 |
| On | On | 1 | 0.391 / 0.558 | 3.375 / 5.036 |

Each merged frame reports 139 merged continuations and still executes all 140
draws. Sets/frame remain 140 with descriptor reuse off and two with it on.
Scheduling variance is substantial, especially in the reference GPU interval;
these results establish host pass reduction and correctness, not a stable
speedup percentage or an Adreno latency prediction.

**OBSERVED (encoder boundary cost control):** the same runner also holds merge
and descriptor reuse **on**, varying only one encoder versus 140 encoder IDs.
It requires all 139 encoder split reasons and all 140 draws on the split path.
Fresh-process A/B/B/A, three warm-up / 40 measured frames, same local lavapipe:

| Encoders/frame | Passes/frame | Submit CPU ms/frame, runs 1 / 2 | GPU timestamp ms/frame, runs 1 / 2 |
| ---: | ---: | ---: | ---: |
| 140 | 140 | 0.818 / 1.356 | 3.257 / 9.497 |
| 1 | 1 | 0.373 / 0.437 | 3.381 / 3.659 |

The merged draw's amortized submit CPU cost is **2.663 / 3.121 microseconds**;
the split draw costs **5.839 / 9.687 microseconds**. The paired difference
divided by 139 extra boundaries is **3.201 / 6.612 microseconds CPU** and
**−0.892 / 42.000 microseconds GPU** per boundary. This measures the backend's
boundary plus transitions/barrier path, not an isolated Vulkan begin/end call.
The negative first GPU difference and large second outlier show that this host
run cannot establish a stable marginal GPU boundary cost. No timing threshold
is asserted, and none of these numbers estimates Turnip CCU cost.

Linux verification passed: the new merge suite; frontend portable controls
(including exact-token policy, storage declaration proofs and trace/reset
fields); backend fill/copy plus timeout/loss/corruption controls; frame trace;
M6 with authored GLSL; mipfeedback; overlap; cache, cache-hits and translation;
M9a through M9b (offline Rust bridge tests and production cache controls);
M9g/H/I/K/L/M/N/O; staging's policy/storage controls; and the metal build
dry-run. M9g/H/I/K/L/O also passed with merge, overlap and descriptor reuse enabled.
M9g/I/L/O additionally ran with feedback enabled. H/K contain strict feedback/
undefined-read refusal assertions: enabling feedback intentionally defeats those
assertions, so their supported combined run keeps feedback off; feedback-on
pixels and overlap are covered by the merge/mipfeedback suites. Portable checks
also include the Mac model Operation API guard. The older metal/metal2/metal2g/
metal2h kernel/ABI suites passed as well.
M9l compares byte-identical MRT output across pipeline changes and checks mixed
final attachment discard actions as well as sparse attachments and fetch merges.
All external inputs were already cached; no downloads or upstream source edits
were used. Reproduction of the new controls and MRT comparison:

```sh
python3 tools/test-fuxi-metal-merge.py --glslang "$GLSLANG"
FUXI_MTL_TEST_BOOTARGS='fuxigpumerge=1 fuxigpuoverlap=1 fuxigpusubmit=1' \
  python3 tools/test-fuxi-metal-m9l.py --translator "$HOST_TRANSLATOR"
python3 tools/test-fuxi-metal-frontend.py
bash tools/ci/build.sh metal --dry-run
```

**Unverified / risks:** native Objective-C encoder identity tests, GCD/IOSurface
integration and the full Mac/iOS build require a Mac. Defaults/pipeline/M6b/M7
wrappers require native SDK-built fixture inputs; artifact qualification and
CA offline replay require external build or proprietary inputs and were not
run here. The authored AIR paths were covered by the listed Linux suites.
No firmware or Apple binaries were used. The recording proxy is not full
Vulkan validation-layer qualification.

CA grouping, pass counts and timing for the original merge opt-in are supplied
above. The fetch/shadow extension's device merge rate, Turnip CCU behavior under
`TU_DEBUG=sysmem,nocb`, pixels, memory pressure, latency and presentation Hz
remain unverified. Attachment-sampling, storage and dependent shadow refresh splits
can leave passes intact, especially in mip chains. Actual encoder boundaries
remain regardless of the cost measured on a host. Hoisting adds
submission-owned planning/transition state; it does not add Vulkan in-flight
lanes or extend work across encoder/submission boundaries. Feedback's existing
same-draw undefined texels remain undefined. Compare identical warm device
interactions with only merge toggled, inspect pass/draw counts and split
reasons with trace on, then repeat with trace off. A host pass reduction does
not establish the guessed device bottleneck or a 60 Hz result.
