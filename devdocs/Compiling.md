---
status: current
owner: build-and-test
last_verified: 2026-11-15
related_code:
  - CMakeLists.txt
  - .justfile
  - rokit.toml
  - .github/workflows/native-ci.yml
---

# Build and test Gargantuan

These instructions describe integrated source `b5df1e7b1b80da27158ea794626f90a8ebd8a6d2`.
The checked-in CMake files and CI workflows remain the exact configuration
contract. Headless tests do not qualify a GPU or a physical network deployment.

## Platform coverage

| Platform | Established coverage | Limits |
| --- | --- | --- |
| Windows x64 | Required Release native CI, MSVC 19.50+ / Visual Studio 2026 | Graphical execution and physical networking need separate qualification |
| Ubuntu 24.04 x64 | Required Clang 19 Release ASan/UBSan headless CI | Not complete graphical Linux product certification |
| macOS | Source/build target | No required macOS CI or current hardware qualification claim |

## Prerequisites and checkout

Install Git, CMake **3.25 or newer**, Ninja, a C++23 compiler, Rokit and
`clang-format`. The optional Just wrapper also expects `ccache`. Install the
Vulkan SDK's `glslc` (or a genuine compatible `glslc` on PATH); normal builds
compile shaders even for headless tests. Windows commands must run in the
x64 Visual Studio developer environment. CI's optional GNS configuration
additionally restores Protobuf through its pinned vcpkg manifest.

```sh
git clone --recursive https://github.com/gmoddev/gargantuan.git
cd gargantuan
rokit install
lute tools/classgen
```

For an existing clone, run `git submodule update --init --recursive` first.
`rokit.toml` pins Lute and other tools, separately from embedded Engine Luau.
Ensure `clang-format` is discoverable. Generated class/service headers and
implementations are ignored artifacts and **must exist before CMake configure**.

