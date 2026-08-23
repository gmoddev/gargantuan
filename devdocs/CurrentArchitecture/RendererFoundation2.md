---
status: accepted
date: 2026-08-22
owner: rendering
implemented_slice: production dirty accumulator, publication contract, persistent SDL projection, EditorViewport, tests, benchmarks
runtime_status: RenderPublication is the supported runtime and EditorViewport path; RenderSnapshot remains compatibility scaffolding
related_code:
  - include/gargantuan/render/RenderDirtyAccumulator.hpp
  - include/gargantuan/render/RenderPublication.hpp
  - include/gargantuan/render/RenderExtractor.hpp
  - include/gargantuan/render/RenderProjection.hpp
  - src/render/RenderDirtyAccumulator.cpp
  - src/render/RenderPublisher.cpp
  - src/render/RenderProjection.cpp
  - tests/RendererPublicationTests.cpp
  - tests/RendererFoundation2Benchmark.cpp
---

# Renderer Foundation 2

> **Foundation 2C final decision (2026-08-22):** the temporary Decision C below
> is resolved by equivalent real GPU deformable, GUI, texture, and mixed tests
> on AMD and NVIDIA hardware. The accepted result is **Decision A — keep SDL
> GPU**, and **GUI START: YES**. See
> [Renderer Foundation 2C](RendererFoundation2C.md) for the final evidence,
> maintenance comparison, and exact next milestone. This document retains the
> Foundation 2/2B rationale as history.

## Decision

**C. RETAIN SDL temporarily, implement Foundation 2 publication first, and defer backend migration until the supported renderer consumes the new publication and visually equivalent deformable/GUI GPU workloads can be measured.**

The long-term semantic boundary is an immutable per-frame `RenderPublication`,
not direct renderer mutation and not a complete `RenderSnapshot` forever:

```text
authoritative DataModel
    -> committed changes / render dirtiness on Main
    -> RenderPublisher (validate, classify, coalesce, bound)
    -> shared_ptr<const RenderPublication>
       |- frame-global camera/light/viewport/DPI
       |- object Creates / Updates / Removes
       |- persistent mesh lifecycle and dirty vertex ranges
       `- transient renderer-neutral UI batches
    -> renderer-owned disposable projection
    -> SDL GPU today, replaceable backend later
