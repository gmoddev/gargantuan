---
status: current
date: 2026-08-23
owner: runtime-assets
related_code:
  - include/gargantuan/assets/AssetTypes.hpp
  - include/gargantuan/services/AssetService.hpp
  - src/assets/AssetImporter.cpp
  - src/services/AssetService.cpp
  - src/filesystem/Project.cpp
  - src/gui/GuiRuntime.cpp
  - src/render/RenderPublisher.cpp
  - src/editor/EditorHost.cpp
  - tests/AssetFoundationTests.cpp
  - tests/AssetFoundationBenchmark.cpp
---

# Asset Foundation 1

## Decision and ownership

Gargantuan exposes one canonical, lazy `AssetService` DataModel service for all
asset kinds. It owns semantic identity, the project catalog, provenance,
canonical content, import state, diagnostics, change sequencing, bounded CPU
ownership, and renderer-neutral consumer resolution. Image, mesh, and font
importers implement the private `IAssetImporter` contract; adding audio,
animation, or materials extends the kind and importer registries rather than
adding another public service.

```text
trusted project-relative source
    -> SourceMount bounded read
    -> AssetService / private importer registry
    -> worker decode, normalize, validate, hash
    -> atomic catalog candidate commit
    -> canonical CPU asset
       -> GuiRuntime image/font resolution
       -> RenderTexture and RenderMesh publication
       -> future type-specific consumers
```

The renderer, GUI object model, and Studio never own importer state. Render GPU
handles, SDL surfaces, `TTF_Font`, and source paths are not public asset
identities. Headless construction, import, catalog loading, metadata lookup, and
mesh/image/font resolution require no GPU device.

## Identity and references

Four identities remain deliberately separate:

| Identity | Contract |
| --- | --- |
| `AssetId` | Stable 128-bit semantic identity for one authored asset. Reimport preserves it. |
| source | Bounded project-relative import provenance used only by trusted authoring/reimport. It is not a reference. |
| `AssetContentId` | Lowercase SHA-256 of the deterministic canonical artifact. Equal canonical output has equal content identity. |
| render identity | Disposable generation-safe `RenderTextureIdentity` or `RenderMeshIdentity`, derived for residency only. |

Serialized references have a closed syntax. Project assets use exactly
`asset://` followed by 32 lowercase hexadecimal digits. Engine-shipped assets
use `builtin://` followed by lowercase path segments containing only letters,
digits, `/`, `_`, and `-`. Raw paths, display names, uppercase aliases, URLs,
GPU identities, and ambient extension lookup are rejected. `ImageLabel.Image`
may additionally be empty to mean no image; `TextLabel.FontFace` always requires
a strict asset reference.

The implemented built-ins are `builtin://image/missing` and
`builtin://font/default`. They pass through the same catalog and resolver paths
as project assets.

## Public surface

The schema-backed runtime Luau surface is intentionally read-only:

- `ResolveAsset(reference: string) -> string?` returns the canonical reference
  only when last-known-good content is available;
- `GetAssetMetadata(reference: string) -> { [string]: any }?` returns bounded
  identity, kind, state, content revision/hash, diagnostic, and kind-specific
  size/count metadata; and
- `IsAssetAvailable(reference: string) -> boolean` recognizes `Ready` and
  last-known-good `Stale` records.

Native trusted consumers additionally use `GetAsset`, `GetCatalog`,
`ResolveImage`, `ResolveFont`, `ResolveMesh`, change-journal reads, and texture/
mesh residency drains. Trusted authoring uses `ImportProjectAsset`,
`ReimportProjectAsset`, and `DeleteProjectAsset`. Import controls and source
provenance are not exposed to ordinary runtime Luau.

## Catalog and lifecycle

Every project record contains `AssetId`, strict reference, kind, display name,
project-relative source, content identity and monotonically increasing content
revision, explicit state, optional bounded diagnostic, dependencies, and the
current canonical value. Foundation 1 dependencies are empty but bounded in the
type so later importers do not need a catalog redesign.

The state model is:

```text
new import:       Importing -> Ready | Failed
reimport: Ready/Stale -> Importing -> Ready | Stale
reload:           persisted record -> Ready | Stale | Missing | Failed
delete:           known unreferenced project record -> removed
```

A successful content-changing reimport preserves `AssetId`, advances
`ContentRevision`, replaces the canonical value in one commit, and queues
consumer residency changes. Identical canonical content clears a stale
diagnostic without advancing the content revision. A failed reimport restores
the previous canonical value, marks it `Stale`, and records a structured
diagnostic. A first import failure remains a visible `Failed` catalog record.
No partial decoded candidate becomes observable.

Deletion rejects built-ins, unknown identities, and project assets referenced by
an `ImageLabel.Image` or text `FontFace`. Successful deletion retires disposable
texture/mesh residency. Broader future consumers must join this reference-safety
check before they expose asset-valued properties.

