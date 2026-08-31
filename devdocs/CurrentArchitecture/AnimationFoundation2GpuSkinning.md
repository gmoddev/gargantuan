---
status: current
owner: runtime-animation
last_verified: 2026-08-29
related_code:
  - assets/shaders/skinning.glsl
  - include/gargantuan/animation/AnimationRuntime.hpp
  - include/gargantuan/render/RenderPublication.hpp
  - include/gargantuan/render/Renderer.hpp
  - include/gargantuan/render/SDLRenderer.hpp
  - src/animation/AnimationRuntime.cpp
  - src/render/RenderProjection.cpp
  - src/render/RenderPublisher.cpp
  - src/render/Renderer.cpp
  - src/render/SkinPaletteCache.cpp
  - src/render/passes/OpaquePass.cpp
  - src/render/passes/ShadowPass.cpp
  - tests/AnimationFoundationTests.cpp
  - tests/AnimationFoundationBenchmark.cpp
  - tests/RendererFoundation2GpuBenchmark.cpp
  - tests/FirstCompleteGameGpuTests.cpp
---

# Animation Foundation 2A: GPU skinning

## Outcome and authority boundary

Foundation 1 made CPU vertex deformation the production path. With a 999-vertex,
64-joint fixture that cost about 3.34 ms of the 6.26 ms 100-rig frame and 16.78
ms of the 32.66 ms 500-rig frame. It also required a transient posed mesh and a
full dynamic vertex publication for every active rig every frame. That linear
CPU vertex cliff was the reason for Foundation 2A.

The production path is now:

```text
Animator / AnimationTrack
    -> CPU time, sampling, blending, and hierarchy solve
    -> final position and normal palette
    -> immutable RenderPublication
    -> one shared static skinned source mesh + one bounded palette per rig
    -> opaque and shadow vertex shaders
```

Animation authority did not move into rendering. The renderer does not advance
time, load clips, interpolate keys, invoke Luau, inspect Animator Instances, or
mutate the DataModel. `RenderUpdateDomain::AnimationPose` now means a semantic
rig palette update; it does not imply dynamic vertex replacement.

[Foundation 2B semantic anchors](AnimationFoundation2SemanticAnchors.md) are the
second consumer of the same CPU joint model transforms. They do not consume
this GPU palette, a framebuffer, or skinned vertices, and renderer capability
cannot affect gameplay-space Attachment results.

## Renderer-neutral contract

`RenderMeshCreate` carries immutable positions, normals, tangents, UVs, four
`uint16` joint indices, four float weights, the stable skeleton compatibility
identity, and the exact joint count. Source geometry is keyed by
`RenderMeshIdentity`; per-rig pose is keyed separately by `ObjectId`, so many
rigs can share one source resource without sharing a pose revision.

`RenderAnimationPoseUpdate` carries the source mesh, optional CPU-fallback posed
mesh, monotonic pose revision, mode, and `RenderSkinPalette`. The palette owns a
stable skeleton identity and immutable entries. Each entry is exactly:

```text
mat4 PositionMatrix = JointModel * InverseBindMatrix
mat4 NormalMatrix   = inverse-transpose(linear(PositionMatrix))
```

The 128-byte entry has a compile-time size assertion. A palette is nonempty,
matches the source mesh's exact skeleton identity and joint count, and never
exceeds 256 joints. The contract contains no SDL, Vulkan, D3D, descriptor, or
shader handle.

Projection validates the entire publication before mutating projected state. It
rejects missing source residency, invalid object binding, duplicate operations,
stale revisions, incompatible skeletons, count mismatch, more than 256 entries,
non-finite matrices, malformed integer joint indices, and weights that are
negative or do not sum to one. Mesh creation already proves every joint index is
below the exact skeleton count; the palette must have that exact count, so a
shared source mesh is not rescanned once per rig on every pose update.

## Capability selection, headless behavior, and CPU fallback

`RendererCapabilities` is internal runtime state with `Graphical` and
`GpuSkinning` flags. The SDL GPU renderer reports `{true, true}`. A generic
graphical backend reports `{true, false}` unless it opts in. The headless
renderer reports `{false, false}`.

The exact CPU fallback condition is:

```text
Graphical && !GpuSkinning
```

Therefore normal SDL graphical execution uses palette-only GPU skinning. An
unsupported graphical backend uses the retained pooled posed-mesh path.
[Foundation 2C](AnimationFoundation2UpdatePolicy.md) advances every headless
track but samples, blends, solves the hierarchy, and generates a renderer-neutral
palette only when a native semantic consumer or explicit pose request requires
it. Headless visual-only rigs initialize no GPU and generate no redundant skin
matrices. Native correctness tests and explicit consumers can still call the CPU
reference implementation when they need vertex results. Gameplay uses the same
Animator and AnimationTrack API in all three cases.

