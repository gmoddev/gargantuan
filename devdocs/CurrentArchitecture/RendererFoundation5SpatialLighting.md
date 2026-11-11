---
status: experimental
owner: rendering
related_adrs:
  - RendererFoundation2C.md
related_code:
  - assets/classes/ReflectionProbe.luau
  - include/gargantuan/environment/ReflectionProbeSemantics.hpp
  - src/render/RenderPublisher.cpp
  - tests/ReflectionProbeSemanticsTests.cpp
---

# Renderer Foundation 5 spatial lighting contracts

This continuation starts at Foundation 4 `94f920485564798742bc22a061cbc3a415034fa3`.
The qualified implementation remains `adf0acda14e6ce5e57657bf01254f1e840c99868`.
The first Foundation 5 attribution build is `29b695b52609977323d590ca03b8426fdc5891b4`.
Local evidence is under `renderer-foundation5-20261111`; prior Foundation 4
packages and unsuccessful experiments remain immutable. Native probe rendering
and layered shadows are implemented experimental paths. Their physical GPU
qualification remains separate from source/syntax and numerical checks.

## Probe authority and publication

`ReflectionProbe` is an ordinary schema-backed Instance. `WorldRoot` maintains
generation-safe weak descendant membership. `RenderPublisher` resolves existing
AssetService Image references and publishes at most 32 coherent probes in ObjectId
order. It inspects at most 256 enabled candidates per extraction; incomplete or
invalid sources do not consume publication slots, and a source inspection
overflow is diagnosed. It shares equal immutable lists across publications. Nearest-BasePart
attachment uses the render CFrame and ignores BasePart size. Owner pose changes
invalidate environment extraction through the existing render dirty accumulator.
No resource, source pathname, importer or Instance pointer enters the publication.
Global `Lighting` and effective direct-child `Sky` selection retain authority.

| Property | Default | Bound and meaning |
| --- | --- | --- |
| `ProbeEnabled` | true | Participates in scoped extraction |
| `ProbeCFrame` | identity | Rigid orientation; position components within ±1e6 |
| `ProbeSize` | (16,16,16) | Full oriented dimensions, each [0.1,16384] |
| `ProbeCaptureOffset` | zero | Strictly inside the box, in cube-local coordinates |
| `ProbeBlendDistance` | 1 | [0,8192], inner world-unit boundary fade |
| `ProbeIntensity` | 1 | [0,8], linear local radiance scale |
| `ProbePriority` | 0 | Integer [-1024,1024] |
| `ProbeBoxProjection` | true | Specular ray correction against the box |
| `ProbePositiveX`, `ProbeNegativeX`, `ProbePositiveY`, `ProbeNegativeY`, `ProbePositiveZ`, `ProbeNegativeZ` | empty | Strict Image asset references in cube-local face order |

The six inputs must resolve coherently to equal square dimensions at most 1024.
Invalid or incomplete authored probes are skipped with bounded diagnostics;
other probes and global environment remain available. Neutral projection rejects
invalid identities, order, finite/rigid bounds and incoherent source revisions
before mutating accepted state. Generic schema editing, journal, persistence,
replication and EditorHost commands remain the existing systems.

Influence is evaluated at the shading point in the oriented box. Smoothstep of
the minimum inside distance divided by blend distance defines weight; zero blend
distance is a hard boundary. At most two selected local probes contribute.
Priority precedes weight and ObjectId resolves equal ties. Equal priorities
normalize only above unit sum; a lower priority uses the remaining high-priority
weight. Global environment receives the residual, so the combined unit source
weights sum to one. More than two equal overlapping sources require deliberate
volume authoring; this bounded selection is not full global illumination.
Intensity scales each selected source after weighting. Diffuse uses the local
normal; specular intersects the reflected ray with the local box and subtracts
the capture offset. Parallel ray axes never divide by zero. This follows
[the original parallax-corrected cube derivation](https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/).

## Shadow authoring handoff

Existing `Lighting` gains `ShadowCascadeCount`, integer [0,4], default zero for
legacy fixed-volume behavior, and `ShadowDistance`, [1,4096], default 250 world
units. The immutable settings retain renderer-neutral cascade and bias values.
Existing LocalLight gains `LightShadows`, default false, and integer
`LightShadowPriority` in [-1024,1024]. Backend admission is bounded; a request
does not guarantee a map. Unsupported light kinds and denied requests contribute
unshadowed illumination. Source and GPU qualification must establish any later
default change.

Studio's extension branch remains independent. These fields use its existing
schema/property commands and asset hints. No Studio or LUI files are changed;
the final qualification record must provide exact source identities and fixtures
for a later authoring integration.

## Retained UI texture lifetime

Projection preflight rejects removal of a texture still referenced by retained
UI when the delta has no UI update. An explicit UI replacement or teardown may
remove that reference and the texture in the same atomic publication. A failed
removal retains the accepted UI, texture and publication identity. This closes
an independently reproduced lifetime gap before any native GUI command records.

## Reference verification

The SDL Sky pass owns the local prefilters and two cube arrays (12 resident
sources, 16px diffuse and 64px seven-level reflection). Arrays use about 3.14 MiB
of logical texels. Each resident prefilter currently owns about 300 KiB plus
its pipelines/LUT; sharing the device-wide BRDF and pipelines is a later resource
optimization. At most one local prefilter remains in flight, with at most one
new local source prepared per frame. A global Sky build has priority. Completed
prefilters copy into stable array layers before world shading, and only submitted
copies become reusable. A changed residency generation, source identity or
revision requires preparation. Pose, priority, intensity and camera changes do
not alter a source key. Array allocation failures retry after 120 preparation
frames; unsupported format capability remains disabled until recreation.
Canceled frames invalidate the record-buffer upload acknowledgement. SDL can
cycle the active backing while recording an upload; canceling that command does
not restore the previous backing. The next frame uploads even if its records
equal the last submitted values. A native post-Sky cancellation fixture checks
that acknowledgement, restored exact pixels, and continued publication order.
The GPU fixture also uses an actual blue global Sky and a red local source:
full-weight local influence excludes global radiance, a 16-step camera sequence
checks monotonic boundary blending, and leaving the volume restores the global
receiver color. The earlier empty-environment room captures establish a
different acceptance fact and are retained separately.

Residency chooses the nearest influence volumes first, then priority and stable
identity. Priority resolves overlapping shading influence, so distant high
priorities cannot displace a nearby room. The shader evaluates at most 12
resident volumes and samples at most two sources. Source disappearance clears
the corresponding record immediately; losing all authored probes releases both
large arrays. In-flight prefilter retirement retains at most one pending source
until its fence completes. Global and probe-only lighting use the same BRDF
integration rather than substituting a white lookup in the absence of Sky.

These probes currently consume authored/imported six-face Image assets. Automatic
scene capture, authored bake scheduling and geometry-dependent recapture are not
yet implemented. Devices without cube-array support select a shared portable
material shader that preserves global IBL and shadows. Physical compatibility
claims still require dedicated adapter gates.

`gargantuan_reflection_probe_tests` checks rotated boxes, capture offsets,
parallel rays, large coordinates, priority, empty fallback and unit energy under
a 1,801-point boundary sweep. Publication tests cover schema persistence, weak
membership, invalid transforms, finite bounds, stale source revisions, atomic
rejection and cascade budget rejection. Actual GPU texel and temporal captures
remain separate gates from these CPU proofs.
