# Standalone Packaging Foundation 1

## Status and boundary

Standalone Packaging Foundation 1 separates an editable project from a portable
runtime package. `PackageBuilder` is the sole packaging implementation used by
the command-line packager and the EditorHost job API. Studio orchestrates that
API; it does not copy files, serialize the project, or define a second package
format.

The validated Foundation 1 platform is Windows x64. The manifest/parser/builder
are platform-neutral and the player has executable-relative `$ORIGIN` and
`@loader_path` loader contracts, but Linux and macOS redistributable packages are
not claimed until their hosted package launches pass.

The authority flow is:

```text
authoritative DataModel revision N + Project + AssetService
    -> immutable GamePayload
    -> PackageBuilder + canonical RuntimeDistribution
    -> validated sibling candidate
    -> finalized runtime package for revision N
```

Packaging is not an authoring mutation. It creates no journal or Undo entry. An
EditorHost package job captures current valid in-memory state, including unsaved
changes, and records both the revision and the unsaved flag. Later authoring does
not alter that immutable payload.

## Durable ProjectId

Each project owns a nonzero random 128-bit `ProjectId`, encoded as exactly 32
lowercase hexadecimal characters in `.gargantuan/project.json` format version 1.
Opening a legacy project without this document performs a one-time identity
migration. Reopen, Save, Save As, relocation, and display-name changes preserve
identity; creating a genuinely new destination creates a new identity.

`ProjectId` is distinct from `AssetId`. It identifies the game for package
inspection and per-game runtime data. It is not an authentication credential.

## GamePayload

`GamePayload` is an in-memory, bounded semantic snapshot:

- ProjectId and display name;
- authoritative revision and whether it differs from the persisted revision;
- the canonical project Instance JSON;
- optional project PreRun schema source;
- a runtime-only AssetService catalog plus immutable canonical artifacts.

It contains no Studio state, EditorHost token, MCP state, source mount, secrets,
or package destination. `PackageBuilder::Capture` obtains project serialization
and assets synchronously from one authoritative world before asynchronous I/O
starts.

The CLI opens the disk project, so its captured and persisted revisions are the
same. Studio uses EditorHost and can deliberately package a valid unsaved
revision.

## RuntimeDistribution

`gargantuan_runtime_distribution` stages the one closed redistributable input
next to `gargantuan-packager` and `GargantuanPlayer`. Its strict
`runtime-distribution.json` version 1 names the platform, player,
`RuntimeCompatibility`, and a sorted list of `Runtime`, `Shader`, and `Notice`
files. PackageBuilder copies only listed files; an unrelated file beside the
distribution is ignored.

The current Windows distribution contains:

- `GargantuanPlayer.exe`, `SDL3.dll`, and CMake's explicit MSVC redistributable
  set (the player's direct compiler-runtime imports are `msvcp140.dll`,
  `vcruntime140.dll`, and `vcruntime140_1.dll`; CMake also selects their bounded
  companion runtime family for the validated toolchain);
- the five default player/controller/camera/ActionMap/interaction Luau modules;
- the engine default font;
- GUI, opaque, and shadow vertex/fragment SPIR-V shaders;
- deterministic Gargantuan and redistributed dependency notices.

SDL_image, SDL_ttf, Luau, Box3D, GLM, JSON, magic-enum, argparse, and Tracy are
linked into the player rather than shipped as separate DLLs in this configuration,
but their notices remain in the package. CMake, compilers, Studio, EditorHost,
tests, importers, Node, and source files are never runtime distribution entries.
Optional telemetry is absent and the player remains fail-open without it. The
Foundation 1 player does not opt into the optional GNS transport.

## Directory package

Foundation 1 deliberately uses a relocatable directory, not a proprietary
single-file archive:

```text
MyGame/
    GargantuanPlayer.exe
    SDL3.dll
    msvcp140.dll
    vcruntime140.dll
    vcruntime140_1.dll
    ...bounded MSVC companion runtime files...
    game.package.json
    runtime/
        DefaultActionMap.luau
        DefaultCamera.luau
        DefaultInteractionRuntime.luau
        DefaultCharacterRuntime.luau
        DefaultLocomotion.luau
        DefaultPlayerRuntime.luau
        GargantuanSans.ttf
    content/
        game.instance.json
        prerun.luau                 # only when the project has PreRun
        assets/
            catalog.json
            artifacts/<AssetContentId>.gasset
    shaders/
        gui.{vert,frag}.spv
        opaque.{vert,frag}.spv
        shadow.{vert,frag}.spv
    notices/
        ...bounded license texts...
```