The in-memory change journal holds 512 records. A reader older than the retained
window receives `RescanRequired`; consumers then rebuild from the catalog rather
than guessing at missed changes.

## Importer registry and canonical representations

`IAssetImporter` selects by explicit `AssetKind` plus a supported lowercase
extension and receives bounded bytes, a cancellation token, and a two-second
deadline. It returns an engine-owned semantic value, versioned canonical
artifact, and SHA-256 content identity. No importer executes embedded code.

Foundation 1 formats and normalized values are:

| Kind | Sources | Canonical value |
| --- | --- | --- |
| Image | PNG and BMP through the pinned SDL_image dependency after header preflight | width, height, tightly packed RGBA8 bytes |
| Mesh | bounded Wavefront OBJ | deduplicated position/normal/UV vertices, triangle indices, generated normals when absent, one submesh, and bounds |
| Font | TTF and OTF validated through in-memory SDL_ttf | immutable bytes and validated face count |

OBJ supports positions, normals, texture coordinates, positive/negative indices,
polygon fan triangulation, and finite-value validation. It deliberately does not
import materials, scenes, rigs, animation, morphs, or embedded resources. Tangent
generation is deferred until a material pipeline requires it.

Canonical artifacts start with `GARGAS01`, carry artifact version 1 and an exact
kind, and encode only the normalized representation. Load verifies the SHA-256,
version, kind, counts, sizes, finite values, normals, indices, and absence of
trailing data before publication. Runtime/reopen consumes these artifacts and
does not decode PNG, OBJ, or font source files. Source files are required only
for authoring reimport.

Source capture occurs through `SourceMount`; decode and validation execute on a
private two-worker `JobSystem`. The caller waits for the bounded worker result,
then performs catalog/DataModel and residency commit on its authoritative caller
domain. At most four imports may be in flight. Cancellation and the deadline are
checked throughout the private importers. A non-blocking EditorHost job/progress
protocol is deferred; the current command remains synchronous while heavy decode
runs off the caller thread.

## Project persistence and relocation

Assets do not inflate the version 4 scene document. The project layout is:

```text
.gargantuan/
  project.json (or the current scene document selected by the project)
  assets/
    catalog.json
    artifacts/
      <64-lowercase-hex-content-id>.gasset
```

`catalog.json` is strict, bounded format version 1. Built-ins, runtime pointers,
GPU state, glyph atlases, and memory-only registration fixtures are not persisted.
Artifact filenames are content identities, so equal canonical content shares one
persisted artifact. The catalog persists semantic identity, kind, relative
source provenance, state/diagnostic, and current content identity/revision.
Absolute host paths never enter an asset reference or catalog record.

`Project::CaptureGame` captures the scene and asset catalog/artifact set at one
authoritative revision. Save atomically replaces each artifact, then the catalog,
then the scene document using the existing same-directory temporary-file path.
Each file has atomic replacement; Foundation 1 does not yet provide a single
directory-wide transaction or garbage-collect artifacts no longer referenced by
the newest catalog. The catalog is written last among asset files, so it cannot
point at a newly captured artifact that has not first been installed.

Open loads only canonical artifacts and marks missing/corrupt content explicitly.
Moving or copying the complete project tree preserves references and allows
reimport because all provenance is project-relative. Local Play clones the exact
captured catalog/artifacts into its isolated DataModel even before Save. Stop
destroys runtime service and residency ownership without mutating authoring
assets; repeated Play receives a fresh clone.

## Cache and residency ownership

Foundation 1 uses three explicit layers:

1. source bytes are request-local and released after import;
2. the canonical CPU working set is owned by `AssetService` and capped at 64 MiB;
3. texture/mesh GPU residency and GUI glyph atlases are disposable derivatives.

The initial policy is bounded admission, not silent LRU eviction. An import,
reimport, snapshot load, built-in registration, or memory-image registration that
would cross the CPU limit is rejected while retaining all prior valid content.
Project deletion and DataModel/Play teardown release canonical ownership. This
keeps references predictable for Foundation 1; usage epochs, pins, preload, and
disposable-residency eviction are Foundation 2 work.

Image residency uses `RenderTextureCreate`, `RenderTextureUpdate`, and
`RenderTextureRemove`. Mesh residency uses generation-safe
`RenderMeshCreate`/`RenderMeshRemove`; `RenderPublisher` retains it for full
renderer resync. The renderer sees normalized bytes/vertices, bounds, revisions,
and render identities only. It never sees an `AssetId`, importer, source, or
project path.

`GuiRuntime` resolves exact image and font references through `AssetService`.
Image changes invalidate only nodes using that image. Font changes include the
content revision in the shaping/font-cache key and dirty only matching text.
Font bytes remain owned by the service while SDL_ttf handles and the glyph atlas
remain GUI-owned disposable state. Missing images resolve to the deterministic
2x2 built-in placeholder; unavailable fonts use the controlled default path.

## SourceMount and security limits