## Configure, build, and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DGARGANTUAN_TRACY=OFF
cmake --build build --parallel 4
ctest --test-dir build -C Release --parallel 2 --timeout 300 --output-on-failure --no-tests=error
```

If discovery fails, pass the actual shader compiler with
`-DGARGANTUAN_GLSLC_EXECUTABLE=<absolute-path-to-glslc>`. CMake rejects missing
dependencies, generated sources and invalid shader tools. A focused headless
profile cannot replace the normal shader/build contract.

The `.justfile` offers `just configure build_type=Release`, `just build`,
`just run_example baseplate.luau` and `just fresh_example baseplate.luau`. Generate before
the first configure. `just build` regenerates sources but uses every detected
CPU; prefer the explicit bounded build on shared machines. **`just test` runs
the legacy Lest core suite only.** It does not replace CTest.
`just run_testbed` and `just configure sanitizer=asan` are not recipes.

Use `ctest --test-dir build -N` to inspect registered tests and `-R <pattern>`
for focused iteration. The complete required suite and five Release workloads
are defined in [Continuous integration](CurrentArchitecture/ContinuousIntegration.md)
and [native-ci.yml](../.github/workflows/native-ci.yml). Test counts depend on
options and revision.

## Run a script or sample

From the repository root after building:

```sh
build/gargantuan --script=assets/examples/baseplate.luau
build/gargantuan --project=samples/FirstCompleteGame
```

On Windows use `build\gargantuan.exe`. These open graphical runtimes.
Add `--headless` for display-free execution; it disables graphics and audio
and does not prove pixels. Runtime scripts can enter an indefinite game loop;
use a parser to check syntax.

The [FirstCompleteGame sample](../samples/FirstCompleteGame/README.md) can
also be opened through a compatible Studio build. The package CLI is:

```sh
build/gargantuan-packager build --project samples/FirstCompleteGame --output packages/FirstCompleteGame --configuration Release
build/gargantuan-packager validate packages/FirstCompleteGame
```

Executables use `.exe` on Windows. Packages include runtime modules, shaders,
assets, dependencies and notices. See
[packaging](CurrentArchitecture/StandalonePackagingFoundation1.md) for options
and lifecycle. Dedicated `GargantuanServer` is distinct from the development
executable's `--headless` option.

The integrated distribution stages all 18 compiled shaders on incremental
builds and verifies the runtime shader directory against the source set. A
partial legacy shader directory is not a valid Foundation 3–5 package. The
renderer reference describes HDR/SDR, backend capability and device limits;
successful packaging alone does not qualify a particular GPU.

## Optional configurations

| CMake option | Default | Effect |
| --- | --- | --- |
| `GARGANTUAN_WITH_GNS` | `OFF` | Pinned GameNetworkingSockets adapter and real-transport tests; enabled in Windows CI |
| `GARGANTUAN_WITH_NODE_CONTENT` | `OFF` | Trusted Server-only Node content adapter; needs canonical Node proto root, Protobuf and gRPC |
| `GARGANTUAN_TRACY` | `OFF` | Instrumented client; compare performance under a declared profile |
| `GARGANTUAN_WORK_TRACE` | `OFF` | Bounded local Engine phase summaries and scoped timing export |
| `GARGANTUAN_BUILD_RENDERER_BENCHMARKS` | `ON` | Benchmark targets; physical GPU execution remains separate |
| `GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS` | `ON` | Serialization comparison target |
| `GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE` | `OFF` | Private codec experiment outside production acceptance |
| `GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION` | `OFF` | Focused named geometry tests without shader tooling; graphical/package targets fail closed |

The Engine VM pins Luau 0.734 with three-component float vectors
(`LUA_VECTOR_SIZE=3`, `LUA_VECTOR_DOUBLE=0`, compiler `vectorPrecision=0`).
Studio and Lute have independent runtime pins.

## Local performance traces

Use the usual optimized configuration and add
`-DGARGANTUAN_WORK_TRACE=ON`. Build `gargantuan` for the development
Engine loop, `gargantuan_player` and `gargantuan_server` for packaged
play, or `gargantuan_gameplay_allocation_workload` for the scripted
possession comparison. The workload needs `BUILD_TESTING=ON` and uses
`GARGANTUAN_WITH_GNS=ON` for the real loopback transport. Use
`ctest --test-dir <build> -R ^gargantuan_work_trace$` to check the bounded
recorder, export, and thread-local scope behavior.

The trace is enabled by `GARGANTUAN_WORK_TRACE_FILE`, a new output path.
With the variable unset, the instrumented build does not start a trace.
The compiled-off build excludes the trace-specific scope hook. This option
is independent of the existing test-owned work samples, the opt-in telemetry
library and the Tracy build. Leave Tracy disconnected and keep diagnostic
logging fixed during timing comparisons.

| Runtime setting | Default | Effect |
| --- | --- | --- |
| `GARGANTUAN_WORK_TRACE_FILE` | Unset | Export on normal completion to a new file |
| `GARGANTUAN_WORK_TRACE_MODE` | `summary` | Frame summaries; use `detailed` to include individual scope spans |
| `GARGANTUAN_WORK_TRACE_STRIDE` | 8 | Sample every nth workload frame, 1 through 6,000 |
| `GARGANTUAN_WORK_TRACE_FIRST_TICK`, `GARGANTUAN_WORK_TRACE_LAST_TICK` | Full run | Inclusive tick window, useful for an isolated burst |
| `GARGANTUAN_WORK_TRACE_PHASES` | All | Comma-separated names from [the work phase table](../src/runtime/RuntimeWorkDiagnostics.hpp) |
| `GARGANTUAN_WORK_TRACE_CAPACITY` | 65,536 | Record limit, 1 through 262,144 |

For a bounded audio capture, build `gargantuan_audio_foundation_benchmark` in the
instrumented Release configuration and run it with `--mix-controls`. Use a
fresh trace path and select `AudioReconcile,AudioMix`. The workload advances
32 voices across all categories for 500 steps and changes one category gain
every 25 steps. It uses a synthetic backend, so the trace measures software
mixing and reconciliation independently of audio hardware. Run the benchmark
without arguments in the ordinary Release build for the shared unity-gain
workloads and their allocation counts.

For example, capture a bounded real-GNS structural crossing from PowerShell
using the existing deadline-paced workload:

```powershell
$env:GARGANTUAN_WORK_TRACE_FILE = 'gameplay-crossing.json'
$env:GARGANTUAN_WORK_TRACE_MODE = 'detailed'
$env:GARGANTUAN_WORK_TRACE_STRIDE = '1'
$env:GARGANTUAN_WORK_TRACE_FIRST_TICK = '912'
$env:GARGANTUAN_WORK_TRACE_LAST_TICK = '930'
& ./build/gargantuan_gameplay_allocation_workload --generic-possession-scale gns 40 1500 off deadline
```

Open the JSON in Perfetto or a Chrome Trace Event viewer. Frame markers carry
the fixture tick, preceding frame interval, and deadline information. A scope
span represents one measured operation. Phase, queue and work-counter
summaries are instantaneous samples. Their arguments report inclusive and
exclusive time, units, sampled allocation deltas where a reader is supplied,
queue depth, age, and deferrals. Timing export happens after collection; a
full buffer drops later records and reports the exact count in
`otherData.dropped`. Check this count before interpreting a timeline.

For packaged play, launch each Player or Server with a distinct trace file.
Choose a bounded `--max-frames` on Player or `--max-ticks` on Server for
local diagnostic runs. The host loop captures existing session polling,
Engine updates and session steps. Render publication and CPU time inside
`Renderer::Draw` have separate scopes. The draw measurement covers the
selected backend's submission and blocking time, including any blocking
inside the renderer. GPU time and input-to-display latency need separate
measurements. Network-paced hosts also record the existing wait deadline
and time at the end of the wait. Trace setup and export errors emit one
`[Runtime:WorkTrace]` line on stderr and the packaged runtime retains its
ordinary exit status.

Thread tracks have process-local IDs. The gameplay fixture uses lane 0 for
the server and 1 through 40 for per-client spans. Its aggregate client
samples use lane 1. A frame or tick can correlate scopes across attached
threads. Callers supply the tick and lane and attach a separate scoped
region and work sample on each collecting thread. Automatic worker
attachment and cross-process clocks are outside this trace contract.
Use the existing Tracy zones for surrounding Engine stages and native platform
traces for worker scheduling and thread lifecycle. Exported names come from fixed Engine
tables. No packet body, player name, file path or credential is exported.

Keep source, build optimizer, machine, fixture arguments and output path
policy equivalent when comparing revisions. Run matched Release
measurements with trace support compiled off, compiled on but disabled,
summary enabled, and detailed enabled. Compare process CPU, peak RSS,
allocation counters and frame distributions from the benchmark. A wall
tick average cannot replace the deadline and coalescing counters. Phase
selection filters export records. The existing inclusive timers still run
on sampled frames so excluded child durations can be charged correctly.
Use a narrow tick window to localize expensive phases, then confirm a
candidate change with capture disabled on the same workload.

## Documentation-only validation

Use [lightweight integrity checks](../tools/docs/README.md). They need no native
build, GPU, Node deployment or shared native worker. Follow the existing
cooperative worker policy for sustained native compilation.

Keep failing commands' console output and inspect the first failure. These
steps require no visible helper windows, system-service changes or error dialogs.
