# FuxiMetal frame barriers

## Observation and accounting

The supplied backboardd trace uses `fuxigpumerge=1 fuxigpuperf=3 fuxigpuoverlap=1`: about 97 draws, 61 render passes, 87.5 image barriers, 44 skipped read barriers and 6.8 ms GPU execution per frame. `area:1299` and `non_draw:674` are window totals, whereas the image/pass fields are per-frame averages. Split counters count adjacent operation boundaries, not draws or pass breaks attributable to unique resources. `merged_draws` counts continuation draws, not entire merged runs. Independently aggregated medians need not satisfy the per-sample identity `draws = draw passes + continuation draws`; empty partial passes also contribute passes. See [FrameTrace.hpp](../tools/fuxi-metal/FrameTrace.hpp#L151) and [trace formatting](../tools/fuxi-metal/FrameTrace.hpp#L179).

`image_barriers` counts image barrier structures, currently one per mip, rather than all `vkCmdPipelineBarrier` calls. The recorder also emits a HOST-write entry memory barrier, an ALL_COMMANDS memory barrier after each run/non-draw operation, a final HOST-read barrier, and transfer dependencies. None of those memory-only barriers increments `image_barriers`. See [recording entry and transitions](../tools/fuxi-metal/Backend.hpp#L1180) and [recording exit](../tools/fuxi-metal/Backend.hpp#L1416).

The trace supports investigating render-pass fragmentation on a tiler. It cannot establish that every barrier resolves the full target, quantify GMEM bandwidth, or assign barriers to particular CA objects. That requires device/driver measurements; no such measurements were made for this change.

## Why the splits fire

The planner runs after shader variants, descriptors, mip access proofs and skipped draws are final. [The admission check](../tools/fuxi-metal/Backend.hpp#L1093) isolates operations that may write outside the attachment set: shader storage writers, unsupported fetch, attachment sampling/aliases, and index-buffer aliases. Adjacent draws must then have compatible attachment identities, levels, formats, actions and encoder identities. Shadow refreshes whose backing aliases an earlier attachment also split. Those restrictions remain with bit 4.

**`area` before bit 4:** [the boundary check](../tools/fuxi-metal/Backend.hpp#L1110) compares the raw `areaWidth/areaHeight` pair for equality. These fields represent Metal's top-left render area, not the draw's scissor or viewport. [The frontend](../tools/fuxi-metal/Frontend.mm#L1199) derives them from `renderTargetWidth/Height`, defaults them to the target extent and copies them into each recorded draw. The backend also accepts zero as a full-extent shorthand. Different partial areas, or explicit full size versus zero, therefore split even with identical attachments, STORE/LOAD actions and independent sampled layers. Scissor and viewport changes alone already merge because they are recorded per draw.

A single Vulkan render pass uses one `renderArea` for load/store actions. Simply ignoring this comparison would be incorrect: enlarging an initial partial CLEAR clears extra pixels, and using a smaller final discard over a larger merged area could discard pixels that Metal preserves. Bit 4 computes the bounding rectangle of the whole run. Expansion past the first rectangle requires LOAD on every attachment; CLEAR/DontCare loads may merge only while the union stays inside their original rectangle. A final store over a smaller rectangle must STORE. Existing intermediate STORE/LOAD checks remain. Every draw's original scissor stays in force, and validation already bounds it to that draw's area. [Union proof](../tools/fuxi-metal/Backend.hpp#L1116), [run accumulation](../tools/fuxi-metal/Backend.hpp#L1140), [render-area recording](../tools/fuxi-metal/Backend.hpp#L1300).

**`non_draw`:** the same admission check rejects non-render operations, including image copies, buffer/image uploads and readbacks, buffer fills/copies, mip generation, and `clearOnly` empty encoders. Both boundaries around an intervening operation can count: draw → copy → draw yields two `non_draw` increments but only two draw passes. Consecutive non-draw operations without a shader on either side do not increment the split diagnostic. An empty partial encoder itself executes a Vulkan render pass so its clear obeys the rectangle; a full empty clear uses a transfer clear. [Partial/full empty paths](../tools/fuxi-metal/Backend.hpp#L1363).

A real backdrop copy reads earlier target pixels, and its blur consumer reads the copy's destination. Neither can move across the producer/consumer without changing Metal ordering. This change keeps all non-draw operations in their original positions. It does not attempt a general transfer scheduler: backing-buffer aliases, imported host ranges, partial updates and shader storage writers require more than distinct texture pointers to prove independence.

**Cross-encoder merging:** `fuxigpumerge=1` merges admitted draws within one nonzero encoder identity. Perf bit 2 permits different nonzero identities while retaining the attachment/action/hazard checks. Zero/unknown identities still split. A successful boundary increments `cross_encoder_merges`. Because isolation and area/action checks can still reject every cross-encoder opportunity, bit 2 does not guarantee a nonzero count. The supplied zero count alone does not identify which resource or boundary blocked each potential merge. [Boundary and counter](../tools/fuxi-metal/Backend.hpp#L1110).

## Which images and layouts generate barriers

[prepareImages](../tools/fuxi-metal/Backend.hpp#L1249) prepares all bound sample slots and full descriptor mip ranges, even when the authored shader's actual access is narrower. Every used attachment is prepared in GENERAL. A merged run already hoists/deduplicates these transitions by image/mip before beginning the pass; without merging, each draw repeats its preparation.

| Resource/use | Recorder behavior |
| --- | --- |
| IOSurface layer sampled by CA | Import creates a defined, linear PREINITIALIZED image in the original host allocation. First sampling changes it to SHADER_READ_ONLY_OPTIMAL; later read-only reuse is skipped by bit 1. A layer produced by a render or copy changes from GENERAL to SHADER_READ_ONLY_OPTIMAL. Host cache/lease handoff remains separate. [Import](../tools/fuxi-metal/Backend.hpp#L486), [frontend import](../tools/fuxi-metal/Frontend.mm#L1791). |
| Scanout/composite target | Uses GENERAL for attachment initial/final layouts, including ordered attachment fetch. Previously every new run could emit GENERAL → GENERAL again. There is no Vulkan swapchain/PRESENT transition in this backend: the target is an imported IOSurface, retired before its lease is released. [Render pass](../tools/fuxi-metal/Backend.hpp#L666), [lease release](../tools/fuxi-metal/Frontend.mm#L1410). |
| Backdrop/image copy | Prepares both source and destination in GENERAL, then copies. Thus a sampled source can change SHADER_READ_ONLY_OPTIMAL → GENERAL, while a fresh destination changes UNDEFINED → GENERAL. Later sampling of the result changes GENERAL → SHADER_READ_ONLY_OPTIMAL. Frontend records the image copy as an intervening operation. [Copy recording](../tools/fuxi-metal/Frontend.mm#L1515), [backend copy/snapshot](../tools/fuxi-metal/Backend.hpp#L1385). |
| Blur intermediate | Rendering writes GENERAL; the next pass sampling it requires SHADER_READ_ONLY_OPTIMAL. These actual layout changes and producer/consumer ordering remain. The host fixture tests a sampled intermediate, not CA's proprietary blur shader. |
| Linear-buffer sample shadow | If direct linear sampling cannot be supported, a read-only optimal shadow is allocated. Each run refreshes it from the backing buffer in TRANSFER_DST_OPTIMAL, then transitions it to SHADER_READ_ONLY_OPTIMAL. Refresh deduplication is run-local because CPU/GPU backing changes must be visible. [Fallback](../tools/fuxi-metal/Backend.hpp#L403), [refresh](../tools/fuxi-metal/Backend.hpp#L1235). |
| Upload, readback and clear | Existing code uses GENERAL for buffer/image transfers and full image clears, rather than TRANSFER_SRC/DST_OPTIMAL. Partial writes to an undefined destination first clear it and insert a transfer write dependency. Aliased image copies snapshot through a buffer, retaining a transfer read/write dependency. [Transfer paths](../tools/fuxi-metal/Backend.hpp#L1385). |
| Mip generation/blits | All mips in the generation range are prepared in GENERAL, one image barrier structure per level if needed. Each blit then inserts a transfer write → read/write dependency before the next level. Later sampling transitions every mip in the descriptor view to SHADER_READ_ONLY_OPTIMAL. [Frontend generation](../tools/fuxi-metal/Frontend.mm#L1551), [backend blits](../tools/fuxi-metal/Backend.hpp#L1274). |

This explains why image barriers do not track draw count one-for-one: split runs repeat target preparation, bound sample views may cover multiple images/mips, and intervening copies, shadows, uploads and mip generation add their own transitions. With mask 3, read-only reuse is already skipped, leaving repeated GENERAL barriers and real changes as major remaining categories. The aggregate device trace cannot give an exact per-resource breakdown.

## Opt-in changes and synchronization argument

The parser now accepts exact, single-digit masks 0–7; duplicates, malformed values and unknown bits still disable the mask. [TranslationPolicy.hpp](../tools/fuxi-metal/TranslationPolicy.hpp#L14):

- Value **1**: existing read-only same-layout barrier skip.
- Value **2**: existing cross-encoder merge permission; requires `fuxigpumerge=1`.
- Value **4**: batch independent boundary image transitions, omit redundant GENERAL/read-only same-layout transitions, and merge compatible differing render areas. Area merging requires `fuxigpumerge=1`; barrier reductions also work without merging.

Use `fuxigpuperf=7` to add the new behavior to mask 3. Bit 4 is off by default. Existing masks retain their recording policy.

Omitting GENERAL → GENERAL does **not** mean omitting write dependencies. The entry HOST-write dependency covers flushed host writes, every prior run/non-draw retains its ALL_COMMANDS memory dependency, and render passes retain their external color/fetch dependencies. Same-layout reuse changes no image layout, so those dependencies already cover visibility and execution ordering, including transfer consumers, storage writers and aliases. Actual layout changes always keep their image barriers; shadow TRANSFER_DST transitions also retain theirs.

[Pending transitions](../tools/fuxi-metal/Backend.hpp#L1185) are flushed before each operation/run or shadow copy. Distinct images with disjoint memory, and disjoint mips of one image, may share one barrier call. Repeated subresources or images sharing memory/overlapping mapped ranges flush the previous batch first, preserving transition order. No batch is emitted inside a render pass. Recording rollback still restores layouts/defined bits through the existing texture snapshots; it never submits a partial recording.

Metal load=DontCare/store=DontCare already select `VK_ATTACHMENT_LOAD_OP_DONT_CARE`/`VK_ATTACHMENT_STORE_OP_DONT_CARE` in [renderPass](../tools/fuxi-metal/Backend.hpp#L670). No additional action substitution is needed or safe merely because an area changed. First load and last store remain the merged run's actions. Intermediate CLEAR, discard and undefined loads retain their existing checks.

## Linux verification

[GpuBarrierTests.cpp](../tools/fuxi-metal/GpuBarrierTests.cpp) runs the production backend through the existing authored GLSL adapter and a forwarding lavapipe proxy. [The runner](../tools/test-fuxi-metal-merge.py) compares all defined output pixels against the mask-3 control and the unmerged path, exercises masks 4/5/6/7, and asserts exact barrier/pass counts. The CA fixture has 12 layer draws, a backdrop copy, one sampled intermediate draw, then 12 further layer draws; alternating 16×16 and 8×8 areas make fragmentation observable.

Measured counts with merge/submit/overlap enabled in both runs:

| Fixture | Mask | Image structures | Calls with images | All barrier calls | Render passes |
| --- | --- | ---: | ---: | ---: | ---: |
| Five independent images | 3 | 5 | 5 | 8 | 1 |
| Five independent images | 7 | 5 | 1 | 4 | 1 |
| Three-mip sample | 3 | 4 | 4 | 7 | 1 |
| Three-mip sample | 7 | 3 | 1 | 4 | 1 |
| CA-like frame | 3 | 29 | 29 | 57 | 25 |
| CA-like frame | 7 | 4 | 3 | 9 | 3 |
| LOAD expansion/shrink | 3 | 3 | 3 | 8 | 3 |
| LOAD expansion/shrink | 7 | 0 | 0 | 3 | 1 |

The CA `area` split count falls 23 → 0; `non_draw` stays 2 → 2. These fixtures reuse prepared layer images; the counts are specific to their sequence, not a prediction for the device trace. Both mask variants produce byte-identical defined pixels. Partial CLEAR expansion, crossed rectangles, DontCare load expansion and smaller final discard keep two passes. Copy/upload hazards keep their two draw passes. The tests also cover failed recording rollback and two outstanding submissions.

Verification commands (supply an already installed GLSL compiler):

```sh
python3 tools/test-fuxi-metal-frontend.py
python3 tools/test-fuxi-metal-merge.py --glslang "$GLSLANG"
bash -n tools/ci/build.sh
bash tools/ci/build.sh metal --dry-run
git diff --check
```

The frontend runner executes policy tests under ASan/UBSan and explicitly skips native Objective-C tests on Linux. The merge runner builds with warnings as errors and ASan/UBSan, checks existing merge/shadow/fetch/storage/mip/variant pixel controls across all masks, tests the extension-hidden fallback, and runs host benchmarks without timing thresholds. [CI registration](../tools/ci/build.sh#L238) keeps CPU policy tests in the normal metal build and optionally runs the Vulkan suite when `FUXI_MTL_TEST_ICD` names a supplied local ICD (`FUXI_MTL_TEST_GLSLANG` supplies the compiler).

No macOS/iOS build, native Objective-C execution, Turnip/Adreno GPU timing, device/phone access or GMEM measurement was performed. Linux lavapipe checks validate recording counts and rendered behavior, not tiler performance. The DrawPlanCache planning block was left untouched.

## Bit 4 narrowing: GENERAL sampling, masks, post-pass skip, shadow refresh once per submit

Code only; device effect **not yet observed**. All of it is under the existing `fuxigpuperf` value 4 (already part of the device mask 7); no new bit or boot argument.

- New trace fields: `pipeline_barriers/frame` (actual `vkCmdPipelineBarrier` calls, including entry/final), `layout_transitions/frame=old>new:n` (und/gen/ro/dst), `post_barriers_skipped/frame`, `shadow_refresh_skipped/frame`. With bit 4, `read_barriers_skipped` also counts GENERAL same-layout skips.
- Sampled images stay `GENERAL` (the GENERAL <-> SHADER_READ_ONLY ping-pong disappears). Spec-valid for any sampled image.
- Image barriers use VERTEX|FRAGMENT|COLOR_ATTACHMENT_OUTPUT|TRANSFER stages (plus HOST in src) and write-only source / consumer-only destination accesses instead of ALL_COMMANDS and MEMORY_READ|WRITE. A batch whose targets are all TRANSFER_DST names only TRANSFER as destination. This backend has no compute stages. Render pass external dependencies are narrowed the same way.
- The global ALL_COMMANDS barrier after a draw pass is skipped when the pass has no storage descriptors: the render pass external dependency (COLOR_ATTACHMENT_WRITE to later shader/transfer/attachment accesses) already closes it, and WAR follows from its execution dependency. Transfer ops and storage-writing passes keep a (narrower) global barrier.
- Shadow refresh: one TRANSFER_DST barrier for every shadow in a run, then the copies. A shadow already refreshed earlier in the submit is not copied again unless the submit has a buffer destination, a storage descriptor, or a written texture overlapping its backing.
- Not done: the CPU snapshot of refused no-copy buffers (6.7 MB/frame) has no CPU mutation signal (see the chunk-compare regression in metal.md), so it still copies.
- Turnip evidence is from memory of `tu_cmd_buffer.cc` (`tu_barrier`, `vk2tu_access`, `tu_flush_for_access`; no Mesa source is available offline): MEMORY_READ|WRITE sets all flush/invalidate bits and ALL_COMMANDS forces wait-for-idle. Not verified against the pinned source.
