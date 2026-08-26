---
status: current
date: 2026-08-23
owner: runtime-assets
related_code:
  - include/gargantuan/assets/AssetTypes.hpp
  - include/gargantuan/classes/MeshPart.hpp
  - include/gargantuan/services/AssetService.hpp
  - src/assets/GltfImporter.cpp
  - src/assets/AssetImporter.cpp
  - src/services/AssetService.cpp
  - src/render/RenderPublisher.cpp
  - src/render/passes/OpaquePass.cpp
  - src/editor/EditorHost.cpp
  - tests/AssetFoundationTests.cpp
  - tests/AssetFoundationBenchmark.cpp
---

# Asset Foundation 2A

## Decision and asset kinds

Foundation 2A extends the single schema-backed `AssetService`; it does not add
type-specific public services. The exact stable `AssetKind : uint8_t` values are
`Image = 0`, `Mesh = 1`, `Font = 2`, `Material = 3`, and `Audio = 4`. Retaining
the first three numeric values is part of artifact version 1 compatibility.
Audio's bounded PCM16/WAV contract is documented in `AudioFoundation1.md`.

One glTF source is a source group containing separately addressable semantic
assets:

```text
project-relative .gltf/.glb
    -> private GltfImporter worker candidate
    -> validate the complete bounded graph
    -> encode and hash every canonical artifact
    -> one authoritative AssetService commit
       Mesh -> Material -> Image
    -> MeshPart -> RenderPublisher -> shared renderer residency
```

No importer type, glTF JSON object, source path, SDL object, or GPU handle enters
the public semantic model. Import and metadata resolution remain headless.

SDL-backed importers run on Gargantuan-owned `std::thread` workers rather than
SDL-created threads. The import job boundary calls `SDL_CleanupTLS` after every
decoder invocation. SDL's pthread TLS key has no thread-exit destructor;
`SDL_Quit` only cleans the calling thread and cannot retire error state left by
an import worker.

## Mesh asset semantics

The canonical mesh is platform-neutral indexed triangle data: finite positions,
normals, tangent `xyz` plus handedness `w`, UV0, unsigned 32-bit indices, bounds,
and one or more primitive ranges. A primitive stores `FirstIndex`, `IndexCount`,
and an optional Material `AssetId`. Primitive ranges are nonempty triangles and
must remain inside the shared index array. OBJ continues to produce one primitive
without a material dependency.

The canonical Gargantuan coordinate convention for imported meshes is
left-handed, +Y up, -Z forward, and one source unit equals one Gargantuan world
unit. glTF is right-handed, +Y up, +Z forward. Import reflects position, normal,
and tangent Z once; reverses each triangle's winding; and negates tangent `w`.
Runtime instances and the renderer perform no second conversion. UV0 is retained
in glTF's normalized convention. Missing normals are generated from canonical
winding. Missing tangents are generated from UV0 when possible; a normal-mapped
primitive without valid non-degenerate UV tangent space is rejected.

## Material asset semantics and rendering

`ImportedMaterial` is the persisted Gargantuan-owned model:

- `BaseColorFactor : vec4`, initially `(1,1,1,1)`;
- optional `BaseColorTexture : AssetId` with `ColorSrgb` intent;
- `MetallicFactor` and `RoughnessFactor`, initially `1`;
- optional `NormalTexture : AssetId` with `LinearData` intent;
- `AlphaMode` (`Opaque`, `Mask`, or `Blend`), `AlphaCutoff`, and `DoubleSided`.

`builtin://material/default` supplies the deterministic fallback through the
same resolver. The material value remains distinct from disposable
`RenderMaterialState`.

The SDL renderer currently applies base-color factor/texture, per-instance
BasePart tint/transparency, alpha mask cutoff, alpha blending, and double-sided
culling. It carries metallic, roughness, and normal-texture identities through
the renderer-neutral publication and projection contracts, but the current
Lambert shader does not yet evaluate metallic-roughness BRDF or normal mapping.
Those values are preserved rather than claimed as visually implemented.
RGBA8 remains the one decoded image representation. Texture use records sRGB
versus linear intent; actual sRGB GPU formats are deferred, so no public API
equates RGBA8 storage with a color-space transfer function.

## glTF importer choice and supported subset

The importer uses the repository-pinned `nlohmann_json` 3.12.0 parser (MIT) and
the existing SDL_image decoders. A separate glTF dependency was not added: the
required static subset is small, the repository already pins a bounded JSON
front end with depth/node/string validation, and a private converter validates
all glTF tables and byte ranges. This avoids exposing a parser framework or
adding a scene engine. All glTF-specific types stay in `GltfImporter.cpp`.

Supported glTF 2.0 input is:

- `.glb` version 2 JSON/BIN chunks and `.gltf` JSON;
- relative project-contained buffers and PNG/JPEG/BMP images;
- bounded base64 buffer/image data URIs;
- embedded GLB buffers and bufferView images without temporary files;
- static triangle primitives using POSITION and optional NORMAL, TANGENT, UV0;
- non-normalized unsigned byte/short/int indices;
- float POSITION/NORMAL/TANGENT and float or normalized unsigned byte/short UV0;
- multiple meshes, primitives, materials, images, and node/scene metadata that
  does not alter the resource semantics;
