Compiling and Testing Gargantuan
Purpose
This document describes the current development workflow for compiling, running, and testing Gargantuan from source.
Gargantuan currently uses:

CMake for project configuration;
Ninja as the generated build system;
Just as the command wrapper;
Lute for generated class sources;
Lest for the Luau test suite;
CCache to accelerate repeated compilation;
Git submodules for several dependencies.

The current build produces a C++23 Gargantuan executable and associated generated/runtime files.

Embedded dependency baselines are intentionally separate:

- the engine pins Luau 0.734 at `3fc82b1071ab387531175869afc4fb528464afa4` in `vendor/luau`;
- Gargantuan Studio embeds its independent NuGet Luau runtime and is not versioned by the engine submodule;
- Lute remains a separate repository-managed tooling runtime.

The engine VM uses the default three-component, 32-bit float vector layout
(`LUA_VECTOR_SIZE=3`, `LUA_VECTOR_DOUBLE=0`). Engine and PreRun compilation set
`lua_CompileOptions.vectorPrecision=0` to match it. Double-precision Luau
vectors require a separate ABI, runtime, and serialization review.

1. Clone the repository
The repository depends on Git submodules, so clone recursively:
git clone --recursive https://github.com/gmoddev/gargantuan.git
cd gargantuan

If the repository has already been cloned without submodules, initialize them with:
git submodule update --init --recursive

The repository also provides:
just submodules

which runs the same recursive submodule update.
2. Required development tools
At minimum, the current workflow expects:

a modern C++ compiler;
CMake;
Ninja;
CCache;
Just;
Git;
the tools configured through the repository's Rokit/Lute setup;
shader compilation tooling;
the platform libraries required by SDL, rendering, fonts, and other native dependencies.

Windows
The project documentation currently targets Windows 10/11. Runtime requires a compatible Vulkan driver; build time requires `glslc`, supplied either by the Vulkan SDK or another trusted shaderc installation.
The Windows build environment must also provide CMake, Ninja, CCache, and the compiler toolchain. A non-PATH shader compiler can be selected explicitly with `-DGARGANTUAN_GLSLC_EXECUTABLE=C:\path\to\glslc.exe`; `matc.exe` is only valid for the separate optional Filament configuration.
Because the Just recipes invoke shell commands, Windows users may occasionally find it easier to execute the underlying CMake commands manually when shell behavior differs.
Run manual compiler builds from a Visual Studio Developer PowerShell/Command
Prompt (or invoke `VsDevCmd.bat`) so the MSVC standard-library include paths are
present.
macOS
The upstream development documentation currently targets recent macOS/Xcode versions.
CMake, Ninja, and CCache can be installed through Homebrew:
brew install cmake ninja ccache

The Vulkan SDK is only required when testing the Vulkan path through MoltenVK.
Linux
A working Vulkan driver is required for graphical execution.
Verify Vulkan first:
vulkaninfo --summary

Example Ubuntu/Debian dependencies from the current project documentation include:
sudo apt install \
  build-essential \
  cmake \
  ninja-build \
  ccache \
  glslc \
  just \
  git \
  curl \
  unzip \
  libfreetype-dev \
  libx11-dev \
  libxext-dev \
  libxcursor-dev \
  libxi-dev \
  libxfixes-dev \
  libxrandr-dev \
  libxss-dev \
  libxtst-dev

The repository documentation also contains package commands for Arch, Fedora, and openSUSE.
3. Install repository-managed tools
The existing development guide uses Rokit for project tools.
From the repository root:
rokit install

If Rokit was just installed, restart the shell if necessary so the executable is visible in PATH.
The build workflow calls:
lute tools/classgen

before invoking CMake's build step. This generates native class/reflection sources required by the engine.
The generator currently launches `clang-format` by name, so `clang-format` must
also be available on `PATH`. On Visual Studio installations it is commonly
under `VC/Tools/Llvm/*/bin`.

This generation step is not yet modeled as a CMake dependency. Running only
`cmake --build` from a fresh checkout can therefore fail or compile stale/missing
generated files; run `lute tools/classgen` first until that bootstrap issue is
fixed.
4. Configure the build
The current Justfile expects the build directory at:
./build

Configure a Debug build with:
just configure

The recipe currently expands approximately to:
cmake -B ./build -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGARGANTUAN_TRACY=OFF \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