```

Foundation 2B makes this the production path. `Engine`, `BaseRenderer`, SDL
passes, `HeadlessRenderer`, and EditorViewport consume immutable publications
and retain disposable projections. `RenderSnapshot` remains only for full-state
extraction, compatibility tests, and the experimental Filament adapter while it
is migrated; it is no longer the normal supported runtime publication.

## What limited Foundation 1

The old comparison was limited primarily by publication architecture, not GPU
object mutation. At 50,000 mostly-static Parts, Foundation 1 measured about
14.50 ms constructing the complete snapshot and 7.94 ms scanning it in
Filament, while applying the actual 500 changed objects took 0.08 ms. Static,
mostly-static, and dynamic scenes all paid the complete extraction and
reconciliation cost. Filament's persistent objects could not recover that CPU
time, even though its reported GPU time was low.

A complete snapshot remains correct and useful for recovery, capture, tests,
and small worlds. It is not acceptable as the only steady-state publication
path at the intended 50,000-object scale. It performs traversal, validation,
matrix construction, sorting, allocation/copy, and backend reconciliation for
unchanged objects.

## Publication semantics

`RenderPublication` is immutable shared ownership. `Id` is a session-local
monotonic sequence. Incremental publications name the exact `BaseId` they
extend; a stale, duplicate, skipped, or out-of-order base is rejected before
semantic mutation. A full resync has `FullResync=true` and is independently
reconstructable.

Lifecycle operations are deterministic and coalesced once per object per
publication. Their semantic order is:

1. validate the complete candidate;
2. remove stale object and mesh generations;
3. create new generations;
4. apply classified updates;
5. replace frame-global and transient UI state.

Updates name one or more semantic domains: transform, material, visibility,
geometry, deformable vertices, or hierarchy/render presence. Authoritative
commits feed one engine-side `RenderDirtyAccumulator`; renderers do not
subscribe to properties and do not read `ChangeJournal`. The accumulator keeps
only stable `ObjectId`, domain flags, estimates, versions, and bounded
diagnostics. It never retains an `Instance*`.

Each object occupies at most one accumulator entry. Repeated writes union their
domains and publication rereads the final authoritative value once. Create plus
updates becomes one create with final values; create plus destroy disappears;
an existing update plus destroy becomes one remove. Removal and recreation use
distinct generational identities, so the old remove and new create cannot be
confused. Irrelevant properties do not create an entry. Ordered `ObjectId`
storage makes publication deterministic.

Capture is non-destructive. A publisher acknowledges an exact captured version
only after it has validated and constructed the complete immutable candidate.
Failure leaves the dirty state available for retry. Separate consumer cursors
let the runtime renderer and EditorViewport observe the same authoritative play
world without starving one another; entries are purged only after every live
consumer acknowledges them.

The general journal retains its independent 4,096-record history bound, but its
raw record volume no longer determines render overflow. This resolves the
Foundation 2 fully dynamic journal-overrun limitation without increasing that
unrelated bound.

## Accumulator and publication bounds

The default hard limits are semantic and explicit:

| Resource | Limit |
| --- | ---: |
| Live accumulation scopes | 64 |
| Live publication consumers | 64 |
| Distinct dirty render identities per scope | 131,072 |
| Estimated deformable dirty bytes per scope | 32 MiB |
| Pending UI bytes | 32 MiB |
| One immutable publication | 64 MiB |
| Retained accumulator diagnostics per scope | 8 |

Deformable accounting tracks the final dirty estimate rather than summing
redundant writes. UI and publication sizes are checked from their immutable
payloads. Crossing a hard bound records one of a bounded number of diagnostics,
marks the consumer for full resync, and refuses to publish a partial delta. The
next viable frame rebuilds from authoritative state; the renderer projection is
never used as recovery authority.

## Full resync and failure

Full resync is required for initialization, backend restart, later device-loss
recovery, Studio viewport recreation, explicit debug comparison, and the
exceptional accumulator or publication hard-limit fallback.
It is generated from authoritative world state, not from the renderer's
projection. A new renderer may accept a full resync regardless of its prior
publication ID and reconstruct all disposable state.

Invalid IDs, duplicate operations, stale updates/removes, revision regressions,
out-of-range indices/dirty ranges, invalid clipping, and non-finite geometry are
rejected. Invalid input never grants mutation authority. GPU allocation failure
may discard the affected renderer projection and request a new full resync; it
must not mutate or repair DataModel state.

## Deformable geometry

Gargantuan is required to support rigid bodies, kinematic controllers,
constraints, cloth, and deformable/soft bodies. Engine/render boundaries must
not assume `static mesh + rigid transform`.

The renderer-facing model separates:

- generation-safe `RenderMeshIdentity` from engine object identity;
- stable `TopologyRevision`, indices, and initial vertices;
- monotonically increasing `VertexRevision`;
- `FirstVertex` plus an immutable contiguous dirty vertex range;
- positions, normals, tangents, UVs, and updated bounds;
- object-to-mesh binding from physics or solver ownership.

Renderer-owned vertex/index buffers persist across updates. Stable topology is
uploaded once. Dirty vertex ranges update only the corresponding GPU buffer
region. A topology change is represented as mesh lifecycle replacement, not a
misclassified vertex update. Indices and vertex ranges are bounded and checked;
NaN/Inf data is rejected before projection mutation.

Physics owns simulation and solver-native structures. The engine's extraction
or publication layer converts committed visible results into render vertices.
The renderer owns GPU resources. Neither side knows whether vertices came from
Box3D, a Box3D extension, a dedicated cloth solver, or another future backend.
Soft-body physics was not implemented by this renderer milestone. The later
[Soft-body Physics Foundation 1](SoftBodyPhysicsFoundation.md) implements the
first CPU XPBD cloth/rubber producer behind this unchanged contract.

Particles use the same principle but are expected to publish large transient or
instanced sets rather than thousands of persistent semantic objects.

## GUI boundary

GUI classes never call SDL GPU. The intended flow is:

```text
GUI semantic tree and properties
    -> engine-owned layout, text shaping, clipping and visual extraction
    -> RenderUiFrame / ordered RenderUiBatch values
    -> renderer UI pass