CPU skinning remains the deterministic oracle. It consumes the same final
position/normal entries as the shader and preserves source tangent handedness.
No CPU fallback output pool is allocated in normal GPU or headless execution.

## SDL GPU resources and uploads

The SDL backend converts the neutral source once to this backend vertex layout:

```text
float3 position
float3 normal
float4 tangent
float2 UV
uint16x4 joints
float4 weights
```

Rigid and CPU-fallback vertices use joint zero, weight one, and one shared
identity palette, allowing the existing opaque and shadow pipelines to serve
both rigid and skinned meshes. A canonical source mesh identity has one vertex
buffer and one index buffer regardless of rig count.

The palette mechanism is a vertex-stage readonly storage buffer. SDL creates
one exact-size `SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ` buffer and one
same-size upload transfer buffer per GPU-skinned rig. Those resources persist
until pose removal, full resync, recovery, or renderer destruction. Updating a
pose maps the retained transfer buffer and submits a buffer upload with cycling
enabled after the first revision, so in-flight contents can be renamed by SDL
without application-level create/destroy churn. Uploads for one publication are
batched into one copy pass. Foundation 2A intentionally does not add a palette
atlas or crowd-instancing scheme.

After warmup the measured path has zero vertex/index/transfer/palette-buffer,
pipeline, shader, or texture creation; zero buffer reallocation; and zero
palette scratch growth. It performs one palette upload per changed rig and draw
submission. The backend records GPU/CPU rig counts, palette uploads and bytes,
fallback transitions, stale-pose drops, source-resource creation, CPU-skinned
uploads, and main/shadow mismatches without logging per frame.

## Matrix, scale, tangent, and owner-transform semantics

GLM stores column-major matrices and multiplies column vectors. The two `mat4`
values are copied byte-for-byte to a `std140` storage-buffer entry; GLSL uses the
same column-major convention. The 2026-08-29 `glslc` build validated the SPIR-V
layout, and the rendered differential catches transposition and multiplication-
order regressions.

Each joint's inverse-transpose linear transform is computed by the authoritative
runtime, rather than making every backend rebuild it or incorrectly using the
position matrix for normals. The shader blends position, normal, and tangent
contributions from up to four joints, normalizes normal/tangent results, and
preserves tangent `w`. Current material shading does not yet consume the tangent,
but the vertex contract and skinning result preserve it for tangent-space
materials.

The shader produces rig model-space vertices. `MeshPart` model/world transform
is then applied exactly once, and its inverse-transpose transforms the already
skinned normal. Differential fixtures include a rotated, non-uniform owner
transform and non-uniform joint scales.

The storage buffer is exact-size, and projection proves every source index is
within that count. The shader also checks each joint against the runtime storage-
array length before indexing. Malformed state cannot reach undefined out-of-
bounds GPU access.

## Opaque/shadow coherence, lighting, and draw calls

Opaque and shadow passes query the same per-rig palette cache during one command
buffer. The shadow pass records the resolved pose revision; the opaque pass
requires the same revision when that object casts a directional shadow. A
mismatch increments a bounded diagnostic counter and fails the frame rather
than rendering two poses. A palette is uploaded once even when both passes use
it.

Skinned output enters the ordinary model, camera, environment, material, fog,
exposure, and shadow paths. There are no animation-specific lighting constants.
GPU skinning does not solve crowd draw calls: one rig/material still means one
main draw, plus one shadow draw when enabled.

For 100 64-joint rigs at 1280x720, 60 fenced Vulkan frames measured 1.3364
ms/frame and 101 draws/frame without rig shadows. Enabling all rig shadows used
the same 819,200-byte palette update, produced 201 draws/frame, and measured
1.4314 ms/frame. The 7.1% observed frame increase is duplicate vertex/draw work,
not another palette evaluation or upload.

## Restart, resource failure, and reimport

GPU resources are disposable derived state. A fresh renderer/full resync
recreates the shared source and exact palette buffers from canonical mesh data
and the current semantic pose. AnimationTrack time and pose revision continue;
there is no asset reimport, Animator mutation, authored journal entry, or bind-
pose interlude.

Foundation 2A follows the existing renderer failure policy: a runtime palette
allocation/upload failure invokes renderer resource recovery and requires the
next publication to be a complete resync. It does not silently render bind pose
and does not switch animation authority mid-session. Tests inject palette
upload, shader creation, and pipeline creation failures, stale pose state, and
source/palette recreation. The palette failure releases partial resources and a
subsequent complete publication reconstructs the current pose.

