---
status: qualification-method
authority: non-normative
owner: rendering
---

# Renderer Foundation 3 qualification

The current benchmark adds `--incremental` to `gargantuan_renderer_benchmark`.
It feeds both offscreen paths the same immutable `RenderPublication` sequence
and shared canonical vertex/index data. The older invocation retains the
complete-snapshot path measured in [Renderer Backend Evaluation](../CurrentArchitecture/RendererBackendEvaluation.md).
[Foundation 2C](../CurrentArchitecture/RendererFoundation2C.md) supplies the
hardware and synchronization precedent. Its SDL GUI, cloth and rubber results
remain separate workloads.

| Scenario | Identical published workload |
| --- | --- |
| `static` | Shared Block mesh and empty deltas after the initial full resync |
| `mostly-static` | Moving transforms for a rotating 1% cohort, with at least one update per frame |
| `dynamic` | Moving transforms for the entire scene |
| `culled` | Same Block mesh with every second object behind the camera |
| `mixed` | Two opaque textured material ranges per Block, a 1% transform cohort, and one alternating 8×8 patch and 64×64 full texture update |
| `material-check` | Separate two-range chart with colored albedo, a texture patch that must preserve the untouched half, and flat versus tilted normal map samples under the same directional light |

All five timed scenarios publish opaque metallic-roughness materials. The four
untextured rows use the same linear gray dielectric with roughness 1.0. Every
row and the material chart use an authored exposure multiplier of 1.0 and the
same directional and ambient light inputs for both backends. Native light
calibration, BRDFs and output transforms still differ. Interpret the timings
as costs for the same authored workload rather than equal-image-quality costs.

The chart emits two top-left RGB PPM images even if its readback probes fail.
The color gate requires red, blue, preserved red and green at known world
positions. The normal gate requires brighter green under the flat normal than
the tilted one. These checks establish representative rendered behavior with
separately authored inputs and compare semantic effects across different
exposure and lighting implementations. Retain the CSV row, images, command,
exit status and executable identity before ranking a backend. The opaque
qualification scope leaves alpha, full lighting, GUI, particles, imported skinning,
dynamic topology, device recovery and packaged deployment as separate axes.

Each timed row starts with a completed initial publication and drains the warmup
queue before measuring. It reports fixture construction, renderer startup,
first completed frame and steady frame P50/P95/P99/max. Completion mode includes
the SDL fence or Filament flush-and-wait per frame. Queued mode reports host
submission and queue pressure, followed by a separately timed final drain.
Neither metric is pure GPU execution time. Backend phase clocks describe
implementation-local work. SDL apply covers mesh, texture and UI preparation.
Filament apply includes changed native objects and related resources. Keep
those phase scopes distinct when interpreting the numbers.

The frame admission gate requires exactly one submitted frame per measured
publication, zero skips and zero full resyncs during measurement. A counter
absent from a backend is `NA`. SDL reports recorded draw commands and resource
operations. When available, SDL also reports main camera culls and world geometry
and material bindings. The camera gate checks the known number of behind-camera
objects on `culled` and zero camera rejections on the other grid rows. Filament
native upload bytes describe its own packed resource
representation, so the neutral requested texture bytes are also reported.
Real post-cull draws, device-owned memory and publication-attributed GPU
timestamps require backend evidence before they can be compared. Fixture time
is synthetic allocation and assembly. Use `gargantuan_renderer_foundation2_benchmark`
for authoritative mutation, extraction and publication costs.

```powershell
cmake --build <build> --config Release --target gargantuan_renderer_benchmark gargantuan_shaders
$env:SDL_GPU_DRIVER = 'vulkan'
<build>/gargantuan_renderer_benchmark.exe --incremental --backend=sdl --scenario=material-check --capture=<new-prefix>
<build>/gargantuan_renderer_benchmark.exe --incremental --backend=filament-headless --scenario=material-check --capture=<other-new-prefix>
<build>/gargantuan_renderer_benchmark.exe --incremental --backend=sdl --scenario=all --count=10000 --frames=300 --warmup=30 --synchronize=on
<build>/gargantuan_renderer_benchmark.exe --incremental --backend=filament-headless --scenario=all --count=10000 --frames=300 --warmup=30 --synchronize=on
```

Use fresh process invocations for counterbalanced order, the same driver API,
the same target resolution, and recorded hardware, driver and build identities.
Repeat at the intended scene sizes and with `--synchronize=off` for queue
behavior. Collect Studio viewport readback and presentation separately. The
comparison establishes performance for the shared opaque workload. The
architecture decision also weighs integration gaps, maintenance and deployment.

## Decision criteria

Use only rows that complete material capture, submit every measured frame and
hold the expected scene size. The 16.67 ms frame interval from Foundation 2C is
the reference for tail cost. Compare completion P95 and P99 at the same size
and resolution, then compare queued submission together with the final drain.
Treat results close to the run-to-run variation as equivalent. The shadow-off
rows leave each backend's normal pass setup in place. SDL still clears its
directional depth target while Filament disables its native shadowing feature.

Keep the production renderer choice separate from the decision about where
to invest in the broader shading stack. Any production backend change also
needs the existing application surface, including GUI, Sky and fog, Studio
viewport integration, deformable and skinned geometry, recovery, and packaged
deployment. Record unsupported surfaces alongside the measured costs.
