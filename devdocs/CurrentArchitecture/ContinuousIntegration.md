---
status: current
owner: build-and-test
last_verified: 2026-08-29
related_code:
  - .github/workflows/native-ci.yml
  - CMakeLists.txt
  - rokit.toml
  - tools/classgen.luau
related_adrs: []
---

# Continuous native build and test contract

## Supported CI configuration

`.github/workflows/native-ci.yml` is the required native engine check for every
push, pull request, and manual dispatch. Its first gate is Windows x64 on
GitHub's explicit `windows-2025-vs2026` image. It matches the production
checkpoint's Visual Studio 2026/MSVC 19.50-or-newer ABI family and uses the
image's CMake and Ninja installations. A dependent Ubuntu 24.04 job then builds
and runs the complete headless contract with Clang 19, ASan, and UBSan.

The repository has no checked-in CMake preset. CI therefore records its complete
single-configuration Ninja contract directly:

```text
CMAKE_BUILD_TYPE=Release
BUILD_TESTING=ON
GARGANTUAN_TRACY=OFF
GARGANTUAN_WITH_GNS=ON
GARGANTUAN_BUILD_RENDERER_BENCHMARKS=ON
GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS=ON
GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF
GARGANTUAN_GLSLC_EXECUTABLE=<discovered pinned Vulkan SDK glslc.exe>
```

Filament and the Glaze prototype are not part of either gate. The Windows job
builds the normal engine, SDL renderer sources, shaders, pinned
GameNetworkingSockets adapter, and native regression targets. The Linux job
builds the same production renderer/core sources but deliberately disables GNS
and hardware renderer benchmarks so the sanitizer gate remains headless. This
is demonstrated Linux sanitizer coverage, not general Linux product support;
the workflow makes no macOS CI claim.

## Fresh-checkout bootstrap

The workflow deliberately supports a cold runner:

1. `actions/checkout` initializes every Git submodule recursively and a
   preflight rejects missing, conflicted, or wrong-revision submodules.
2. The pinned Rokit release installs the exact tools in `rokit.toml`, including
   Lute 1.0.0. Rokit's cache is an optimization only.
3. Visual Studio's `clang-format` is located and `lute tools/classgen` generates
   the ignored class/service reflection sources before CMake configure.
4. The pinned Chocolatey Vulkan SDK package provides `glslc`; the executable is
   located, version-probed, and passed to CMake explicitly.
5. `vswhere` locates the x64 Visual Studio C++ workload and each configure/build
   step enters that installation's developer environment.
6. The runner's vcpkg toolchain restores Protobuf from `cmake/gns/vcpkg.json`,
   whose builtin baseline is pinned, and CMake fetches GameNetworkingSockets at
   the immutable revision declared in `cmake/GameNetworkingSockets.cmake`.

`CMakeLists.txt` also checks representative files from every required top-level
dependency and every expected output derived from the class/service declarations.
Missing dependency content
reports the recursive submodule command. Missing generated content reports the
Rokit, `clang-format`, and class-generator commands. Existing shader discovery
continues to reject a missing, stale, wrongly named, or non-executable `glslc`.
CMake itself owns missing compiler diagnostics.

## Enforced tests

CI builds the default Release target graph, including `gargantuan`, the shader
outputs, and registered test executables, then runs:

```text
ctest --test-dir build-ci -C Release --parallel 2 --timeout 300
      --output-on-failure --no-tests=error
```

Shader compilation and runtime staging are distinct build obligations. The
default `gargantuan_stage_shaders` target copies every generated shader into the
executable's `shaders` directory on every incremental build, even when the
executable does not relink. `gargantuan_packaged_shaders` compares that runtime
directory with the complete source shader set and fails when a compiled output
is missing.

The Windows production contract contains 42 tests. The Linux sanitizer
configuration with `GARGANTUAN_WITH_GNS=OFF` and renderer benchmarks disabled
omits the two real-transport and two renderer-headless entries and therefore
contains 38; that reduced matrix is not the complete Windows gate. Current
CMake and this document are authoritative.