A different CMake build type can be selected through the recipe argument:
just configure build_type=Release

Tracy can be enabled during configuration if required:
just configure tracy=ON

After changing important CMake options, compiler settings, or dependency state, reconfigure before rebuilding.
5. Compile Gargantuan
Run:
just build

The current recipe first regenerates class sources:
lute tools/classgen

and then builds using all detected CPU cores:
cmake --build ./build -j=<CPU count>

The executable is expected under the build directory.
The exact executable suffix/path may vary by platform and generator configuration; on Unix-like systems the Justfile currently refers to:
./build/gargantuan

6. Run the engine
To invoke the built executable through Just:
just gargantuan --help

Arguments following the recipe are passed through to Gargantuan.
For example:
just gargantuan --project=./studio

or:
just gargantuan --script=./assets/examples/cube.luau

7. Run example scripts
Run an example without rebuilding:
just run_example cube.luau

The recipe maps the supplied filename beneath:
assets/examples/

and runs Gargantuan with --script.
To rebuild first and then run the example:
just fresh_example cube.luau

Use the fresh_* form when testing C++ engine changes.
Use the non-fresh form when only rerunning already-built code/content.
8. Run EditorHost
The original in-engine Studio prototype and its Just recipes were removed after
Studio moved to a separately authored application. Its MPL-2.0 implementation
remains available through Git history.

Launch the public non-executing document host with a fresh random token:
gargantuan --editor-host --editor-token <random-token>

EditorHost expects bounded protocol messages on standard input. Use the private
Studio client or a protocol test rather than treating it as an interactive shell.
9. Run tests
Run the complete CMake-registered suite from a configured build with:
ctest --test-dir build --output-on-failure

The Release production checkpoint uses:
ctest --test-dir build -C Release --output-on-failure

The legacy `just test` recipe still invokes only `lest run core`; it is not equivalent to the current CTest suite and should not be used as the sole validation for foundational changes.
When changing native runtime behavior, successful compilation alone should not be considered sufficient validation.

The required continuous contract is documented in
`devdocs/CurrentArchitecture/ContinuousIntegration.md`. Every push and pull
request performs a fresh recursive checkout, generates native reflection
sources, configures and builds Release on Windows x64/Visual Studio 2026, and
runs all 20 tests registered by the CI configuration. The workflow invokes
CTest as a whole with `--no-tests=error`; it does not maintain a second list of
test binaries.
10. Recommended development loop
For normal C++ work:
just build
ctest --test-dir build --output-on-failure --no-tests=error

Then exercise the affected runtime path:
just run_example <example>.luau

or rebuild-and-run in one operation:
just fresh_example <example>.luau

For changes involving Studio-facing functionality, build the engine and run the
EditorHost integration test through the normal CTest target.

For changes affecting generated Instance classes, reflection, properties, or services, always use the normal build recipe rather than directly invoking the compiler, because just build runs class generation first.
11. Testing expectations for foundational engine work
The recent architecture/security audit identified native lifetime, parsing, Luau-binding, scheduling, and hierarchy issues. For foundational changes, tests should increasingly include more than the current Lest core suite.
Instance and hierarchy tests
Test:

parenting and reparenting;
self-parent rejection;
descendant-cycle rejection;
parent destruction;
child destruction;
repeated Destroy();
destruction during callbacks;
ancestry signal ordering;
stale object handles;
object ID reuse behavior.

These are particularly important because future networking will depend directly on Instance identity and hierarchy correctness.
Luau boundary tests
Test malformed calls deliberately:

wrong receiver type;
wrong argument type;
missing arguments;
wrong userdata type;
invalid ModuleScript target;
exceptions originating from native code;
non-string Luau error objects.

A malformed script should produce an ordinary Luau/runtime error, not a native crash.
Serialization tests
Test:

valid round trips;
malformed JSON/TOML;
incorrect array sizes;
deeply nested input;
oversized strings/documents;
missing references;
stale references;
temporary-string ownership;
save interruption and atomic replacement once atomic save exists.

Job system tests
When the JobSystem is introduced, test:

one submitted job executes exactly once;
many jobs execute successfully;
jobs can be submitted concurrently;
a fence/group waits for all children;
shutdown drains or cancels work according to documented semantics;
exceptions do not terminate worker threads;
authoritative DataModel mutation from a worker domain is rejected in debug/test builds.

