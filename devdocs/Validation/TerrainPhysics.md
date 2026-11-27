---
status: qualification
owner: Terrain Foundation 1
last_verified: 2026-11-26
---

# Terrain collider staging and native provider qualification

The Main-domain adapter translates each conditioned chunk into an immutable neutral `PhysicsTriangleMesh`. It creates a hidden anchored body using the existing `PhysicsWorld` admission path and a translation of `ChunkKey * 32 * Spacing`. Chunk-local positions are scaled when the surface is extracted. The provider receives unit mesh scale. Each attempted resource revision has an independent identity within the adapter's namespace so old and staged meshes can coexist in native residency.

A terrain publication first validates the cohort, its candidate payload budget and duplicate keys. Each successful provider creation is recorded before owner-map allocation. If any candidate fails, destruction of the staging handle retires all its admitted candidates and leaves installed ownership and collision active. When storage and other subsystem staging succeed, `Install` retires superseded bodies and installs preallocated owner-map nodes. An empty replacement retires the prior body. A geometry-identical paint updates its installed semantic revision while retaining native collision. Terrain gameplay ownership resolves from the installed `PhysicsBodyId` to the terrain's chunk and revision.

The neutral API creates a live body immediately. The enclosing Stage, commit, and Install or discard sequence runs synchronously at one Main safe point with queries, observers, and simulation excluded for the entire interval. Query users can inspect `IsStaging` and `IsHealthy`. A provider exception before a body handle is returned or a provider that keeps a retired body alive makes the adapter unhealthy. Retirement of owned, unexposed, unconstrained bodies is part of provider compatibility. Teardown retries releases of owned IDs and invalidates outstanding transaction handles before the world is destroyed.

The default adapter caps installed chunks at 512, distinct candidate chunks per cohort at eight, and neutral candidate position and index data at 16 MiB. The provider applies its own resource, shape, canonical-byte, native-byte and coordinate limits to old and staged bodies together. At the verified baseline, Box3D denies transformed positions outside its 100,000-world-unit mesh envelope. This is tighter than the possible integer terrain coordinate range. A denied native coordinate or resource is a publication denial, with coherent prior collision preserved. Per-triangle material IDs are retained in the preceding conditioning output, but the current neutral mesh description and query hit lack per-triangle material and ordinal fields. Native contact friction and restitution therefore remain chunk-wide defaults in this path. [Physics provider contract](../CurrentArchitecture/PhysicsBackend.md), [Surface conditioning](TerrainSurface.md).

## Release results

The focused executable used actual `PhysicsWorld`, `Box3DPhysicsBackend`, `Box3DMeshChunks`, Engine transform and reflection code, and the pinned Box3D provider at `8441b4a06d6d09dcfb0b0f704df4d847d1437b92` with the Engine compatibility patch. Windows MSVC 19.50 x64 compiled with `/O2 /DNDEBUG`. Each qualification assert passed. [Native terrain staging test](../../tests/terrain/physics/TerrainPhysicsStageTests.cpp).

| Case | Observed result |
| --- | --- |
| Adjacent negative and nonnegative chunk keys | Downward rays hit distinct native bodies at the same surface height with upward normals. The installed map returned their owning chunk keys and revision. Sphere cast and overlap found the split surface. A kinematic capsule crossed the seam, stayed grounded and retained an upward floor normal. |
| Actual provider resource-pressure denial | Two resources were installed under a provider limit of three. A two-replacement cohort created its first candidate and failed on its second. After rollback, provider counters showed one creation, one retirement, and one failed admission. Active count stayed at two and both query hits kept their original body IDs. |
| Independent publication discard | Abandoning an admitted candidate reduced active resources from three back to two. The old body and hit survived. |
| Install, paint, and clear | A successful replacement changed the ray height and native body ID and invalidated the old handle. Material-only publication advanced the semantic revision with zero additional native resource creation. Empty replacement removed the affected body and its query hit. |
| Native sliver conditioning | Box3D denied an unconditioned closed eight-face shell. The bounded edge inset kept all faces and the provider admitted the conditioned result. A ray hit its exterior and resolved to the correct owner. |
| Volumetric tunnel across a chunk seam | The conditioned two-chunk cave admitted both colliders. A centerline ray and swept sphere passed through, while an offset ray hit the surrounding rock. Filling both chunks in one revision blocked the center ray and returned ownership at the replacement revision. |
| Outliving stage handle | Adapter teardown retired both installed and staged resources. A later call on the held transaction installed nothing. Provider resources and consumers both returned to zero. |

This test covers focused provider admission, ownership, queryability, and conditioned surface geometry. It leaves full Engine and stock or possessed Character integration, dynamic bodies during excavation, sustained contact continuity across sharp seams, per-material contact coefficients, and full-world timings for separate qualification.

## Reproduction

Compile from the terrain worktree in an x64 MSVC developer shell. `$Includes` points to the pinned dependency headers and the patched Box3D include directory. `$Libraries` selects the corresponding Luau VM, Luau Common, SDL3 import and Box3D libraries. `$Build` is a temporary task-owned directory. Existing dependency outputs were reused without rebuilding the Engine.

```powershell
$Sources = @(
    'src/terrain/PhysicsStage.cpp', 'src/terrain/Surface.cpp',
    'src/physics/PhysicsBackend.cpp', 'src/physics/Box3DPhysicsBackend.cpp',
    'src/datatypes/CFrame.cpp', 'src/datatypes/Enum.cpp', 'src/reflection/Enums.cpp',
    'src/scripting/ScriptSecurity.cpp', 'tests/terrain/physics/TerrainPhysicsStageTests.cpp'
)
cl /nologo /std:c++latest /EHsc /O2 /Gy /DNDEBUG /MD /c /Iinclude /Isrc @Includes @Sources "/Fo$Build/"
$Objects = $Sources | ForEach-Object { Join-Path $Build ((Split-Path $_ -LeafBase) + '.obj') }
link /nologo /NODEFAULTLIB:LIBCMT /OPT:REF /OPT:ICF "/OUT:$Build/TerrainPhysicsStageTests.exe" @Objects @Libraries
& "$Build/TerrainPhysicsStageTests.exe"
```

The recorded executable and object files used `%TEMP%\GargantuanTerrainSurface`. The reused Box3D headers and library were under `%TEMP%\gargantuan-terrain-20261126\build\box3d-compat-source\include` and `box3d-compat-build\src\box3d.lib`. The retained local GUI Foundation Release build supplied SDL3 and Luau libraries. Its SDL3 DLL was available on the test process's path.
