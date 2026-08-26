---
status: current
owner: rendering
last_verified: 2026-08-26
related_code:
  - assets/services/Lighting.luau
  - assets/classes/Sky.luau
  - include/gargantuan/environment/EnvironmentSemantics.hpp
  - include/gargantuan/render/RenderEnvironment.hpp
  - src/render/RenderPublisher.cpp
  - src/render/passes/SkyPass.cpp
  - src/render/passes/OpaquePass.cpp
  - src/render/passes/ShadowPass.cpp
  - tests/RendererPublicationTests.cpp
  - tests/EnvironmentLightingBenchmark.cpp
---

# Environment / Lighting Foundation 1

## Ownership and boundary

Each DataModel owns exactly one canonical schema-backed `Lighting` service. It
is ordinary semantic state: properties are saved, replicated, available through
generic schema metadata, and editable through the existing Studio property and
hierarchy workflows. This foundation adds no environment-specific public
service, editor command, or Studio panel.

`RenderPublisher` extracts a complete renderer-neutral `RenderEnvironmentState`
into immutable publications. The value contains colors, derived sun state,
exposure, simple fog, and optional six-face texture identities/revisions. It
contains no Instance, AssetId, source path, callback, SDL type, or GPU handle.
Backends and passes consume the projection only; they never traverse the
DataModel. AssetService remains the authority for decoded RGBA8 content and
generation-safe texture residency.

## Authored model and validation

`Lighting` has these bounded properties:

| Property | Default | Valid values |
| --- | --- | --- |
| `Ambient` | `(0.2, 0.2, 0.2)` | each channel `[0, 1]` |
| `SunColor` | `(1, 1, 1)` | each channel `[0, 1]` |
| `Brightness` | `1` | `[0, 8]` |
| `ClockTime` | `12` | strict `[0, 24)`; no modulo or clamping |
| `ExposureCompensation` | `0` | `[-8, 8]` stops |
| `EnvironmentColor` | `(0.08, 0.12, 0.2)` | each channel `[0, 1]` |
| `FogEnabled` | `false` | boolean |
| `FogColor` | `(0.5, 0.6, 0.7)` | each channel `[0, 1]` |
| `FogStart` | `0` | `[0, 100000]` and `FogStart <= FogEnd` |
| `FogEnd` | `1000` | `[0, 100000]` and `FogEnd >= FogStart` |

Setters reject NaN, infinity, invalid ranges, and inverted fog intervals before
commit. A write equal to current state is a no-op. There is deliberately no
`OutdoorAmbient` in Foundation 1. `Brightness` scales direct sun only;
`Ambient`, Sky pixels, and the fallback background are not multiplied by it.

`Sky` is an ordinary Instance intended as a direct `Lighting` child. `Enabled`
and the six strict Image asset-reference strings are saved and replicated. An
empty string is allowed as incomplete authoring state, but it cannot become an
active rendered Sky. Generic AssetReference editor hints filter every face to
Image assets.

## Sun and exposure semantics

World coordinates use `+Y` up. `SunDirection` points from the world toward the
sun:

```text
Angle = (ClockTime - 6) * 2pi / 24
SunDirection = normalize({ cos(Angle), sin(Angle), 0 })
SunIntensity = Brightness * max(SunDirection.y, 0)
ExposureMultiplier = 2 ^ ExposureCompensation
```

Therefore 06:00 is `+X` sunrise, 12:00 is overhead `+Y`, 18:00 is `-X`
sunset, and 00:00 is below the horizon. `SunColor` is authored. Direct light is
zero below the horizon. The shadow camera selects `+Z` as its up vector when the
sun is nearly collinear with world `+Y`, avoiding the noon look-at singularity.

## Effective Sky and coherence

Only enabled direct `Sky` children participate. If several are enabled, the
lowest generation-safe `ObjectId` wins and one bounded diagnostic is emitted.
Nested, disabled, destroyed, or wrong-class objects are inactive.

All six faces of the winner must resolve through AssetService to available Image
records and resident RGBA8 textures. Each face must be square, all dimensions
must match, and dimensions must be within AssetService's 1024-axis bound. The
published face order is `+X, -X, +Y, -Y, +Z, -Z`.

Validation is coherent: a partially edited or failed reimport never mixes new
and old faces. The publisher retains the last-known-good complete face set only
for the same effective Sky instance. A different Sky has no inherited fallback.
No enabled Sky produces the authored `EnvironmentColor`. Deleting an Image
source group referenced by any Sky face is rejected; successful content changes
mark the referencing Sky in the environment dirty domain.

The SDL shader's face UV mapping is:

| Face | Unnormalized UV axes |
| --- | --- |
| `+X` | `(-z, -y)` |
| `-X` | `( z, -y)` |
| `+Y` | `( x,  z)` |
| `-Y` | `( x, -z)` |
| `+Z` | `( x, -y)` |
| `-Z` | `(-x, -y)` |

