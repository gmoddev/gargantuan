# Production build checkpoint

Checkpoint date: 2026-08-16

## Result

`PRODUCTION BUILD CHECKPOINT READY`

The tested engine source revision is `ad9ff936eeed6857b3caf1ce5aba0f5f00c318b5`
(checkpoint fixes on top of baseline `1080859f593368ce099799b34ddaf164f86cfb68`).
The matching Studio source revision is
`64387e67ff307166f589aad3a69fa480dfdbc8b4`.

This is a production-like developer checkpoint, not a redistributable release.
The installer, signing, updater, asset pipeline, and external distribution work
remain out of scope.

## Validated Windows environment

| Component | Validated value |
| --- | --- |
| OS | Microsoft Windows `10.0.26200`, x64 |
| Visual Studio | Community 2026 `18.4.2` |
| MSVC | `19.50.35728`, x64 tools `14.50.35717` |
| CMake | `4.2.3-msvc3` |
| Ninja | `1.12.1` |
| vcpkg | Visual Studio vcpkg `2025-12-16`, commit `44bb...` |
| Python | Python `3.12.6` for integration tests; SDL_ttf also discovered `3.14.3` on one configure |
| shader compiler | shaderc/glslc `2026.2`, target SPIR-V 1.0 |
| .NET SDK | `10.0.201` (Studio targets `net7.0`) |
| GPU path | SDL GPU Vulkan, AMD Radeon RX 7900 XT, driver 26.7.1 |

No Vulkan SDK environment variable was present. The checkpoint provisioned
`glslc.exe` from the official vcpkg `shaderc` port into a checkpoint-only tools
directory and passed its absolute path explicitly. A normal developer may use
either the Vulkan SDK or another trusted shaderc installation.

## Clean Release configure

Run class generation before CMake because it is not currently a CMake target.
The checkpoint used repository-pinned Lute 1.0.0 and Visual Studio's
clang-format.

The validated configuration was a fresh Ninja directory with these material
arguments:

```powershell
cmake -S . -B build-production-checkpoint -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=ON `
  -DGARGANTUAN_TRACY=OFF `
  -DGARGANTUAN_WITH_GNS=ON `
  -DGARGANTUAN_BUILD_RENDERER_BENCHMARKS=ON `
  -DGARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS=ON `
  -DGARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF `
  -DGARGANTUAN_GLSLC_EXECUTABLE=C:\trusted\shaderc\glslc.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_MANIFEST_DIR=$PWD\cmake\gns `
  -DVCPKG_INSTALLED_DIR=$PWD\build-production-dependencies\vcpkg_installed `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_MANIFEST_INSTALL=OFF `
  -DFETCHCONTENT_BASE_DIR=$PWD\build-production-checkpoint\_deps