```

A UI frame carries viewport pixels and DPI scale. Each batch carries texture
identity, optional scissor rectangle, layer, opacity, vertices, and indices.
Quad, image, border, and glyph geometry can share this representation. Layout,
text semantics, font fallback, hit testing, and semantic z-order belong above
the renderer. Atlas residency, pipeline choice, buffer allocation, scissor
commands, blend state, and draw submission belong to the renderer.

This is deliberately closer to backend-neutral render primitives than to
high-level UI objects. It keeps SDL/Filament/bgfx/WebGPU types out of GUI
classes while permitting batching, clipping, transparency, frequent updates,
and high-DPI Studio/runtime presentation. Rounded corners and borders can be
expressed later through geometry or a small semantic material/effect set; they
do not require a general material graph.

## Material and texture boundary

The existing `RenderItem::Color` is sufficient only for primitive solid color.
`RenderMaterialState` establishes the next bounded semantic shape: revision,
base-color factor and texture, optional normal texture, metallic, roughness,
opacity mode, and alpha cutoff. `RenderTextureIdentity` is generation safe.
This supports textured rigid/deformable meshes and the required transparency
classification without exposing shaders or building a material graph.

Texture asset resolution and semantic material values belong to the engine and
asset system. Device texture residency, samplers, pipelines, descriptors, and
upload lifetime belong to the backend. UI texture/atlas references use the same
identity principle but remain in UI batches so GUI batching is explicit.

## Foundation 2 Release evidence

Command, from a matching Visual Studio developer environment:

```powershell
cmake --build build-release-native --target gargantuan_renderer_foundation2_benchmark gargantuan_renderer_publication_tests -j 4
build-release-native/gargantuan_renderer_publication_tests.exe
build-release-native/gargantuan_renderer_foundation2_benchmark.exe 5
```

The following are five-frame CPU means in milliseconds on the local Windows x64
Release build. `Accumulate` includes the authoritative property setters, normal
journal bookkeeping, and render-dirty accumulation; it is not a standalone
accumulator microbenchmark. `Generate` and `Apply` isolate publication
construction and CPU projection application. The table does not claim GPU time
or renderer memory.

| Workload | Elements | Accumulate | Generate | Apply | Published operations | Upload bytes | Full resyncs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Static rigid | 1,000 | 0.0000 | 0.0005 | 0.0005 | 0 | 0 | 0 |
| Static rigid | 10,000 | 0.0000 | 0.0012 | 0.0008 | 0 | 0 | 0 |
| Static rigid | 50,000 | 0.0000 | 0.0024 | 0.0016 | 0 | 0 | 0 |
| Mostly static rigid | 50,000 | 1.0704 | 0.5055 | 0.1199 | 2,500 updates | 0 | 0 |
| Fully dynamic rigid | 50,000 | 96.5560 | 51.6781 | 12.9927 | 250,000 updates | 0 | 0 |
| Fully dynamic, 4 writes/object | 50,000 | 165.0967 | 49.1045 | 12.5947 | 250,000 updates | 0 | 0 |
| Deformable | 4,096 vertices | 0.0000 | 0.0563 | 0.0135 | 5 updates | 983,040 | 0 |
| Deformable | 16,384 vertices | 0.0000 | 0.2269 | 0.0476 | 5 updates | 3,932,160 | 0 |
| Deformable | 65,536 vertices | 0.0000 | 1.2013 | 0.2044 | 5 updates | 15,728,640 | 0 |
| GUI | 10,000 quads | 0.0000 | 0.4265 | 0.2273 | 40 batches/frame | 7,600,000 | 0 |
| Mixed | 50k rigid + 16k deformable + 5k GUI | 0.0000 | 0.5173 | 0.2219 | 2,505 updates + 20 batches/frame | 7,732,160 | 0 |

The prior Foundation 2 prototype fully rebuilt the 50,000-object dynamic scene
on every measured frame: about 23.03 ms generation, 13.67 ms apply, and ten
full resyncs in ten frames. Foundation 2B publishes exactly 50,000 coalesced
updates per frame with zero measured resyncs. Four transform writes per object
produce the same update count and essentially the same generation/apply time as
one write; only the deliberately measured mutation/accumulation phase grows.

This removes the correctness/performance cliff caused by journal retention,
but it does not make the worst-case CPU path faster than the old rebuild on this
machine: the fully dynamic publication and apply means are about 64.67 ms
combined. The static and 1%-dirty paths retain the intended O(dirty) behavior.
The remaining dynamic CPU cost is now visible as extraction/allocation and
per-object value work rather than hidden by recovery, and should be optimized
only with representative GPU workload evidence.

Deformable and GUI byte counters are exact semantic vertex/index payload bytes.
SDL now consumes deformable creates and dirty ranges through persistent buffers,
but this CPU benchmark still does not claim device timing or actual GPU transfer
counters.

## Backend evaluation after publication redesign

SDL GPU remains the supported backend. It owns a persistent `RenderProjection`
and primitive/dynamic mesh caches. Transform, material, and visibility changes
update object state without recreating mesh resources. Dynamic topology creates
persistent vertex/index buffers; stable-topology deformable changes upload only
the validated contiguous vertex range. Full resync discards and reconstructs
this disposable state. Instance batching/culling, texture residency, the actual
UI GPU pass, and trustworthy GPU timestamps/counters remain to be built. See the
official
[SDL GPU API](https://wiki.libsdl.org/SDL3/CategoryGPU).

Filament remains a credible mature PBR renderer across desktop, mobile, and
WASM, but the existing experiment used only rigid primitives and the old full
publication. Its feature advantage is real; its measured Gargantuan CPU result
did not justify migration. Re-test only after an adapter consumes
`RenderPublication` directly and implements equivalent dynamic mesh and UI
workloads. See the official [Filament documentation](https://google.github.io/filament/).

bgfx is a credible lower-level alternative. It supports multiple native
backends, compute, indirect drawing, and transient buffers intended for UI and
other per-frame geometry. It does not supply Gargantuan's scene, material,
lighting, GUI semantics, or publication model, so adopting it now would move
API/toolchain maintenance without removing the work Foundation 2 identifies.
See the official [bgfx documentation](https://bkaradzic.github.io/bgfx/).

WebGPU through Dawn/wgpu is architecturally clean and offers explicit buffers,
pipelines, copies, render bundles, and compute. Like SDL GPU and bgfx, it is an
RHI rather than a ready-made engine. There is no measured Gargantuan benefit to
pay for a migration today. See the [WebGPU specification](https://gpuweb.github.io/gpuweb/).

A custom Vulkan/D3D12 RHI is rejected. No evidence justifies duplicating
portable device, synchronization, shader, swapchain, and tooling work.

## EditorViewport and next milestone

EditorViewport uses its own accumulator consumer and publisher against the Edit
workspace or exact active Play workspace. Creation and recreation request a
deterministic full resync; subsequent document/render changes are incremental.
Resize updates frame-global publication state and recreates only viewport GPU
targets. Capture applies the same publication/projection/dynamic-mesh path as
runtime, and picking reads the projected immutable values associated with the
displayed frame. No Studio-only property subscription or renderer authority was
introduced.

The blocker before real GUI rendering is now the renderer-neutral UI producer
and resource-residency boundary: layout/text shaping must emit bounded batches,
texture/atlas identities need an engine asset contract, and SDL needs a real UI
pipeline with clipping, blending, and transient buffer management. The current
production path validates and retains already-defined UI batches, but it does
not yet turn `GuiObject` semantics into them or draw them.

Decision C remains in force. Reevaluate Filament or another backend only after
SDL and the alternative consume this same publication and run visually
equivalent rigid, deformable, UI, and mixed GPU workloads with measured
publication, apply, upload, submission, GPU time, draw count, and memory.