Compatible mesh reimport can replace canonical source geometry at a revision
boundary while retaining the same skeleton contract. An incompatible skeleton
retires the pose with a bounded diagnostic before an old layout can be bound to
a new palette. Active tracks continue to retain the immutable clip revision
captured when loaded.

## Correctness and application proof

The deterministic GPU differential renders the same source through CPU fallback
and GPU palette paths. Across eight generated poses with one, two, and four
influences, translation, rotation, scale, non-uniform scale, and a rotated/non-
uniform owner transform, the Release Vulkan run reported:

```text
mean channel difference       0
maximum channel difference    0
mismatched pixel fraction     0
normal-palette gate           PASS
owner-transform gate          PASS
```

FirstCompleteGame's `AnimatedBeacon` is the application fixture. A canonical
`BeaconRoot/BeaconTip` Attachment now carries a quiet positional Sound and a
ProximityPrompt. The
GPU proof builds and validates a Release package, deletes the source project,
loads the relocated packaged world, and runs its ordinary Engine and SDL
renderer. It compares the animated Attachment against the same joint model
transform and GPU-palette socket. Twelve steady frames produced 12 palette uploads (3,072 bytes), one
skinned source resource, zero CPU-skinned vertex uploads, no resource growth,
and no main/shadow mismatch. Replacing the renderer mid-clip consumed a current
full-resync pose while the semantic revision continued. The normal packaging and headless tests separately
assert that the package publishes a semantic palette with no posed mesh or
dynamic vertex update.

## Release performance evidence

These are Windows x64 Release observations from 2026-08-29 on the primary
Ryzen 9 7950X3D / RX 7900 XT machine. They are evidence, not portable budgets.
The actual graphical backend selected `vulkan`. Renderer values use a 1280x720
offscreen color target, 10 warmup frames, 60 measured frames, and an explicit
fence each frame. SDL GPU timestamp queries are not exposed by this path, so the
reported GPU-side proxy is fence-completion latency, not fabricated timestamps.

### CPU animation/runtime path

The synthetic rig uses 999 source vertices. `cpuSkinMs` was exactly zero in all
production-palette scenarios.

| 64-joint rigs | Total ms | Sample/blend | Hierarchy | Palette matrices | Publisher | Projection |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.0125 | 0.0019 | 0.0039 | 0.0023 | 0.0024 | 0.0015 |
| 10 | 0.1211 | 0.0210 | 0.0390 | 0.0247 | 0.0215 | 0.0100 |
| 100 | 1.1366 | 0.1998 | 0.3863 | 0.2364 | 0.1763 | 0.0894 |
| 500 | 5.8386 | 1.0059 | 1.9145 | 1.2033 | 0.9309 | 0.5097 |

For 10 64-joint rigs, 1/2/4 tracks measured 0.1156/0.1212/0.1340 ms.
For 10 rigs, 16/64/128/256 joints measured 0.0417/0.1132/0.2053/0.3956
ms. This shows that sampling/hierarchy/palette generation, not vertex count,
is now the animation-owned CPU cost.

The retained CPU oracle measured 0.0428/0.4395/2.1737/4.3822 ms for
1K/10K/50K/100K vertices. Those costs are absent from normal GPU and headless
frames and remain useful for fallback capacity planning.

### Fenced SDL/Vulkan renderer path

This benchmark uses one shared 1,024-vertex/5,766-index source. Publication
construction and Animator evaluation are deliberately outside this table so
projection, palette transfer, submission, and fence wait remain distinguishable.

| 64-joint rigs | Frame mean / p50 / p95 ms | Projection | Palette-transfer stage | Submission | Fence wait | Palette bytes/frame | Draws/frame |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.1283 / 0.1148 / 0.1731 | 0.0032 | 0.0242 | 0.0319 | 0.0678 | 8,192 | 2 |
| 10 | 0.3096 / 0.2548 / 0.7714 | 0.0103 | 0.0353 | 0.0403 | 0.2223 | 81,920 | 11 |
| 100 | 1.3364 / 1.2478 / 1.9229 | 0.0876 | 0.1043 | 0.0857 | 1.0558 | 819,200 | 101 |
| 500 | 6.1755 / 5.6022 / 10.2367 | 0.4866 | 0.6732 | 0.3341 | 4.6705 | 4,096,000 | 501 |