- metallic-roughness factors, base-color texture, unit-scale normal texture,
  alpha mode/cutoff, and double-sided state.

Explicit rejection covers required extensions, compressed primitives, unknown
vertex channels, non-triangle modes, sparse accessors, morph targets/weights,
skins, animation, texture/material extensions, explicit samplers, non-UV0
texture coordinates, metallic-roughness textures, occlusion, emissive inputs,
scaled normal maps, invalid tangent handedness, and unsupported component/type
combinations. Full scene/prefab creation, skinning, morphs, animation, custom
attributes, MTL, and arbitrary material graphs are not Foundation 2A features.

## Source authority and security limits

External discovery runs before decode. Every non-data URI is canonicalized and
read through the source file's project `SourceMount` directory. Empty/oversized
URIs, backslashes, absolute paths, drive/colon forms, percent-encoded ambiguity,
`file:`, HTTP(S), and `..` escape fail. URI recursion and remote fetching do not
exist. Embedded bytes remain owned memory.

The relevant hard bounds are:

| Resource | Limit |
| --- | ---: |
| source / JSON / one data URI | 8 MiB / 4 MiB / 8 MiB |
| GLB chunks / external resources | 8 / 256 |
| buffers / bufferViews / accessors | 256 / 4,096 / 4,096 |
| meshes / primitives | 1,024 / 4,096 |
| materials / images / textures | 1,024 each |
| generated assets / dependencies per asset / graph | 1,024 / 256 / 8,192 |
| diagnostics / diagnostic bytes | 64 / 1,024 |
| canonical vertices / indices / primitives | 262,144 / 1,048,576 / 256 |
| one and aggregate graph artifacts | 64 MiB |
| decoded image / dimension | 4 MiB / 1,024 per axis |
| canonical CPU working set | 64 MiB |

JSON inherits the common 64-level and bounded-node/string validation. Checked
offset/stride/count arithmetic rejects overflow and out-of-bounds access. NaN,
Inf, invalid ranges, invalid indices, zero normals/tangents, degenerate triangle
or tangent space, excessive names, cycles, duplicate identities, and excess
edges fail the complete graph. Importers execute no embedded code.

## Compound transaction and stable child identity

Every import allocates one `SourceGroupId`. Each generated record carries that
group, a bounded `LogicalKey`, and exactly one group record is
`PrimarySourceAsset`. Reimport may be invoked with any child; the primary kind
and common project-relative source select the importer.

Logical keys never use display Name. Mesh keys hash primitive vertex-semantic
and accessor topology; material keys derive from their sorted mesh/primitive use;
external images use their relative URI, while embedded/data images derive from
their sorted material use. Duplicate indistinguishable keys receive a
deterministic rank. Reordering mesh/material/image arrays while preserving those
uses therefore retains identities; changing accessor topology or the first use
is a semantic rematch boundary. An existing logical key reuses its `AssetId`; a
new key receives a deterministic child ID derived from group plus key; a missing
key is retired during the same commit if references permit it.

Candidate bindings are resolved only after all keys exist. AssetService validates
kind-compatible edges, deduplicates them, rejects self/cyclic/missing targets,
encodes every final artifact, checks aggregate CPU/artifact admission, and only
then changes the catalog. Any failure produces zero records for a first compound
import. A failed reimport restores the complete previous graph as last-known-good
`Stale` content with one bounded diagnostic.

## Dependency graph, revisions, and deletion

The catalog stores explicit dependent-to-dependency `AssetId` edges; the
renderer never discovers them by scanning strings. Mesh primitive material IDs
and material image IDs agree with those edges and are verified on load.

On reimport, directly changed canonical artifacts advance their stable record's
`ContentRevision`. Dependency-edge changes also advance that record. The service
then walks the acyclic graph and advances only transitive dependents. Unrelated
records keep their revision. The bounded semantic change journal publishes
`Added`, `Removed`, `ContentChanged`, `MetadataChanged`, `DependencyChanged`, or
`StateChanged`, and MeshParts referencing the changed chain are marked only in
the geometry/material domains that changed.

Deletion is source-group atomic. It rejects built-ins, in-flight groups, any
outside catalog dependent, and exact scene references from ImageLabel,
TextLabel/font use, or MeshPart mesh/material properties. A disappearing child
during reimport applies the same checks. There is no force-delete path and no
silent dangling reference. Destroying or duplicating a MeshPart affects only
scene ownership; it never deletes shared asset records.

## MeshPart

`MeshPart` is a schema-backed `BasePart` with two authored properties:

- `Mesh : string` is empty or a strict Mesh asset reference;
- `Material : string` is empty or a strict Material asset reference.

Both are serializable and replicated and advertise `AssetReference:Mesh` or
`AssetReference:Material` editor hints. Empty Material uses imported per-primitive
materials, falling back to `builtin://material/default`; nonempty Material is a
whole-mesh override. Per-slot instance overrides are deferred. Missing mesh or
material content has deterministic no-draw/fallback behavior and never reads a
source file from the Instance. MeshPart inherits transform, visibility, color,
transparency, and current BasePart behavior. Physics deliberately remains the
safe inherited box shape; render triangles are not injected into Box3D.