The stable executable name is `GargantuanPlayer` in Foundation 1. It is not
semantic identity. The player derives package root solely from its executable
location; current working directory is never package authority.

## Package manifest and integrity

`game.package.json` is the package marker and has exactly twelve fields:

- `Format: GargantuanGamePackage`;
- `PackageFormatVersion: 1`;
- `RuntimeCompatibility: 1`;
- ProjectId, bounded display name, Development/Release configuration;
- captured nonzero revision and unsaved-change disclosure;
- relative player path;
- the closed relative startup paths for project, asset catalog, and optional
  PreRun;
- SHA-256 of the deterministic content table;
- sorted content records containing relative path, byte size, SHA-256, and one
  closed category.

Validation rejects unknown top-level fields, unsupported versions, malformed or
unsorted records, duplicates, case collisions, absolute/backslash/colon/traversal
paths, missing required entries, redirected paths, byte-limit violations, and
any size/hash mismatch. Validation also rejects any undeclared file or directory
inside the package. Every required file is hashed before project parsing or
runtime startup. Package inspection exposes metadata only and never script text.

SHA-256 supplies integrity relative to a trusted manifest; it does not establish
publisher authenticity when an attacker can replace the manifest too. Future
manifest signing and OS executable signing are separate work. The content table
and AssetContentIds form the seam for later delta calculation without adding an
updater now.

## Assets and built-ins

Foundation 1 conservatively packages every project asset whose canonical state is
`Ready` or `Stale`; it does not attempt unsafe script reachability analysis.
`Stale` means the last-known-good artifact is included with a structured warning.
`Failed`, missing, malformed, hash-mismatched, or dependency-incomplete assets
block both configurations.

The runtime catalog has only AssetId, AssetReference, kind, name, ContentId,
content revision, state, and dependency AssetIds. It contains no source path,
source group, logical source key, import log, or reimport provenance. Artifact
bytes must hash to their AssetContentId and every dependency must occur in the
closed catalog. Gameplay resolves these bytes through the existing AssetService.

Built-in fallback image/material behavior remains engine-owned. The default font
and default player modules are explicit RuntimeDistribution resources; none use
source-tree lookup.

## Scripts and schema

The project snapshot retains authoritative Luau source because that is the
current persistence/runtime authority. Optional PreRun source is staged
separately and bootstraps the packaged schema without a project filesystem. No
script executes during packaging or `validate`.

Bytecode production, source stripping, protected scripts, obfuscation, and DRM
are deferred. The package startup fields leave a versioned seam for later script
representations without changing authoring authority.

## GargantuanPlayer

`GargantuanPlayer` is a narrow host over the production Engine:

1. derive package root from the executable;
2. validate manifest, versions, every content hash, project envelope, and assets;
3. bootstrap native plus optional packaged PreRun schema;
4. deserialize the DataModel with no authoring filesystem;
5. install the runtime AssetService snapshot;
6. construct the normal Engine, SDL host, renderer, scripts, player, physics,
   input, camera, and GUI loop;
7. destroy runtime state and exit with the game's ProcessService code.

`--headless --startup-smoke` is the bounded CI path. A graphical
`--startup-smoke --max-frames N` exercises the real Windows SDL renderer. These
flags do not weaken validation.

The CTest package smoke enables that graphical launch only when configured with
`-DGARGANTUAN_RUN_GRAPHICAL_PACKAGE_SMOKE=ON`. The default remains headless
because generic hosted runners do not guarantee an SDL GPU backend. Graphical
coverage belongs on a GPU-capable host and a missing backend remains a hard
failure whenever the opt-in is enabled.

Runtime modules, font, and shaders resolve only under executable-relative
`runtime/` and `shaders/`. There is no parent/build/source fallback. Linux uses
`$ORIGIN` and macOS uses `@loader_path` for adjacent shared libraries.

## UserDataRoot

Runtime-writable state is outside the package. `SDL_GetPrefPath("Gargantuan",
ProjectId)` selects a platform user-data directory keyed by durable ProjectId.
Two projects therefore cannot collide merely because their display names or
executable names match. The package itself remains read-only runtime input.