| Coverage | Existing CTest entries |
| --- | --- |
| Shader requirement | `gargantuan_shader_tool_validation` |
| Runtime shader package completeness | `gargantuan_packaged_shaders` |
| Foundation/runtime, persistence and serialization, EditorHost/protocol, render extraction and backend boundary | `gargantuan_foundation` |
| Asset import, canonical artifact, dependency graph, and runtime materialization | `gargantuan_asset_foundation` |
| Skeletal import/artifacts, playback/blending, CPU/GPU/headless pose equivalence, semantic Attachment/Sound/Prompt anchors, reimport/lifecycle/journal behavior, and bounded Release scaling/50K smokes | `gargantuan_animation_foundation`, `gargantuan_animation_foundation_benchmark_smoke` |
| Optional telemetry dynamic loading, ABI negotiation, consent, privacy, and fail-open lifecycle | `gargantuan_optional_telemetry` |
| Physics | `gargantuan_physics_backend` |
| Soft-body runtime and bounded Release smoke | `gargantuan_soft_body_physics`, `gargantuan_soft_body_physics_benchmark_smoke` |
| Platform input | `gargantuan_platform_input_boundary` |
| Player runtime | `gargantuan_player_runtime` |
| Provider-neutral entitlement semantics, Luau authority, vectors, headless lifecycle, and provider overhead smoke | `gargantuan_entitlement_service`, `gargantuan_entitlement_provider_benchmark_smoke` |
| GUI retained runtime and bounded Release smoke | `gargantuan_gui_foundation`, `gargantuan_gui_foundation_benchmark_smoke` |
| PreRun bootstrap | `gargantuan_prerun_bootstrap` |
| Serialization smoke | `SerializationBenchmarkSmoke` |
| Networking contracts and deterministic transport | `gargantuan_networking_contracts`, `gargantuan_simulated_transport`, `gargantuan_scheduler_contract` |
| Replication and remotes | `gargantuan_replication`, `gargantuan_remote`, `gargantuan_remote_luau` |
| Networking bounded-load smokes | `gargantuan_replication_benchmark_smoke`, `gargantuan_remote_benchmark_smoke` |
| Real transport lifecycle and remotes | `gargantuan_real_transport`, `gargantuan_remote_real_transport` |
| Renderer extraction/projection and renderer interface | `gargantuan_renderer_projection_headless`, `gargantuan_renderer_backend_headless` |
| Renderer Foundation 2B dirty coalescing/bounds, publication lifecycle, deformable ranges, and UI batches | `gargantuan_renderer_publication` |
| Environment publication/application scaling, ClockTime, fog, exposure, and Sky reimport smoke | `gargantuan_environment_lighting_benchmark_smoke` |
| Package format, integrity, relocation, atomicity, cancellation, asset closure, shared packaged-world bootstrap, and packaged gameplay | `gargantuan_packaging` |
| EditorHost package authority, revision, conflict, Play exclusion, progress, and cancellation | `gargantuan_packaging_editor_host` |
| Package capture/build bounded-load smoke | `gargantuan_packaging_benchmark_smoke` |
| FirstCompleteGame headless Play/Stop and semantic animated-anchor proof | `gargantuan_first_complete_game` |
| FirstCompleteGame CLI build/validate/inspect, relocation, dedicated player startup, animated-anchor asset closure, and corruption rejection | `gargantuan_first_complete_game_package_smoke` |

CTest labels mirror those coverage groups so developers can select a subsystem,
but CI runs the complete registered set rather than maintaining a second list of
test executables.

## Headless boundary

Every registered test in this configuration runs without a display or GPU.
Player-runtime tests use `HeadlessRenderer`. The renderer smoke entries invoke
the existing renderer benchmark with `projection` and `headless` backends; those
paths do not initialize SDL video or create an SDL GPU device. Foundation tests
exercise render extraction, renderer ownership, viewport picking, and
EditorHost protocol behavior using CPU-side state.

`gargantuan_editor_viewport_smoke` is built but not registered because its
purpose is offscreen pixel capture and its implementation creates an SDL GPU
device. The SDL and optional Filament GPU tests likewise remain outside the
headless gate. A future GPU-capable runner may add them as a distinct job; they
must not be reported as having run on the current headless worker.

## Failure artifacts and hardening

CTest prints failing output and writes JUnit results. On any failed job, the
workflow uploads the configure log, CTest log, and JUnit file when available.
No dependency or compiler cache is required for correctness; only the
repository-tool download cache is currently enabled. Protobuf and the pinned GNS
source must therefore restore successfully on a cold runner.

ASan/UBSan are deliberately a dependent second gate rather than part of the
Windows production configuration. Ubuntu 24.04 uses Clang 19 plus the GCC 14 C++
runtime, `-fsanitize=address,undefined`, leak detection, immediate failure, and
symbolized stacks. It runs registered tests serially with the SDL dummy video
driver. The sanitizer job uploads the same configure, CTest, and JUnit evidence
as the Windows gate on failure. UBSan is not described as an MSVC feature, and
the workflow does not weaken the normal Windows ABI/toolchain contract to obtain
sanitizer coverage.
