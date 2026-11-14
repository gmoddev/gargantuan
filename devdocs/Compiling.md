---
status: current
owner: build-and-test
last_verified: 2026-11-14
related_code:
  - CMakeLists.txt
  - .justfile
  - rokit.toml
  - .github/workflows/native-ci.yml
---

# Build and test Gargantuan

These instructions describe integrated source `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`.
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

## Optional configurations

| CMake option | Default | Effect |
| --- | --- | --- |
| `GARGANTUAN_WITH_GNS` | `OFF` | Pinned GameNetworkingSockets adapter and real-transport tests; enabled in Windows CI |
| `GARGANTUAN_WITH_NODE_CONTENT` | `OFF` | Trusted Server-only Node content adapter; needs canonical Node proto root, Protobuf and gRPC |
| `GARGANTUAN_TRACY` | `OFF` | Instrumented client; compare performance under a declared profile |
| `GARGANTUAN_BUILD_RENDERER_BENCHMARKS` | `ON` | Benchmark targets; physical GPU execution remains separate |
| `GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS` | `ON` | Serialization comparison target |
| `GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE` | `OFF` | Private codec experiment outside production acceptance |
| `GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION` | `OFF` | Focused named geometry tests without shader tooling; graphical/package targets fail closed |

The Engine VM pins Luau 0.734 with three-component float vectors
(`LUA_VECTOR_SIZE=3`, `LUA_VECTOR_DOUBLE=0`, compiler `vectorPrecision=0`).
Studio and Lute have independent runtime pins.

## Documentation-only validation

Use [lightweight integrity checks](../tools/docs/README.md). They need no native
build, GPU, Node deployment or shared native worker. Follow the existing
cooperative worker policy for sustained native compilation.

Keep failing commands' console output and inspect the first failure. These
steps require no visible helper windows, system-service changes or error dialogs.
