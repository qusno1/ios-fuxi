# FuxiMetal draw-plan rebasing

The frame trace reports `plan_hits/frame` and `plan_misses/frame` for planning
attempts. A failed local rebase counts as a miss and uses the existing full
translator/variant path. Translation trace mode bypasses both lookup and insertion.
Counters require frame tracing; disabled tracing adds no clocks or atomics.

## Source contract

Audited metal2vulkan revision `720ef1ce9ae2a2068999a8675717418b9416283e`
(`tools/ci/pins.env`), specifically these external source files:

- `crates/metal2vulkan/src/bind/state.rs`: `BoundBuffer::shape`, `length_class`,
  `offset_alignment` (maximum vertex alignment 4).
- `crates/metal2vulkan/src/bind/plan/slots.rs`: `plan_buffer`, descriptor view
  alignment/range limits, shape-dependent buffer variants.
- `crates/metal2vulkan/src/bind/graphics.rs`: planning, `instantiate`, `vk_draw`,
  vertex fetchability and push fields.
- `crates/metal2vulkan/src/bind/instantiate.rs`: `descriptor_range`, `bind_buffers`,
  `slot_field`.

The original Fuxi adapter in `tools/fuxi-metal2vulkan-bridge.rs` remains the ABI-8
boundary. Its optional `fuxi_m2v_draw_plan_v1` returns the same accepted output plus
rules derived from public pipeline interface slots, device limits and plan push
placements. It makes no second planner call. The external dependency is unchanged.
Older bridges, unknown push fields and unrepresentable metadata use exact-input
caching. Trace mode calls the original draw entrypoint.

The 32-entry key includes requested pipeline identity, every format/filter/LOD bit,
indexed/strip values and every buffer's presence, bytes/buffer kind, padded-length
bit class for buffers, and offset alignment class capped at 4. Bytes lengths do not
change the external shape. Descriptor offset residues modulo their view alignment
must also match the entry. These are conservative keys: no float arithmetic and no
resource contents or object identities. Requested/resolved pipeline invalidation
and serialization under the backend mutex are unchanged.

## Output and validation

Let `E = round_up(length,16)`, `A = max(min*BufferOffsetAlignment,16)`, and
`B = bind offset`. A descriptor's window and view limits come from its immutable
resolved pipeline. For the affine fast path:

| Output | Rebase |
| --- | --- |
| Bound-buffer descriptor offset, bounded window `[s,s+n)` | `align_down(B+s,A)` |
| Bound-buffer descriptor range, bounded window | `round_up(B+s+n,16) - descriptor offset` |
| Bound-buffer descriptor offset, whole window | `align_down(B,A)` |
| Bound-buffer descriptor range, whole window | `E - descriptor offset` |
| Neutral/blob/image/sampler descriptors; remaining descriptor fields | Copy cached bytes |
| `vertexOffsets` | Buffer slot's new offset; zero for inline bytes |
| `vertexCount`, `vertexBindings`, `vertexSlots` | Copy; `vertexCount` counts bindings, not vertices |
| `indexed`, descriptor/push counts, push placements/stages | Copy |
| `count` | New draw count |
| `first` | New start for primitives; zero for indexed draws |
| `indexOffset` | New index offset for indexed draws; zero otherwise |
| `SlotBindOffset` push | Low 32 bits of `B - descriptor offset` |
| `SlotGuestBase` push | New buffer offset, since ABI-8 guest GPU address is zero; zero if absent |
| `BaseVertexInstance` push | `{start,0}` for primitives, `{0,0}` for indexed draws |
| Other explicitly recognized constant pushes and unused output bytes | Copy cached bytes |

The fast path requires an unclipped window, at least 16 descriptor bytes, unchanged
descriptor alignment residue and a range within the uniform/storage view limit.
It falls back for clipped/negative windows or changed residues. The full translator
can accept such cases by clamping/widening the range and updating residual pushes;
local checks must never turn them into a rejection.

Every new input also repeats the adapter's cheap checks: count in 1..65536,
start at most `65536-count`, indexed start zero, UINT16 index offset even and
`count*2` bytes inside the index allocation. Every nonempty buffer must have
`offset < length <= 16 MiB`; inline bytes require offset zero and length at most
4096. Zero length means absent, so its other fields are ignored.

A Metal bind offset need not satisfy Vulkan descriptor offset alignment: the
translator aligns the descriptor base and pushes the residual. Fixed-function
vertex alignment can instead require a pulled-buffer variant, which is why its
alignment class stays in the key. The translator does not reject short descriptor
windows merely for being shorter than shader accesses: it pads/clamps them. Actual
vertex fetch bounds, index contents, resource aliasing and Vulkan resource state
remain checked by Backend after planning. Resource-write plans are already refused
by the adapter, so draw-time uniform/write aliasing cannot add a hidden validation
case to a successfully cached plan.

## Linux verification

`tools/test-fuxi-metal-draw-plans.py --translator <built-bridge>` runs sanitizer
unit controls, a deterministic randomized/interleaved differential stream against
the full external translator, and unsanitized `SubmitBench` A/B/B/A measurements.
The differential stream varies lengths, offsets, counts, starts, index bounds,
bytes classes, presence, formats, filters and LOD bits, and includes a real
pulled-vertex variant. Every accepted output is compared with `memcmp`; every
accept/reject result must agree. Boundary cases include unaligned uniforms,
clipping, padding/length-class transitions, invalid unused buffers and overflows.
Synthetic controls additionally exercise guest-address and draw-base pushes.

The ring benchmark uses 120 draws/frame, alternating textures and indexed/direct
calls, three streamed buffer bindings, rotating 256-byte offsets, counts 3/4 and
starts 0/4. `PIPE_BENCH_EXACT=1` disables the optional extension to measure the
original exact-input cache. Both modes check final pixels and deferred pipeline /
resource lifetime. The trace control must report zero hits. Timing excludes GPU
wait and warmup; no timing threshold is imposed. These are Linux/lavapipe tests
with authored AIR, not a macOS build or a CoreAnimation device measurement.

Verified on Linux with an optimized Rust 1.98.0 bridge and `clang++ -O2` benchmark,
using lavapipe and the authored M7 AIR control. The A/B/B/A run measured:

| Cache | Median pipe ms/frame | Hits/frame | Misses/frame |
| --- | --- | --- | --- |
| Exact A | 1.307 | 0 | 120 |
| Shape B | 0.051 | 120 | 0 |
| Shape B | 0.027 | 120 | 0 |
| Exact A | 1.840 | 0 | 120 |

The differential test checked 29,744 draws (27,392 accepted, 2,352 rejected),
including 2,000 draws of the pulled-vertex variant, with no acceptance or byte
mismatches. Its deliberately irregular stream had 25,244 hits / 4,500 misses
(84.87%). Trace bypass reported 0 hits / 120 misses per frame. C++ ASan/UBSan
unit/differential tests, existing Linux frontend models, frame-trace reuse controls,
and the merge suite (including variant invalidation and all barrier/perf masks)
passed. Bridge Rust tests: 10 passed, one existing manual benchmark ignored.
Native Objective-C tests were skipped because they require macOS; no macOS build
or device timing was performed.