## Renderer residency and sharing

AssetService maps stable `AssetId` to a disposable generation-safe
`RenderMeshIdentity` or `RenderTextureIdentity`. The current revision replaces
content through residency changes without persisting renderer identity. Every
MeshPart using one Mesh publishes the same mesh identity and retains only its
own object transform/material presentation. Per-primitive material state is a
shared immutable publication list, and texture pixels stay in texture residency
rather than being copied into each object every frame.

Full resync recreates retained mesh/texture resources and all MeshPart objects.
Incremental mesh reimport updates affected geometry users; image -> material ->
mesh propagation updates only affected presentation and shared texture state.
Stop and renderer teardown release disposable object/GPU ownership while the
authoring catalog remains intact.

## Persistence and Play

Catalog format 2 adds dependencies, source grouping, logical keys, and primary
ownership. Artifact format 2 adds Material and mesh primitive/material records.
Load explicitly accepts catalog 1 and artifact 1 for Foundation 1 Image, Mesh,
and Font data, synthesizing a one-record source group and empty dependencies.
Material requires artifact 2. Writers always emit version 2 and never silently
reinterpret an unknown version.

Save/reopen and project relocation preserve strict references because the
catalog stores only project-relative provenance. Local Play clones the exact
in-memory catalog 2 and artifact graph, including unsaved Material/Mesh revisions
and MeshPart properties. Runtime never needs source files. Asset mutation remains
outside the existing scene Undo/Redo binary history; Studio routes it as one
authoritative mutation and reconciles catalog/project revision from EditorHost.

## Studio and EditorHost workflow

EditorHost catalog version 2 provides bounded kind-specific metadata, primitive
count, material references, dependency IDs, `SourceGroupId`, `LogicalKey`, and
primary status without vertices or pixels. Compound mutations return nullable
primary `Asset` plus the complete `Assets` list. Atomic first-import failure has
no partial asset record; stale reimport returns the previous complete group.

Studio's one Asset Catalog accepts Image, Mesh, Font, Material, and Audio. Import now
offers `.gltf`/`.glb`; Catalog groups generated children by source, shows kind,
logical key, dependencies, and diagnostics, and reimports/deletes the complete
source group through the shared command runner. Schema asset-reference editors
filter choices by requested kind, so MeshPart Mesh and Material cannot be
cross-assigned. The UI adds no separate mesh/material browser and no thumbnail
system.

## Verification and Foundation 2B

The headless asset suite covers GLB, external and data `.gltf`, compound atomicity,
stable/add/remove children, dependency propagation, deletion constraints,
coordinate conversion, materials, shared MeshPart residency, selective updates,
corrupt reimport, Play clone, save/reopen/relocation, catalog/artifact v1 loading,
v2 persistence, tamper failure, traversal/absolute/remote URIs, malformed GLB/
JSON, out-of-range accessors, invalid indices, non-finite data, unknown semantics,
and limit overflow. The test is part of the complete ASan/UBSan CTest contract.

The Release benchmark reports source JSON parse, canonical candidate conversion,
artifact serialization, SHA-256, dependency graph build, cold small/medium/
four-material GLB import, unchanged and texture-changing reimport, 1,000 shared
MeshParts with mixed material overrides, full resync, dependency lookup, and
snapshot reload. Filesystem-cold timings are machine observations, not portable
budgets.

The 2026-08-23 Windows Release observation was:

| Case | Time |
| --- | ---: |
| source JSON parse / canonical graph conversion | 0.36 / 1.52 ms |
| artifact serialization / SHA-256 / graph build | 0.04 / 0.35 / 0.01 ms |
| cold small / medium / mesh+4 materials+textures GLB | 1.06 / 3.55 / 2.23 ms |
| unchanged / one-texture-changing reimport | 2.22 / 11.63 ms |
| create 1,000 shared MeshParts | 238.07 ms |
| 1,000-object publication / full resync | 4.01 / 2.90 ms |
| dependency lookup pass / snapshot reload | 7.00 / 2.87 ms |

The conversion observation invokes the complete private importer candidate path;
the standalone parse measurement isolates the pinned JSON front end. The 64 MiB
bounded-admission benchmark still rejected the 16th 4 MiB image as designed, so
2A does not change the cache policy. The shared 1,000-object result gives no
evidence that canonical 64 MiB admission is itself impractical; 2B should add
residency telemetry before choosing eviction policy.

Asset Foundation 2B priorities are: implement shader-correct sRGB/linear sampling,
normal mapping and metallic-roughness rendering; add explicit material sampler
semantics if required; add async Studio import progress/cancellation; add usage
epochs/pins and disposable residency eviction driven by measured 64 MiB pressure;
add per-slot overrides only with a demonstrated authoring need; design a separate
mesh-collision asset boundary; add artifact garbage collection/directory-wide
save transactions; and expand glTF fuzz corpus plus repeated sanitizer runs.