```

Use a Visual Studio Developer PowerShell so MSVC's standard-library paths are
present. The vcpkg dependency restore was performed separately from
`cmake/gns/vcpkg.json`; it provides protobuf 3.21.8. The configure then uses
that explicit install tree without a hidden manifest restore.

The second configure used a build-local FetchContent directory. It fetched the
pinned GameNetworkingSockets revision
`2cb93a06350bb065db53abdb0d87cf297e0bfd34` rather than using the repository's
older shared `.fetchcontent-cache` directory.

### Dependency resolution

- Box3D: pinned `vendor/box3d` revision
  `8441b4a06d6d09dcfb0b0f704df4d847d1437b92`.
- SDL: pinned `vendor/sdl` revision
  `f87239e71e42da91ca317a12eefb82cfbf3393eb`, shared SDL3 runtime.
- SDL_image: pinned revision
  `0ee698ef02cf38026c476e28157c182685861d18`; PNG is supplied by the stb
  backend, not its source-mutating vendored libpng path.
- Luau: pinned revision
  `3fc82b1071ab387531175869afc4fb528464afa4`.
- GameNetworkingSockets: pinned FetchContent revision above, static library,
  Windows BCrypt, reference Curve25519, ICE/WebRTC disabled.
- protobuf: vcpkg 3.21.8, dynamically linked by the GNS test executables.
- JSON: production remains nlohmann JSON at pinned revision
  `2e23687092840a66876b4bea48060bd79595ea4b`.
- Glaze: pinned and configured, but the optional serialization prototype stays
  off for production.
- Renderer: default SDL GPU path. Filament remains optional and disabled.

## Shader tools

The default SDL/Vulkan renderer requires `glslc` to compile the four SPIR-V
artifacts. The optional Filament benchmark separately requires the matching
`GARGANTUAN_FILAMENT_ROOT/bin/matc.exe`.

`cmake/ShaderTools.cmake` now:

- discovers `glslc` from `VULKAN_SDK/Bin` or `PATH`, or accepts
  `GARGANTUAN_GLSLC_EXECUTABLE`;
- rejects `matc`, another wrongly named executable, a directory, or a tool that
  cannot execute `--version`;
- discards a cached `glslc` path when its file no longer exists and performs
  discovery again;
- fails configure with the exact supported remedies when no valid tool exists.

CTest includes `gargantuan_shader_tool_validation`, covering wrong-tool,
missing-tool, and valid-tool cases. Neither clean configure contained a stale
temporary shader path or an old build-tree path.

## Release build and tests

The complete default build finished all 1,325 Ninja actions and produced the
engine application, EditorHost mode, all registered test programs, benchmarks,
SDL3.dll, and four SPIR-V shaders.

`ctest --test-dir build-production-checkpoint -C Release --output-on-failure`
passed **16 of 16** tests on both clean builds:

1. shader tool validation;
2. serialization benchmark smoke;
3. Foundation;
4. Box3D physics backend;
5. platform/input boundary;
6. PreRun bootstrap;
7. networking contracts;
8. simulated transport;
9. scheduler contract;
10. replication;
11. remotes;
12. Luau remotes;
13. replication benchmark smoke;
14. remote benchmark smoke;
15. real GNS transport;
16. real GNS remotes.

The only native compiler warning in the final full build was an upstream
HarfBuzz integer-to-pointer cast warning. The checkpoint fixed the engine's
`DiskFilesystem::MapFileOpen` missing-return warning.

## Runtime smokes

- The SDL renderer benchmark ran `mixed`, three objects, two measured frames,
  shadows enabled, and a 320x180 resize round trip. SDL selected Vulkan and
  rendered Block, Ball, and Cylinder geometry before clean shutdown.
- `gargantuan_editor_viewport_smoke` passed initial 64x64 capture, resize to
  32x48, capture after resize, and shutdown.
- A Release Luau script initialized the VM, accessed Workspace and
  ProcessService, exercised Vector3 and CFrame bindings, and exited normally.
- The Release physics, input, serialization, replication, remotes, and GNS
  paths are covered by the registered Release tests above.
- The matching Release Studio smoke opened a project, loaded schema/snapshot,
  selected objects, reconciled authoritative edits through Journal v6,
  captured/resized/picked the viewport, and shut down EditorHost.

Direct Luau assignment of a detached new Instance's `Parent` to a live
DataModel scope is currently rejected by the existing object-reference scope
guard. Other tested property writes work. This is a pre-existing runtime-policy
limitation, not a Release build or staging difference; the checkpoint did not
redesign mutation authority.

## Production-like staging

The validated manual staging layout was:

```text
build-production-staging/
├── Engine/
│   ├── gargantuan.exe
│   ├── SDL3.dll
│   └── shaders/
│       ├── opaque.frag.spv
│       ├── opaque.vert.spv
│       ├── shadow.frag.spv
│       └── shadow.vert.spv
└── Studio/
    └── framework-dependent net7.0 build output
