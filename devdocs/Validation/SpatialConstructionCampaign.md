# Spatial Domains and BlockGrid production ledger

## Integration base and ownership

This campaign started on 2026-11-27. Integration uses `production/spatial-construction-production-20261127` in `~/.codex/worktrees/spatial-construction-production-20261127`. The fork point is `284443879fcecc66a0257006fcdf7d65566fef4d` from the unified Engine/Studio promotion candidate. Remote main was `ff5cf28f8` at reconciliation. The candidate includes the integrated Studio host and character walking change. The Terrain headers and source match the qualified Foundation 2 tree at `ea223efe7`. Results against the original research executables do not qualify this new binary.

| Owner | Branch or location | Current responsibility |
| --- | --- | --- |
| Domain worker | `production/spatial-domain-production-20261127`, sibling worktree `spatial-domain-production-20261127` | SpatialRegion, membership, admission, native world lifetime, view selection, tests |
| BlockGrid worker | `production/blockgrid-production-20261127`, sibling worktree `blockgrid-production-20261127` | Cell field, semantic mutation, schema and scripting, storage and artifact round trips |
| Qualification worker | Supervisor worktree, `devdocs/Validation/SpatialConstructionQualification.md` | Exclusive heavy worker lease, MSVC and Linux builds, immutable-artifact provenance, normal entry-point tests |
| Supervisor | Integration branch | Native derived grid geometry, physics owner resolution, picking, integration, project lifecycle, subsequent lanes, ledger |

All three production branches forked at the integration base. Use `git rev-parse <branch>` to recover live tips. The engine consolidation, installer, original 3L checkout, and PreRun worktrees have independent owners. Preserve their outstanding changes. Local promotion only.

## Accepted contracts

- Research at `b0851557b` under `research/spatial-regions` establishes independent coordinate domains in one DataModel. Each applicable subsystem resolves effective domain authority before admitting resources. A normal Workspace is the default.
- Domain-local code can iterate directly. The 128-world-unit server spatial buckets supply conservative candidates. Resource residency can be selected separately for each subsystem. Direct iteration, index maintenance, and residency have independent costs.
- Research at `fd105f2a6` under `research/block-grid` establishes one creatable grid with native signed cells. A single grid can cover a world. Developers may create additional grids for independent identity or movement. Chunk layout remains private and local.
- First integration uses built-in Stone and Brick definitions, signed half-open regions, and per-field revisions. The rendering and collision adapters consume ordinary field data through the same authority as Luau. A rigid pose change preserves local cells and topology.
- Multiplayer disclosure and controller admission require separate authority. Native renderable or queryable resources alone do not grant either.

## PreRun handoff

The integration baseline evaluates the project schema in an isolated PreRun VM before constructing the new DataModel. The independent `prerun-explorer-design` worktree is designing ordered sources. The host owns source ordering, schema publication, error recovery, and world replacement. The domain and grid lanes consume the resulting schema after DataModel construction. They require no additional PreRun entry point or mutable global registry.

Native domain handles, Box3D bodies, grid caches, and render publications belong to the current world generation. Edit-to-Play cloning and reopening may transfer only authored data, object reference fixups and canonical grid contents. On Stop the edit world must retain its original cells and affiliation. Validate successful and failed PreRun, Play edits, Stop, save/reopen, duplication and offline package startup on the eventual qualified host revision. The active multi-source design was uncommitted at reconciliation.

## Checkpoint and pending qualification

The first production field commit is `fde9e6981` in the BlockGrid lane, integrated as `315a601f1`. Its lightweight MSVC Release executable passed 1,000 seeded edits, signed coordinates, representation transitions, stale revision and no-op cases, rejected provider admission and malformed input. These are field and codec results. The Windows normal-project worker began a full core build and the Linux worker began sanitizer checks.

The supervisor worktree now contains the integration adapters under development. Native merged box colliders retain their cell ranges for hit attribution. Mixed world queries require `RaycastParams.IncludeBlockGrids`; Character motion uses collision participation directly. Native GAS binaries and bounded text chunks in JSON carry field state on trusted persistence paths. The older v5 serializer requires explicit saved reference properties; the default-domain migration accepts a missing nullable SpatialRegion membership alone. Domain and BlockGrid network materialization needs a delivery/admission protocol. Startup and mutation reject unsupported content under the existing network profile while offline integration proceeds.

The initial checkpoint establishes branch ownership and contracts. Production behavior awaits integrated test evidence. Qualification worker has exclusive lease generation 686 on HOSTPC via the private fiber connection. The controlling PC remains available. An earlier research capture, renderer fixture, or cached Player executable cannot stand in for the integrated binary.

## November 27 integrated checkpoints

The D1 region and Workspace physics owners, explicit nullable member affiliation, routed rendering and detached-view cleanup are integrated. Grid code now covers native edits, bulk revision checks, cell attribution through merged static colliders, signal delivery, render publication, GASM, project persistence and native undo. Engine EditorHost integration is at `d10ecb5c6`. The Studio inspector and grid-aware viewport are on independent branch `production/spatial-construction-studio-20261127` at `b2d7572`, based on `7bff715` in sibling Studio worktree `spatial-construction-studio-20261127`. Five Studio Luau sources passed bytecode compilation. Foreground authoring still needs verification.

The Windows worker built integration commit `b5f9b3a03` and passed native Box3D domain isolation, grid semantic hits, render publication, clone, project save/reopen, field tests and reference compatibility. It confirmed the detached-view fix. Six offscreen Vulkan frames at 192 by 128 changed 874 pixels on placement and 1,771 on movement. Hiding and destroying left zero residual pixels and showing again exactly reproduced the moved image. The binding test constructed an invalid test authority and failed before the intended provider denial. The corrected test is integrated at `9b3089e5a` and awaits its paired run. See the qualification ledger for artifact pins and run outputs.

The `32ad79ad6` checkpoint adds a headless EditorHost workflow and actual package staging fixture. Domain-local Terrain field ownership entered at `fa8a21a97`, grid affiliation at `2201f4c46`, live independent ViewportFrame publications at `f8b729c22`, and bounded terrain owner bindings for projects, Play, GAS places and offline packages at `cb8db611a`. The new snapshot preserves existing versions for places without authored domain Terrain. Queue the latest binary for paired Windows and Linux tests, two differently edited coincident terrains, swapped manifest rejection, Play isolation and packaged Player startup.

The grid worker owns definition catalogs, partial geometry, exact shape picking and the BlockGridWorld collision adapter. The domain worker owns live views, domain-aware Studio terrain and demand. The supervisor owns the grid rendering adapter, project and package bindings, creator sample and cross-lane integration. Qualification owns the exclusive build lease, process-level tests and independent regression review. Domain-aware network delivery, bilateral readiness, resource demand, custom shapes, native state channels, connectivity, dynamic assemblies, end-to-end sample, matched performance and independent documentation trials still require implementation or qualification. The temporary default-only network admission profile explicitly rejects independent domains and grid content.