The Sky ray uses camera right/up/look vectors and field of view, so the
background follows rotation but never translation.

## Publication and rendering

Environment properties, Sky lifecycle/reparenting, and Sky face references use
the dedicated `RenderUpdateDomain::Environment`. The bounded dirty accumulator
coalesces many writes by ObjectId. A changed environment sets one
`EnvironmentChanged` bit and carries complete final state; it does not emit
object geometry updates. Static frames reuse the projected environment. Full
resync and renderer restart reconstruct environment and texture residency.

The SDL pass order is shadow, Sky/background, opaque world, then GUI. The Sky
pass clears/draws the background. Opaque loads that color, clears depth, applies
per-channel ambient plus semantic colored sun and shadowing, uses inverse-
transpose normal transforms for nonuniform scale, applies optional linear
camera-distance fog, multiplies exposure, and uses the common ACES filmic
approximation. GUI is composited afterward and is intentionally independent of
world exposure and fog.

The current RGBA8/UNORM pipeline does not yet provide an end-to-end linear/sRGB
transfer contract. The lighting and ACES ordering is explicit and stable, but
exact colorimetric output remains limited by the existing display-ish texture
and swapchain formats. HDR output and color-management migration are deferred.

## Persistence, Play, packaging, and diagnostics

Generic instance persistence covers Lighting and Sky. Save/reopen, undo/redo,
replication, Edit/Play cloning, headless projection, and Studio inspection use
the existing schema and mutation boundaries. Runtime packages include the Sky,
opaque, and shadow shaders plus the referenced canonical image artifacts; no
source image path is needed at runtime. FirstCompleteGame contains authored
Lighting and a six-face Sky fixture using its packaged 512x512 Image asset.

Expected diagnostics use `[Environment:Lighting]`, `[Environment:Sky]`,
`[Environment:Fog]`, or `[Environment:Semantics]`. Invalid authoring is rejected
at mutation time when possible. Asset availability/coherence failures are
bounded extraction diagnostics and retain last-known-good or deterministic
fallback state; they do not log every static frame.

## Benchmark and platform validation

Release CPU measurements on Windows build `10.0.26200`, x64 AMD Family 25 Model
97 (32 logical processors), use 20 frames per case. Times below are means from
the dedicated environment benchmark and are nanoseconds per frame:

| Parts | Case | Dirty accumulation | Publication | Projection apply | Environment changes | Object operations |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1,000 | static | 20 | 1,195 | 595 | 0 | 0 |
| 1,000 | ClockTime | 595 | 1,300 | 555 | 20 | 0 |
| 10,000 | ClockTime | 1,995 | 1,535 | 565 | 20 | 0 |
| 50,000 | ClockTime | 2,525 | 2,900 | 940 | 20 | 0 |
| 50,000 | fog | 885 | 1,805 | 950 | 20 | 0 |
| 50,000 | exposure | 1,010 | 1,705 | 910 | 20 | 0 |
| 50,000 | one-face reimport | 2,035 | 5,425 | 1,165 | 20 | 0 |

The 50K ClockTime path still publishes exactly zero object operations. The cache
estimate is 160,240 bytes at 1K, 1,600,240 bytes at 10K, and 8,000,240 bytes at
50K. The small reimport fixture has two unique 4x4 RGBA8 resident images (128
bytes) referenced by six coherent faces. This is a structural estimate; the
repository has no allocation-counting harness, so no allocation-count claim is
made.

Primary real-device validation used SDL's Vulkan backend on an AMD Radeon RX
7900 XT, AMD driver 26.7.1, at 1280x720 with completion fences. Over 60 measured
frames, fallback-static averaged 0.0968 ms/frame and the animated six-face Sky +
fog + ClockTime + exposure + periodic face reimport case averaged 0.0960
ms/frame. The animated case recorded 60 environment applications, 60 Sky draws,
6 face uploads, no buffer/transfer allocation after warmup, and a passing fresh-
renderer resync. These are CPU wall/fence-completion measurements, not GPU
timestamp queries. A 32,761-vertex rubber workload averaged 1.0473 ms/frame; the
mixed 25K rigid + 16K cloth + 5K GUI workload averaged 36.7659 ms/frame and kept
resource stability gates passing. Offscreen pixel readback separately proves
fallback color changes, the camera-facing `-Z` Sky orientation, and identical
full-screen GUI pixels at exposure multipliers 1/256 and 256.

No genuinely independent second GPU/backend was available locally. Headless
publication/projection and package relocation remain backend-free validation;
the Native Engine CI sanitizer job is the independent Linux compiler/platform
gate.

## Deferred work

Foundation 1 does not add local point/spot lights, advanced shadow cascades or
quality controls, atmosphere/cloud simulation, image-based lighting,
environment probes, HDR/sRGB migration, post-processing volumes, or bespoke
Studio environment tools. Those features must extend this semantic/publication
boundary rather than move renderer or asset authority into Instances.
