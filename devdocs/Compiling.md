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
The project documentation currently targets Windows 10/11 and requires the Vulkan SDK for the supported graphics path.
The Windows build environment must also provide CMake, Ninja, CCache, and the compiler toolchain.
Because the Just recipes invoke shell commands, Windows users may occasionally find it easier to execute the underlying CMake commands manually when shell behavior differs.
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
The build itself calls:
lute tools/classgen

before invoking CMake's build step. This generates native class/reflection sources required by the engine.
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
8. Run Studio
Run the current Studio project without rebuilding:
just run_studio

Rebuild first and then run it with:
just fresh_studio

Studio is currently a development scaffold rather than a production editor, so it should not be treated as a comprehensive engine validation path.
9. Run tests
Gargantuan's current test suite uses Lest.
Run all currently wired tests with:
just test

At present the Justfile defines test in terms of the core test target:
just test_core

which performs a build and then runs:
lest run core

Therefore, just test currently tests the core suite rather than representing a large collection of independent subsystem suites.
When changing native runtime behavior, successful compilation alone should not be considered sufficient validation.
10. Recommended development loop
For normal C++ work:
just build
just test

Then exercise the affected runtime path:
just run_example <example>.luau

or rebuild-and-run in one operation:
just fresh_example <example>.luau

For changes involving Studio-facing functionality:
just fresh_studio

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
The audit recommends separating enough engine functionality from graphical startup that core tests can run without creating an SDL GPU device or display.
This is important for:

CI;
server builds;
serialization tests;
scripting tests;
JobSystem tests;
networking protocol tests;
Linux build agents;
sanitizer/fuzzing jobs.

A future headless target should not require shader compilation or GPU availability merely to test the object model.
14. Debug versus Release
Both configurations should eventually be tested continuously.
Use Debug during normal engine development:
just configure build_type=Debug
just build

Use Release to catch configuration-dependent failures:
just configure build_type=Release
just build
just test

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
just test

Then manually or automatically execute the affected runtime path.
For changes to Instance lifetime, serialization, scripting boundaries, JobSystem behavior, or eventual replication infrastructure, add regression tests before considering the change complete.
A successful build answers only:

"Does this compile?"

The desired standard is:

"Does this compile, preserve runtime invariants, reject malformed input safely, and behave the same way repeatedly?"

17. Known workflow limitations
The current developer workflow is still evolving.
Notable current limitations include:

tests are concentrated in the core Lest suite;
no standardized headless CI target is yet exposed through the Justfile;
sanitizer builds are not yet first-class recipes;
the public development guide has minor command drift relative to the current .justfile;
some platform claims are broader than the currently demonstrated CI coverage;
build/test infrastructure is one of the explicit targets of the new architecture roadmap.

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
just test

Run an example:
just run_example cube.luau

Rebuild and run an example:
just fresh_example cube.luau

Run Studio:
just run_studio

Rebuild and run Studio:
just fresh_studio

Update submodules:
just submodules

List all available Just recipes:
just -l
