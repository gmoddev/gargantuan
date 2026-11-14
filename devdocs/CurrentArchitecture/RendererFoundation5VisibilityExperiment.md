---
status: experimental
owner: rendering
---

# Renderer Foundation 5 visibility experiment

The camera pass currently walks `RenderProjection` objects in its original
`unordered_map` order and applies the Foundation 4 conservative `OutsideView`
test. That order matters for exact depth ties. The standalone
`tests/RendererVisibilityHierarchyProbe.cpp` compares this source kernel with
two renderer-owned, disposable rejection structures:

- A deterministic four-way hierarchy of transformed world bounds, with
  32-object leaves and spatial median partitions. It writes candidate bits
  and then emits visible object ordinals in original traversal order.
- One conservative envelope around every object. It skips the entire camera
  object traversal only when that envelope is outside the camera. Otherwise
  it executes the exact original linear test and order. Updates expand the
  envelope, so removal or motion cannot cause a false rejection; a later
  rebuild can recover tightness.

Neither structure owns object identity, changes picking, queries Engine
Instances, or changes publication authority. Both test the exact Foundation 4
object kernel for every object they cannot reject. Hierarchy construction
includes transforming all object bounds; refit includes changed bounds and
affected nodes. The envelope construction transforms all object bounds, and
its dynamic cost expands changed bounds. The experiment records construction,
query, refit, and additional storage separately.

## Bounded MSVC result, 2026-11-11

Three independent `/O2` executions each ran 1K, 10K, 25K, 50K, and 100K
objects through fully visible, fully culled, spatially mixed 50% culling,
rotated camera, 1% moving, and 100% moving controls. Each row alternated the
order of the linear, hierarchy, and envelope queries over eight frames;
the first two were warmup and the median of six retained query observations
was used per execution. Exact visible ordinals matched in every timed row.
The adversarial control matched 40 camera views over 768 objects, including
near-plane positions, million-unit coordinates, extreme scales, rotations,
and incremental refits. No false cull was observed in this bounded input.

Mean of the three within-run medians, in milliseconds:

| Workload | Objects | Linear | Four-way hierarchy incl. refit | Envelope incl. update | Hierarchy wins |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fully culled | 50K | 0.727 | 0.046 | <0.001 | 3/3 |
| Mixed 50% | 50K | 0.964 | 1.217 | 1.072 | 1/3 |
| Rotated camera | 50K | 1.170 | 1.520 | 1.174 | 0/3 |
| 1% moving, mixed | 50K | 0.953 | 1.035 | 0.964 | 0/3 |
| 100% moving, mixed | 50K | 1.035 | 3.134 | 2.503 | 0/3 |
| Fully visible | 50K | 1.541 | 2.110 | 1.482 | 0/3 |
| Fully culled | 100K | 1.271 | 0.066 | <0.001 | 3/3 |
| Mixed 50% | 100K | 1.919 | 2.135 | 1.924 | 0/3 |
| 100% moving, mixed | 100K | 2.169 | 9.116 | 5.181 | 0/3 |

The 50K hierarchy build averaged 43.7 ms and added 3.82 MB of bounds and
index storage; the 100K build averaged 84.8 ms and added 6.95 MB. It needs
roughly 72–74 fully culled frames to repay construction. The 50K and 100K
envelope builds averaged 1.65 and 3.53 ms respectively, with one 56-byte
bound, and repay in roughly three fully culled frames. Tiny differences in
envelope rows that still execute the full linear scan are timing noise or
cache order effects, not a general speedup. The complete 30 scenario rows
and three raw executions are in
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111/visibility-probe`.
The exact LF source SHA-256 is
`3C5CA2B046A8300FF1C29D082CA9E5A62731D1CD56FFCD753D43A49F92979FE6`;
the `/O2` executable SHA-256 is
`3D200E7D91C6BA9D743538A9C609657D14186836014F607198212F0CAA4A1F5B`.
The source builds with MSVC `cl /O2 /std:c++20 /EHsc` using the repository
`include` and `vendor/glm` directories and runs without arguments.

## Decision

Reject the four-way hierarchy for the current 50K mixed regression. Its
construction, memory, ordered output pass, and refit cost are not justified
by this workload. The source experiment supports only a bounded coarse
scene-envelope fast path for a mostly static scene when the whole envelope
is conservatively off-camera. If it intersects or has uncertain bounds, run
the exact linear traversal. Dense dynamic updates should keep the linear
path; building or expanding an envelope every frame loses its advantage.
This is a source-kernel result, not a completed SDL frame result. Production
admission would require a same-binary opt-in control, actual mesh bounds and
deformable exclusions, camera pixel parity, and physical GPU/CPU timing.

There is no renderer-neutral authored geometry LOD chain in the current
publication contract. Future LOD selection needs explicit alternate mesh
identities, deterministic thresholds and hysteresis, and an authored shadow
and picking policy. This experiment does not simplify or replace meshes.