## Development and Release

Both configurations intentionally use the same format, runtime closure, strict
integrity validation, source-free asset catalog, and atomic publication.
Foundation 1 records `Development` or `Release` in the manifest; it does not yet
stage symbols or obfuscate content. This keeps configuration behavior explicit
without creating divergent package authority. Symbols, richer development-only
artifacts, and release size policy are future bounded additions.

## Atomicity, replacement, and cancellation

The builder validates inputs, writes and hashes a unique sibling candidate,
validates the complete candidate, then renames it into place. A recognized prior
package is moved to a sibling backup only during finalization and restored if the
candidate rename fails. An arbitrary nonempty directory is refused. Build failure
or cancellation before commit removes the owned candidate and preserves the prior
valid output. Package operations never recursively delete an unrecognized user
directory.

One EditorHost package job may be active. The UI receives bounded phases:
Snapshot, Validate, Stage runtime, Stage content, Hash/manifest, Finalize, and
Complete. Cancellation is cooperative during bounded streaming copy and before
final commit. Project replacement and EditorHost shutdown request cancellation
and join the worker, so no job retains torn-down engine state.

## Security and resource limits

The format is closed and all parser/copy inputs are bounded. Current principal
limits are 4 MiB manifest, 1 MiB runtime-distribution/catalog documents, 16 MiB
project snapshot, 512 MiB individual content, 8 GiB aggregate content, 16,384
entries, 512-byte relative path, 256-byte display name, 128 runtime dependencies,
64 shaders, and 128 diagnostics. Streaming copies/hash use a 64 KiB buffer.

Symlinks and Windows reparse points are rejected at roots and path components.
Package-relative paths cannot be absolute or escape. Asset provenance is never a
copy source. Windows candidates validate every final and staging path against a
conservative 240-character stream-I/O contract before content staging; names are
never truncated. Diagnostics expose controlled category/code/message/item data,
not exception dumps or arbitrary project paths.

## CLI

The packager and its adjacent `RuntimeDistribution` are sufficient; repository
working directory and `.git` discovery are irrelevant:

```powershell
gargantuan-packager.exe build --project C:\Projects\MyGame --output C:\Builds\MyGame --configuration Release
gargantuan-packager.exe validate C:\Builds\MyGame
gargantuan-packager.exe inspect C:\Builds\MyGame
```

`--runtime <directory>` selects another trusted canonical distribution. The CLI
does not accept individual runtime files or project-controlled copy paths.

## Validation evidence

The automated gate covers deterministic repeat builds, Development and Release,
exact saved and unsaved revisions, concurrent authoring after capture, relocation,
arbitrary working directory, source-free catalog, optional telemetry absence,
package replacement/refusal, cancellation/failure injection at every phase,
path traversal/absolute/case/duplicate/version/count/string/path bounds, modified
project/asset content, missing shaders, stale/failed/missing assets, and isolated
per-ProjectId user data.

The FirstCompleteGame subprocess smoke builds with the CLI, validates its
canonical Audio catalog/artifact alongside its other generic assets, and
inspects, copies the package to another system-temp directory, launches headless
from an arbitrary working directory and a minimal system-only `PATH`, and launches
with the real Windows renderer. The player verifies SDL and the MSVC runtime were
loaded from the package root. The smoke then proves incompatible, shader-missing,
and asset-corrupt packages are rejected. Studio has a separate real EditorHost build
smoke. Packaging benchmarks report capture, asset enumeration, each builder
phase, and size for FirstCompleteGame plus approximately 1K- and 5K-instance
synthetic variants. Disk timings are regression evidence, not an SLA; peak memory
is not yet instrumented.

## Explicit deferred work

- Linux/macOS hosted redistributable launch gates and a published platform matrix;
- installed Studio/Engine distribution assembly;
- symbols and richer Development artifacts;
- reachability pruning, compression, archive/container formats, and package-size
  optimization;
- publisher/package signing, Windows code signing, notarization, and SBOM;
- installers, stores, online publishing, CDN, patch/update services, and rollback;
- protected Luau/bytecode, encrypted assets, DRM, native plugins, mods, dedicated
  server specialization, Node deployment, and mobile packages.