Networking preparation tests
Before actual networking exists, test the primitives networking will consume:

stable ObjectId;
object lookup;
stale-handle rejection;
ordered change records;
create/change/reparent/destroy journal ordering;
schema metadata correctness;
server/client access-policy evaluation.

12. Sanitizers and native diagnostics
The audit recommends AddressSanitizer and UndefinedBehaviorSanitizer where supported.
These should become especially important for:

Instance lifetime tests;
raw/weak pointer transitions;
JSON/TOML parsing;
Luau userdata conversion;
signal callback teardown;
scheduler ownership;
filesystem operations.

The exact sanitizer flags are not currently standardized by the repository's public Just recipes, so they should be introduced as explicit CMake options or presets rather than developers maintaining personal compiler command lines.
A useful eventual workflow would be conceptually:
just configure sanitizer=asan
just build
just test

but this command does not currently exist and should not be documented as working until added to the build system.
13. Headless testing
The CMake-registered CI suite is headless. Foundation, serialization,
scripting, physics, platform input, player runtime, networking, EditorHost, and
protocol tests do not create a display or GPU device. Player tests use
`HeadlessRenderer`; the renderer CI smokes use the existing benchmark's
`projection` and `headless` backends, neither of which initializes SDL video.

The engine target still compiles its required shaders and renderer source. That
is a build requirement, not graphical test initialization. The
`gargantuan_editor_viewport_smoke` executable is built but not run by headless
CI because offscreen pixel capture currently creates a real SDL GPU device.
Optional SDL/Filament GPU execution requires a separate GPU-capable job and must
not be reported as covered by the headless gate.
14. Debug versus Release
Release is the first continuous configuration because it matches the validated
Windows production checkpoint. Debug remains the recommended local development
configuration and is the next matrix expansion after the first gate is stable.
Use Debug during normal engine development:
just configure build_type=Debug
just build

Use Release to catch configuration-dependent failures:
just configure build_type=Release
just build
ctest --test-dir build -C Release --output-on-failure --no-tests=error

This matters because release-only differences have already caused issues in the repository, including code paths affected by NDEBUG.
15. Tracy profiling
Tracy support exists but is optional.
The repository's existing documentation provides commands for building Tracy tools separately:
cmake -B build -DGARGANTUAN_TRACY_TOOLS=ON
cmake --build build --target tracy-profiler tracy-capture tracy-csvexport

Profiling should be used after correctness is established, particularly when developing:

the JobSystem;
task scheduling;
rendering extraction;
physics;
networking replication;
asset loading;
terrain generation.

For the JobSystem specifically, give worker jobs profiler-visible names/categories early. This will make later concurrency problems far easier to diagnose.
16. Before committing foundational changes
For low-level runtime changes, the minimum local verification should be:
just build
ctest --test-dir build --output-on-failure --no-tests=error

Then manually or automatically execute the affected runtime path.
For changes to Instance lifetime, serialization, scripting boundaries, JobSystem behavior, or eventual replication infrastructure, add regression tests before considering the change complete.
A successful build answers only:

"Does this compile?"

The desired standard is:

"Does this compile, preserve runtime invariants, reject malformed input safely, and behave the same way repeatedly?"

17. Known workflow limitations
The current developer workflow is still evolving.
Notable current limitations include:

the `just test` wrapper has not caught up with the current CTest suite;
sanitizer builds are not yet first-class recipes;
the first CI gate does not include Glaze, Filament, or GPU execution;
Debug and non-Windows configurations are not yet continuous gates;
the public development guide has minor command drift relative to the current .justfile;
the ignored generated source trees still require a pre-CMake Lute step rather than a CMake generation target.

When documentation and the Justfile disagree, treat the checked-in build scripts and CMake configuration at the commit being built as the source of truth.
Quick reference
Initial setup:
git clone --recursive https://github.com/gmoddev/gargantuan.git
cd gargantuan
rokit install
just configure

Build:
just build

Test:
ctest --test-dir build --output-on-failure --no-tests=error

Run an example:
just run_example cube.luau

Rebuild and run an example:
just fresh_example cube.luau

Run EditorHost:
gargantuan --editor-host --editor-token <random-token>

Update submodules:
just submodules

List all available Just recipes:
just -l
