---
status: experimental
owner: rendering and Studio integration
related_code:
  - assets/services/Lighting.luau
  - assets/classes/Sky.luau
  - assets/classes/ReflectionProbe.luau
  - assets/classes/LocalLight.luau
  - assets/classes/SpotLight.luau
  - src/editor/EditorHost.cpp
  - src/classes/ReflectionProbe.cpp
  - src/assets/AssetImporter.cpp
---

# Renderer Foundation 5 Engine to Studio handoff

This is the Engine contract for the separate Studio integration campaign.
The source baseline is Engine Foundation 5 branch `foundation/renderer-5`,
descended from `94f920485564798742bc22a061cbc3a415034fa3`; the contract was
reviewed at `525be49a0ee7c05096e2ed7bec346db9af228ddb`. The separate HDR
metadata correction is `fd73aec496c31a078d98555070841ce71712af39`.
Studio must bind
against the Engine handshake and schema returned by its running EditorHost,
since a partial or older Engine may have different capabilities. This document
does not change the Studio repository.

## Discovery and commands

`Handshake` reports protocol version, `PropertyBatchVersion`, and capabilities.
Relevant capabilities are `Schema`, `Snapshot`, `SetProperty`,
`SetPropertyBatch`, `AssetCatalog`, `ImportAsset`, `ReimportAsset`,
`DeleteAsset`, `StrictAssetReferences`, `ConfigureViewport`,
`SetViewportCamera`, `CaptureViewport`, `PickViewport`,
`ViewportLatestFrame`, and `ViewportPresentationDiagnostics`. Shared memory
viewport transports are conditional and advertised in `ViewportTransports`;
Base64 RGB8 is the compatibility transport. The latest-frame mailbox uses
BGRA8 when advertised. This F5 slice adds no separate renderer-settings or
texture-residency EditorHost capability.

`GetSchema` accepts discovery version 6 or 7 and returns each class with
constructibility, properties, type, default wire value, editability,
`AtomicBatchWritable`, validation presence, persistence, and replication.
Use the runtime schema identity and declaring definition version returned by
discovery for writes. `SetPropertyBatch` requires the advertised batch version,
project `Scope`, `ExpectedRevision`, a prior snapshot cursor, and at most 256
unique object/property writes. It atomically rejects stale identity, invalid
values, or conflicts. `SetProperty` remains the ordinary single-field path.
Studio should display rejected writes without replacing its last valid value.

`GetAssetCatalog` accepts `IncludeBuiltIns` and `IncludeAdvancedMaterials`.
Catalog version 2 records include `AssetId`, canonical `Reference`, `Kind`,
`Name`, `Source`, `SourceGroupId`, `LogicalKey`, `PrimarySourceAsset`,
`ContentId`, `ContentRevision`, `State`, `Available`, `BuiltIn`, dependencies,
diagnostic, and metadata. Image metadata includes width and height. The
catalog does not expose Image pixel format; do not infer HDR from file name
after import. `ImportAsset` takes project-relative `Source`, optional `Kind`
(`Image` for HDR) and `Name`; `ReimportAsset`/`DeleteAsset` take canonical
`Reference`. Asset operations require `EditorCommands`, a loaded project,
no open transaction, and a current project revision. The response contains
`OperationSucceeded`, primary `Asset`, all `Assets`, `Diagnostic`, and
`ProjectState`. Check `OperationSucceeded`; an operation can return a normal
protocol response with a diagnostic. Catalog reads require `ReadDataModel`,
property writes require `MutateDataModel`, and viewport calls require
`ViewportControl`.

## Authored controls

All listed properties are saved and replicated. Bounds are enforced by the
Engine; Studio may mirror them for controls but must use Engine rejection as
authority.

| Class | Properties, defaults, and accepted range | Studio behavior |
| --- | --- | --- |
| `Lighting` | `ShadowCascadeCount` integer 0, range 0–4; `ShadowDistance` float 250, range 1–4096 | Zero retains the existing fixed directional shadow volume. Expose distance with cascade count. |
| `Lighting` | `Ambient` Color3 (0.2, 0.2, 0.2); `SunColor` (1, 1, 1); `Brightness` 1, range 0–8; `ClockTime` 12, range [0, 24) | Color channels 0–1. Brightness affects direct sun only. |
| `Lighting` | `ExposureCompensation` 0 stops, range -8–8; `ToneMapping` `Filmic` | Exposure is 2 raised to authored stops; use enum discovery for other tone modes. |
| `Lighting` | `EnvironmentColor` (0.08, 0.12, 0.2); `EnvironmentRotation` 0 degrees, range -180–180; `IblIntensity` 1, range 0–8 | Fallback background and global image lighting. Rotation changes sky and IBL together. |
| `Lighting` | `FogEnabled` false; `FogColor` (0.5, 0.6, 0.7); `FogStart` 0; `FogEnd` 1000 | Colors 0–1; distances 0–100000, with ordered start/end validation. |
| `Sky` | `Enabled` true; six `SkyboxPositiveX`, `SkyboxNegativeX`, `SkyboxPositiveY`, `SkyboxNegativeY`, `SkyboxPositiveZ`, `SkyboxNegativeZ` strings, empty by default | Direct `Lighting` child. Use strict Image references from the catalog. An incomplete or incoherent set does not produce a new sky. |
| `ReflectionProbe` | `ProbeEnabled` true; `ProbeCFrame` identity; `ProbeSize` (16, 16, 16); `ProbeCaptureOffset` (0, 0, 0); `ProbeBlendDistance` 1; `ProbeIntensity` 1; `ProbePriority` 0; `ProbeBoxProjection` true | Box pose is rigid and relative to nearest BasePart, otherwise world. Use box gizmo and local capture-origin gizmo. |
| `ReflectionProbe` | Six `ProbePositiveX`, `ProbeNegativeX`, `ProbePositiveY`, `ProbeNegativeY`, `ProbePositiveZ`, `ProbeNegativeZ` strings, empty by default | Assign strict Image references. All six must resolve to ready, matching square faces of the same format. Incomplete source is permitted for editing but contributes no local lighting. |
| `LocalLight` | `LightShadows` false; `LightShadowPriority` 0, range -1024–1024 | Abstract base for PointLight and SpotLight. A request may be unadmitted under the finite shadow budget. PointLight remains unshadowed by this F5 atlas. |