```

Studio receives the Engine executable as an explicit, canonical `--engine`
argument. It does not search the source tree, current directory, PATH, or
uncontrolled DLL locations. The same executable provides `--editor-host`.

The engine stage contains 6 files totaling approximately 6.4 MB. The current
framework-dependent Studio build contains 58 files totaling approximately
125 MB because ordinary `dotnet build` retains native assets for multiple
platforms. A later Windows RID publish can narrow that set; it was deliberately
not turned into packaging work here.

### Runtime prerequisites

Engine runtime:

- Windows x64 with a compatible Vulkan-capable GPU and driver;
- `gargantuan.exe`, executable-local `SDL3.dll`, and executable-local shaders;
- Microsoft Visual C++ runtime (`MSVCP140.dll`, `VCRUNTIME140.dll`, and
  `VCRUNTIME140_1.dll`) installed system-wide.

GNS verification executables additionally require executable-local
`libprotobuf.dll`; GameNetworkingSockets itself is linked statically. The
current ordinary `gargantuan.exe` does not link the optional GNS adapter, so GNS
was validated through its Release lifecycle/remote test executables.

Studio runtime:

- all files and `runtimes` assets emitted by the Studio Release build;
- installed Microsoft.NETCore.App 7.0 runtime for this framework-dependent
  build;
- the explicitly selected matching Engine stage.

Build-only tools include Git/submodules, Lute and clang-format, MSVC, CMake,
Ninja, glslc, vcpkg/protobuf, and Python 3.11+ for the protocol harness. None
were present on the reduced runtime PATH.

The stage launched from `C:\Windows\System32` with PATH reduced to essential
Windows directories. A real Vulkan engine launch and Studio smoke both passed.
The engine resolved shaders relative to its executable, and Studio resolved
EditorHost from the explicit engine argument. Five consecutive engine and five
Studio/EditorHost cycles passed with no remaining processes.

No staged runtime JSON/configuration contained a developer, temporary, old-build,
shared-FetchContent, or checkpoint path. A binary string scan found absolute
source `__FILE__` assertion strings inside the vendored SDL3.dll and the output
PDB path inside GargantuanStudio.dll. These are diagnostic/build metadata, not
runtime lookup paths. No engine or Studio runtime configuration referred to
them.

## Failure behavior

- A missing Studio `--engine` target exits 1 with `Gargantuan executable was
  not found` and the exact requested path.
- A missing required shader now exits 1 during renderer construction with the
  exact missing shader path and SDL filesystem error. Previously it continued
  with null shaders and produced SDL pipeline assertions.
- A missing SDL3 or VC runtime remains a Windows loader-level prerequisite; the
  process cannot provide engine-owned diagnostics before the loader starts it.

## Source and vendor cleanliness

The first clean configure exposed an SDL_image vendored-zlib defect: enabling
the libpng backend configures `external/zlib`, whose CMake script renames its
tracked `zconf.h` to `zconf.h.included` even in an out-of-source build. The file
was verified against the pinned blob and restored. The engine now leaves
`SDLIMAGE_PNG` enabled through SDL_image's stb backend while forcing
`SDLIMAGE_PNG_LIBPNG=OFF`; this also means APNG remains disabled.

Both subsequent clean configures left `vendor/sdl_image`, its nested zlib
revision `0e68590d11e618d60866aa86629fbda128bc068a`, and every other pinned vendor
repository clean. No vendor revision changed.

## Clean-machine assessment

`YES, WITH DOCUMENTED PREREQUISITES`

The staged artifacts plausibly run without the source checkout, build tree,
package caches, Python, compiler, CMake, Ninja, glslc, or Visual Studio. Evidence
is limited to this Windows x64 machine. Another compatible machine still needs
the VC runtime, a Vulkan-capable driver, and .NET 7 for Studio.

## Remaining distribution boundary

### Blocks internal vertical-slice development

None found by this build checkpoint.

### Blocks external developer distribution

- No deterministic CMake install/staging target or checked-in distribution
  manifest exists; the validated stage was assembled explicitly.
- .NET 7 is end-of-support. Studio should move to a supported target before
  external distribution, after a separately scoped compatibility review.
- The framework-dependent Studio build requires a preinstalled .NET 7 runtime;
  the eventual runtime strategy is undecided.
- The VC runtime prerequisite is not bootstrapped or bundled.
- Engine `--version` reports only `1.0` and SDL reports no application
  version/ID; the binary does not expose revision or Release/Debug identity.
- No external redistributable bundle, dependency license inventory, or Windows
  RID-specific Studio publish is produced.

### Safe to defer

- installer/MSI/MSIX, updater, launcher, signing, CDN, and Steam packaging;
- crash reporting and telemetry;
- package size optimization and removal of non-Windows Studio native assets;
- final native project/container format and asset pipeline;
- optional Filament distribution support;
- Playable Vertical Slice implementation.

Studio already exposes assembly version 1.0.0.0 and an informational version
containing its Git revision. Engine build identity remains release technical
debt rather than a checkpoint redesign.

## Reproducibility conclusion

After the fixes above, the checkpoint directory was deleted and configured a
second time. The second configure fetched GNS into its own `_deps`, selected the
same compiler and glslc, built all 1,325 actions, and passed the same 16 tests
without editing a cache or restoring vendor files. The explicit arguments that
matter are the Release/test/GNS options, the trusted glslc path, the vcpkg
toolchain plus GNS dependency tree, and build-local `FETCHCONTENT_BASE_DIR`.

Next milestone: `BUILD PLAYABLE VERTICAL SLICE`.
