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

The initial checkpoint establishes branch ownership and contracts. Production behavior awaits integrated test evidence. Qualification worker has exclusive lease generation 686 on HOSTPC via the private fiber connection. The controlling PC remains available. An earlier research capture, renderer fixture, or cached Player executable cannot stand in for the integrated binary.

Next actions are to integrate the domain and grid native commits, exercise coincident domains through real physics and renderer queries, exercise grid mutation, pose, cell hit and serialization, and qualify the assembled artifacts. Subsequent checkpoints own domain-local terrain, affiliation and replication authorization, multiplayer recovery, spatial demand, advanced shapes, transactions, channels, connectivity, moving assemblies, Studio tools, matched benchmarks and an independently reviewed construction sample. Each capability remains open until its normal entry point passes on the relevant platforms.