`ReflectionProbe` validation: `ProbeSize` components are 0.1–16384;
`ProbeCaptureOffset` must stay strictly inside half the box extents;
`ProbeBlendDistance` is 0–8192; `ProbeIntensity` is 0–8; `ProbePriority`
is -1024–1024. The CFrame position components are finite and within one
million world units and its rotation must be rigid. Property validation sees
the current box and capture offset, even in a prepared batch. To shrink a box
past the current capture offset, move the offset inward first, then resize.
Existing
`LocalLight` controls remain: enabled true, white linear color, intensity 16
(0–4096), range 16 (0.01–1024), identity pose; SpotLight cone half angles
default (20°, 35°), outer 1–89° and inner 0–outer. The spot points along
negative local Z.

The engine publishes at most 32 probes, prepares up to 12 resident local
sources, and blends up to two per pixel. Priority controls selection before
distance/blend. Budget overflow is an unlit local fallback, not a schema error.
These limits are renderer behavior, not additional public properties.

## HDR assignment and compatibility

Import a square Radiance RGBE `.hdr` (up to 512 × 512) as one Image or a 2:1
panorama (up to 1024 × 512) as a six-Image source group. The source file must
be under 8 MiB. The import response `Assets` order and catalog order are not
a direction contract: map `LogicalKey` values `positive-x`, `negative-x`,
`positive-y`, `negative-y`, `positive-z`, `negative-z` to the six Sky or probe
properties. Reimport preserves each derived AssetId and updates revisions.
Select all six references as a coherent group in one authoring operation when
possible; partially assigned faces remain editable but do not illuminate.
The panorama center faces +Z, the right quarter +X, and the top +Y.

The importer preserves finite linear HDR radiance in RGBA32F and rejects
negative/non-finite RGB. It accepts standard RGBE `-Y +X`, neutral exposure
and pixel aspect, and rejects metadata that would alter color or aspect without
conversion. See [HDR asset contract](HdrEnvironmentAssetsFoundation5.md).
Existing RGBA8 Image artifacts and strict `asset://` references remain valid;
no separate HDR asset kind or GPU object is authored. Existing projects with
no probes, no coherent Sky, or zero cascades retain their fallback lighting.
The renderer's optional instancing and envelope comparison switches are
backend qualification options, not Studio project settings. Renderer LOD and
block-compression authoring have no F5 DataModel or EditorHost contract.

## Compatibility fixtures for Studio

1. Discover schema v7 (fall back to v6) and gate controls by handshake and
   property presence. Open a legacy project with no F5 objects; confirm its
   Lighting defaults and viewport remain usable.
2. Import a 2:1 HDR image, map `LogicalKey` rather than array position, assign
   all six Sky faces, save/reopen, reimport the source, and verify references
   and AssetIds are unchanged while content revisions advance. Repeat with a
   square face and with a malformed or over-limit HDR file; preserve the last
   valid assignments on failure.
3. Create two adjacent colored ReflectionProbes, move the camera through the
   overlap, change priority/rotation/capture offset, save/reopen, and verify
   the volume gizmo and viewport agree. Clear one face and verify its local
   contribution disappears while the global Sky remains. Delete a probe,
   then delete its now-unreferenced project image.
4. Set cascade count 0 then 1–4 and distance extremes; toggle SpotLight
   `LightShadows` and overflow more than eight shadow-requesting spots. Confirm
   overflow lights still illuminate and PointLight does not gain a shadow.
5. Exercise project-revision conflict, stale schema identity, invalid CFrame,
   capture offset outside a resized box, and invalid asset reference. The UI
   should show the Engine diagnostic and retain the last authoritative state.
6. Resize/capture the viewport through advertised transport, and read
   `GetViewportPresentationState` for produced frames, generation, timing,
   and failures. `GetStreamingDiagnostics` concerns play streaming; it is not
   a texture residency or probe budget inspector.

Engine reference fixtures are `AssetFoundationTests`,
`RendererPublicationTests`, `ReflectionProbeSemanticsTests`,
`ReflectionProbeGpuTests`, and `ShadowFoundation5GpuTests`. The Studio campaign
should keep its own EditorHost protocol and persistence fixtures alongside
these Engine tests; backend GPU fixture success alone does not validate Studio
schema wiring or project round trips.