Studio sends only a project-relative source path. EditorHost constructs the
project `SourceMount`; neither Studio nor runtime Luau receives filesystem
authority. Absolute paths, `..` escape, symlinks/reparse points, arbitrary URLs,
and unsupported extensions fail before decode. The source read is revalidated by
the existing mount policy. Engine-packaged default-font configuration is the only
native package path and is not derived from project input.

| Resource | Hard limit |
| --- | ---: |
| Catalog records | 4,096 |
| Source bytes per import | 8 MiB |
| Canonical artifact | 64 MiB |
| Canonical CPU working set | 64 MiB |
| Imports in flight | 4 |
| Name / diagnostic | 256 / 1,024 bytes |
| Source path / dependencies | 4,096 bytes / 256 |
| Image dimension / RGBA bytes | 1,024 per axis / 4 MiB |
| Mesh vertices / indices / submeshes | 262,144 / 1,048,576 / 256 |
| Font bytes | 8 MiB |
| Import deadline | 2 seconds |

Catalog JSON is capped at 1 MiB. Project loading additionally inherits
`SourceMount` aggregate limits. Malformed artifacts, integer/count overflow,
invalid OBJ indices, NaN/Inf values, zero normals, image dimension bombs,
unsupported kind/version, and content-hash mismatches produce bounded failures.

## Studio and EditorHost

The EditorHost handshake advertises `AssetCatalog`, `ImportAsset`,
`ReimportAsset`, `DeleteAsset`, and `StrictAssetReferences`. DTOs contain only
bounded semantic metadata. Asset mutations require `EditorCommands`, reject
during Play or an open transaction, and use the same optimistic project revision
check as other authoring commands. A failed import/reimport that commits a
visible Failed/Stale record returns `OperationSucceeded = false` plus the record,
diagnostic, and updated project state, allowing Studio to reconcile revision and
catalog before presenting the error.

Studio validates catalog version 1 and every identity, state, count, and string
bound into a typed `StudioAssetCatalog`. Session bootstrap and project
replacement refresh it transactionally. The Assets -> Import command accepts
PNG/BMP/OBJ/TTF/OTF selected beneath the current project root, converts the path
to a forward-slash project-relative value, and routes it through
`StudioCommandRunner` and EditorHost. Reimport/delete client and session commands
are present for the future browser. Schema property choices expose matching
image/font references for a basic assignment path; a polished asset browser,
thumbnails, drag/drop copy-in, and selection UI are deferred.

## Verification and measured baseline

`gargantuan_asset_foundation_tests` covers strict syntax and SHA-256, BMP and a
representative PNG, OBJ and font import, logical/content identity separation,
GUI property validation, image/mesh publication, render projection, delete
safety, successful/failed reimport, cancellation, SourceMount escape, image
dimension bombs, cache admission, save/reopen, failed-status persistence,
root relocation, tamper detection, EditorHost capabilities/commands, and two
Play/Stop asset-clone cycles. Existing foundation and GUI suites remain green.
Studio self-tests cover typed catalog parsing and command/capability exposure.

The 2026-08-23 Release benchmark on the development Windows machine measured:

| Case | Result |
| --- | ---: |
| 1,024x in-memory 16x16 image import | 11.75 ms |
| 1,024 / 10,240 catalog lookups | 0.34 / 1.92 ms |
| 1,024 cached image resolves | 0.20 ms |
| Cold 64x64 BMP / OBJ / font import | 0.98 / 0.55 / 9.12 ms |
| 1,024 cached font resolves | 0.10 ms |
| Image reimport / residency drain | 6.05 / 0.0003 ms |
| Catalog/artifact snapshot reload | 1.70 ms |
| Fill 60 MiB then reject over-limit admission | 313.15 ms |
| Published texture bytes / mesh creates | 1,081,360 / 1 |

This is a regression baseline, not a cross-machine performance promise. The
benchmark does not treat filesystem cache timing as deterministic.

## Foundation 2 priorities and accepted gaps

- add an asynchronous EditorHost job/progress/cancellation protocol rather than
  waiting synchronously for the worker result;
- add usage epochs/pins and evict disposable residency before rejecting canonical
  admission, with memory telemetry;
- add a public mesh-consuming Instance such as `MeshPart`; Foundation 1 proves
  mesh normalization and renderer residency but does not add scene semantics;
- add glTF/GLB, material slots/tangents, dependency extraction, and then audio,
  animation, and material kinds through the same service;
- add an asset browser with status, reimport/delete, thumbnail, copy-in, and
  property-assignment workflows;
- add filesystem watching/debounced source-change detection; Foundation 1 hot
  reload is an explicit reimport command;
- make multi-file project persistence directory-transactional and garbage
  collect unreachable content artifacts;
- add package manifests, compression, preload/pins, CDN negotiation, and network
  streaming without introducing raw paths or arbitrary runtime HTTP URLs; and
- expand malformed corpus/fuzzing and sanitizer coverage on supported CI hosts.