All four rows passed exact upload, one-source, stable-resource, zero-CPU-upload,
zero-allocation, and coherence gates. Queued-mode submission means were
0.0436/0.0510/0.1089/0.4022 ms, but buffer cycling can move GPU back-pressure
into later transfer calls; fenced results are the authoritative completion
measure.

At 10 rigs the bone/palette matrix was:

| Joints | Runtime total ms | Fenced renderer mean ms | Bytes/rig/frame | Batched transfer stage ms |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 0.0417 | 0.3001 | 2,048 | 0.0349 |
| 64 | 0.1132 | 0.3151 | 8,192 | 0.0352 |
| 128 | 0.2053 | 0.3265 | 16,384 | 0.0388 |
| 256 | 0.3956 | 0.4563 | 32,768 | 0.0425 |

The same-workload Foundation 1 to 2A runtime comparison is 6.2634 to 1.1366
ms at 100 rigs (81.9% lower) and 32.6575 to 5.8386 ms at 500 rigs (82.1%
lower). This is the removal of CPU vertex deformation and dynamic posed-mesh
publication; it is not a claim that the GPU vertex shader alone created the
whole improvement. The separate renderer table includes actual upload, draw,
and fence costs.

The 50,000-static-Part plus one-rig regression published zero static updates,
one pose, zero dynamic vertices, and no full resync; publisher time was 0.0601
ms. Its real Vulkan frame measured 22.4291 ms, dominated by 50,002 draws and
20.0295 ms of CPU submission, while palette upload remained 8,192 bytes and all
resource gates stayed at zero. Static-world draw submission, not animation, is
the bottleneck in that fixture.

## Memory and sharing

The benchmark source occupies 96,792 GPU bytes: one 72-byte backend vertex for
each of 1,024 vertices plus 5,766 32-bit indices. That geometry remains one
copy for 1, 100, or 500 rigs.

Each rig has one immutable CPU palette shared by runtime/publication/projection,
one exact GPU storage buffer, and one exact transfer buffer. For a 64-joint rig
that is 8,192 bytes in each location: 24,576 bytes total palette/staging storage
per rig, before object-map overhead. One rig plus shared source is therefore
about 121,368 bytes; 100 rigs plus the same source are about 2,554,392 bytes.
The runtime also retains approximately 10,752 bytes of local/model pose buffers
and 456 bytes per active track for the 64-joint fixture.

CPU fallback adds a pooled 49,152-byte `RenderVertex` output for this 1,024-
vertex source and a private posed backend mesh while fallback is active. That
pool and private GPU geometry are absent from GPU/headless mode. Shared clip key
arrays and canonical source geometry are never multiplied by rig count.

## Validation and backend limits

The clean Windows Release build compiled all shader stages and staged the
opaque/shadow SPIR-V into both normal and RuntimeDistribution layouts. The full
40-test CTest suite passed, including animation, renderer/projection,
environment/lighting, GUI, assets, audio, physics, soft body, query,
interaction, packaging/relocation, FirstCompleteGame, player, and Luau/network
lifecycle coverage. The GPU-only matrix is intentionally manual because CI and
headless hosts cannot be assumed to expose a physical device.

The validated graphical backend is SDL GPU over Vulkan on the primary Windows
machine, including the 256-joint/32-KiB-per-palette limit. D3D12 was compiled by
the pinned SDL backend but was not selected on this machine, so no direct D3D12
performance claim is made. The public contract remains backend-neutral; a
backend that cannot support the semantic 256-joint limit selects CPU fallback
rather than reducing or truncating the asset limit.

## Explicit deferrals and next priorities

Foundation 2A does not implement semantic animated Attachments, Sounds,
ProximityPrompts, hitboxes, or cloth anchors. They still follow authored rigid
transforms, not visual bones. Root translation remains model-local visual pose;
there is no root-motion authority. Collision/query geometry remains the rigid
BasePart collider, and skeletal GPU buffers are not physics or XPBD inputs.
Cloth attached to a future animated rig must consume semantic animated anchors,
not renderer palette resources.

Also deferred are IK, retargeting, animation graphs, ragdolls/bone physics,
animation replication, timeline tools, authored animation LOD controls, crowd
instancing, and palette atlases. Internal update policy and measured jobified
evaluation are now implemented by Foundation 2C.

The measured 2A priorities were completed in order: [Foundation 2B](AnimationFoundation2SemanticAnchors.md)
closed the semantic anchor gap, then [Foundation 2C](AnimationFoundation2UpdatePolicy.md)
reduced visual-only work before jobifying the still-expensive full/semantic rig
solve. Renderer scalability remains separate: palette atlases or crowd
instancing should follow measured draw/submission workloads rather than becoming
part of the Animator contract.
