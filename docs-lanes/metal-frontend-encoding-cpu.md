# Frontend encoding CPU replay

Linux results, 2026-10-26, Xeon E5-2680 v4, clang 18.1.3, C++17 `-O2`.

```
python3 tools/bench-fuxi-metal-frontend.py --baseline-ref d167dd07 --frames 15000 --profile
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-backend.py --headers "$VULKAN_HEADERS"
python3 tools/test-fuxi-metal-frametrace.py
```

The frontend's existing Linux harness is the portable `TestBackend.hpp` CPU model;
Foundation/Metal Objective-C frontend tests require macOS. The replay models the
recording sequence in `Frontend.mm`, using its portable buffer/operation types,
binding bounds, shadow ranges/index snapshots, and the new shared encoding helpers.
The baseline builds against headers archived from the indicated revision and uses
the old retention and commit-copy adapters. This does not measure Objective-C
messages, Foundation descriptor formatting, CA's own CPU work, or Vulkan allocations.
Pass/pipeline diagnostic strings are synthesized with public descriptor-like fields;
render descriptor creation/validation itself is not timed. Results are host model
wall time, not a prediction of device frame-time savings.

Each frame creates one command, 31 encoders with one color target and mixed
load/store actions, and 149 draws. Each draw sets a pipeline (four, repeated), a
shadowed no-copy ring vertex buffer plus offset, 16–256 inline bytes, a fragment
buffer, one or two sampled textures, and a sampler. Every fourth draw sets
viewport/scissor; alternating draws use u16 quad indices. Commit scans frozen
indices, refreshes shadowed vertex ranges, submits through the CPU model, and
releases command resources. The inline buffer still allocates once per setBytes,
as in the real frontend. Both variants produce the same resource/draw checksum.

Three independent runs per variant, each with 200 warmup frames. The reported
central value is the median of the three run means (not a frame percentile):

| Model | ns/draw | ns/frame | Run range, ns/frame | sizeof(Operation) |
| --- | ---: | ---: | ---: | ---: |
| Before | 5,265.0 | 784,484.7 | 778,495.4–800,343.0 | 1,728 |
| After | 3,816.3 | 568,633.1 | 559,041.4–587,130.1 | 1,712 |

Reduction: 27.5%, or 215,852 ns per modeled frame. Draw cost divides the entire
frame, including commit/cleanup, by 149. Device confirmation still needs the same
scroll trace's `cpu_before_commit`, `frontend_wall`, and `frontend_cpu` fields.

`perf` is denied by the host's perf-event permissions. The benchmark instead takes
1 kHz SIGPROF PC samples in a separate run, walks bounded frame pointers when the
PC is in an external library, and attributes that work to the nearest application
caller. This is caller attribution, not an inclusive call graph. The timing runs
have sampling disabled. Baseline: 11,699 samples; after: 8,428 samples.

| Sampling highlight | Before | After |
| --- | ---: | ---: |
| Operation vector deep-copy helpers | 13.9% + 4.0% | absent from top 12 |
| Resource-set hash lookup | 2.8% | 1.2% |
| Resource-set insertion helper | 2.2% | absent from top 12 |
| Draw recording (including attributed libc work) | 24.0% | 32.5% |
| Frame setup/cleanup (including attributed libc work) | 24.6% | 27.1% |
| CPU-model buffer allocation | 3.0% | 4.8% |

Changes: a 64-entry command-local resource cache bypasses repeated dynamic
classification and set/map work. The existing ownership set remains authoritative;
cache collisions fall back to it. Cache keys also preserve every unbounded shadow
copy reason, including escalation from bounded binding to useResource/blit.
Diagnostics use immutable shared text rather than allocating/copying a long string
for every draw and event-segment copy. Event-free synchronous and asynchronous
submissions pass the owned operation vector directly. Redundant pipeline/texture/
sampler setters skip immutable checks for exact frontend classes only, after
encoder-status and slot validation; subclasses keep the original checks.
The fixed-size operation state copy remains: the profile identified deep string
and whole-vector copies as the larger removable cost. No draw planner, barriers,
merge logic, or shadow-range implementation changed.

Existing trace fields remain. At `fuxigpuframetrace=2`, the new
`frontend_draw_ns/frame` reports p50/p90 estimated draw-recording totals, based on
one timed call in sixteen (ten samples for 149 calls), alongside
`frontend_draw_samples/frame`. Level 0/1 performs no draw clock reads. This samples
`record:` only, including validation; state-setting calls remain in `frontend_wall`.
The estimator extrapolates measured wall nanoseconds by calls/samples and can be
noisy for heterogeneous draws.

Validation passed: the complete portable frontend test runner with ASan/UBSan,
production-backend fill/copy and injected timeout/loss/corruption checks, five
frame-trace/scratch-reuse configurations, and a sanitized 20-frame replay plus its
200-frame warmup. Regression tests cover cache collisions/reset, bounded-to-full
reason escalation, diagnostic sharing/snapshots/moves, trace fields, and disabled
sampling. New macOS Objective-C coverage checks actual resource ownership/release,
rebinds across encoder boundaries, and redundant-state rejection after endEncoding.
It was added to normal and compile-only runners but could not be run on Linux;
its render textures explicitly use Private storage to avoid macOS managed defaults.
