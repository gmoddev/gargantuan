# Terrain Foundation 1 active metadata qualification

Base Engine revision `ff5cf28f8`. The Release probes compare four private data layouts for one metadata record per active chunk. Inactive coordinates use a separate sparse directory. The selected implementation is the private [`ActiveChunkRegistry`](../../include/gargantuan/terrain/ActiveChunkRegistry.hpp). Chunk source data remains owned by `Field` and terrain Instances retain DataModel authority.

## Existing Engine patterns and upstream evidence

| Source | Design evidence and terrain decision |
| --- | --- |
| [Runtime Foundation 6 and current ownership](../CurrentArchitecture/FoundationRuntime.md#runtime-foundation-6-storage-and-change-distribution-disposition) | The Engine retained Instance fields after an end-to-end single-consumer SoA experiment failed to repay synchronization. Consumer projections remain useful when they eliminate measured work. Terrain's bulk lattice and derived scheduling records start outside Instance fields. Applying a component model to the whole Engine would cross the established authority boundary and needs independent evidence. |
| [Spatial runtime projection 3K](../CurrentArchitecture/SpatialRuntimeProjectionFoundation3K.md), [core invariants](../../docs/invariants/Core.md) | Derived space records and acceleration memberships have separate ownership. Prefer a terrain-owned key/coverage index for working-set selection and use existing relevance policies at the authority boundary. One stable signed-coordinate key answers where the chunk lives. Its current activation generation answers which live record can receive work. |
| [JobSystem](../../src/runtime/JobSystem.cpp), [execution domain](../../src/runtime/ExecutionDomain.cpp) | The shared job queue grows to accept submissions, so terrain's independently bounded admission and result queues must control work and captured bytes before submission. `JobGroup` pairs completion with worker exceptions. Workers consume immutable input and return owned results. Main applies still-current results within its own budget. |
| EnTT 4.0.0, 23 July 2026 | Packed sparse sets map entity slots to dense component storage. Component pool mutation policies include swap-and-pop. Sparse-set style is attractive when optional components are independent and a single small pool drives query iteration. A multi-component chunk read can require several joins. The simplified probe compares this layout pattern. Upstream is MIT-licensed. [Tagged source](https://github.com/skypjack/entt/blob/v4.0.0/src/entt/entity/sparse_set.hpp), [release](https://github.com/skypjack/entt/releases/tag/v4.0.0), [license](https://github.com/skypjack/entt/blob/v4.0.0/LICENSE). |
| Flecs 4.2.0, 4 November 2026 | Identical component sets share archetype tables, allowing cached queries to skip unrelated tables. Add/remove can move a row between tables and update its index. Component values can also represent frequently changing states without moving rows. The probe uses fixed expected role combinations and indexed swap removal. Upstream is MIT-licensed. [Tagged table source](https://github.com/SanderMertens/flecs/blob/v4.2.0/src/storage/table.c), [queries](https://www.flecs.dev/flecs/Queries.html), [release](https://github.com/SanderMertens/flecs/releases/tag/v4.2.0), [license](https://github.com/SanderMertens/flecs/blob/v4.2.0/LICENSE). |
| Unity Entities 1.4 | The archetype and chunk design illustrates fixed-size allocation, cached matching, and structural rebuild costs. Enableable components provide a separate state-change path when archetype transitions would be frequent. Product-specific API and implementation are reference materials. [Archetype docs](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-archetypes.html), [enableable component docs](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-intro.html). |

EnTT and Flecs are feasible dependency candidates under their respective MIT notice requirements. Their portability, integration, and license review would still be required for adoption. The probes ran independently authored layout models. Results compare storage patterns rather than benchmark or rank the upstream libraries. With a small known role schema and a stable field owner, a new dependency offers little immediate leverage.

## Matched layout experiment

[MetadataBenchmark.cpp](../../tests/terrain/metadata/MetadataBenchmark.cpp) compares these layouts with identical state transitions and checksums.

| Layout | Packed columns and lookup | Role changes |
| --- | --- | --- |
| Specialized SoA | One indexed record with hot geometry/material revisions plus nullable render and collision refs | Rewrite values in place |
| Sparse sets | Independent geometry, material, render and collision component pools | Insert or swap-remove optional component rows |
| Archetype tables | Four predeclared ready-role combinations with packed columns and index to the row's table | Migrate records when the ready mask changes |
| Hybrid | One packed hot revision table with independent sparse sets for optional roles | Update hot row and optional memberships |

The 1,024-active balanced fixture populated both ready-role flags at 50%. The 8,192-active skewed fixture maintained 50% render readiness and 6.25% collision readiness. Both used 16 inactive directory entries per active entry. The large cold catalog exceeds the default production field directory cap to stress comparative metadata scaling. Every round used the same signed-coordinate directory probes and generated traces across candidates. The shared fixtures issued 2,048 direct and 2,048 keyed lookups, 256 ready-role toggles and restores, 4,096 revision changes arranged in neighboring groups, and 128 retire/recreate cycles. A bounded ring retained deferred dirty keys until admission and admitted at most 1,024 immutable tickets per wave. The benchmark measures full discovery of pending dirty keys as a separate supplemental scan. Main rejected revoked tickets after lifecycle churn.

Build and execution used x64 MSVC 19.50.35728 with `/O2 /DNDEBUG /EHsc /W4 /WX` on a Ryzen 9 7950X3D and Windows build 26300. `steady_clock` had 0.1 µs observation granularity. Candidate order rotates on each round. Each scene used one warm-up and 101 measured repetitions. The process was launched hidden and run below normal priority. Percentiles use nearest rank and describe this host/run. Cold and hot directory allocations used the same allocator for every candidate. PMR counts requested allocation sizes, peaks and calls. It includes bucket/node allocations and vector capacity but excludes object headers, stack storage, allocator internals, field pages, assets and native resources. No hardware cache counters were collected. The complete output, including p50, p99 and worst samples, is in [MetadataBenchmark.csv](../../tests/terrain/metadata/MetadataBenchmark.csv).

### Time

Each value is batch p95 in microseconds. SoA, Sparse, Archetype and Hybrid columns preserve source variant order.

| Stage | Balanced SoA | Balanced Sparse | Balanced Archetype | Balanced Hybrid | Skewed SoA | Skewed Sparse | Skewed Archetype | Skewed Hybrid |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Render projection | 3.2 | 1.0 | 0.7 | 0.9 | 22.6 | 11.9 | 4.5 | 8.7 |
| Collision projection | 3.0 | 0.8 | 0.7 | 0.7 | 7.3 | 1.1 | 0.6 | 0.9 |
| Random record access | 3.3 | 14.7 | 17.1 | 14.2 | 6.5 | 13.7 | 14.4 | 12.4 |
| Keyed directory + record lookup | 43.0 | 51.5 | 48.7 | 48.5 | 116.5 | 121.5 | 110.1 | 116.9 |
| Structural or in-place role churn | 2.1 | 5.1 | 10.3 | 5.1 | 3.3 | 5.1 | 10.4 | 4.4 |
| Revision edits + dirty admission | 21.4 | 37.5 | 31.6 | 36.4 | 27.1 | 38.2 | 35.2 | 36.8 |
| Supplemental full dirty scan | 0.9 | 0.6 | 0.7 | 0.6 | 24.8 | 17.8 | 18.5 | 19.1 |
| Immutable value preparation | 19.8 | 19.9 | 19.9 | 20.2 | 20.3 | 20.0 | 20.2 | 20.7 |
| Metadata lifecycle | 1.5 | 3.5 | 2.8 | 2.8 | 2.2 | 5.4 | 2.9 | 3.8 |
| Directory erase/reinsert | 8.3 | 6.8 | 8.2 | 8.2 | 10.7 | 9.5 | 10.6 | 10.1 |
| Main admission | 3.0 | 6.3 | 4.0 | 2.8 | 3.1 | 5.2 | 3.8 | 3.2 |

Archetype lanes won the two full ready-state projections. Packed SoA records reduced point reads, revision/edit admission and lifecycle cost in both fixtures. The sparse and hybrid competitors improved projection while adding allocation/indirection. Direct record lookup avoids most directory cost. A bounded active/ready directory and dedicated consumer-owned ready subsets can preserve useful scans without adding repeated structural movement to editing, activation, resource swaps and LOD.

### Allocations and bytes

| Scene | Layout | Live record bytes | Peak record bytes | Record allocations | Live bytes per active record |
| --- | --- | ---: | ---: | ---: | ---: |
| Balanced | SoA | 32,768 | 32,768 | 6 | 32 |
| Balanced | Sparse | 51,200 | 53,248 | 14 | 50 |
| Balanced | Archetype | 45,056 | 46,080 | 33 | 44 |
| Balanced | Hybrid | 43,008 | 45,056 | 12 | 42 |
| Skewed | SoA | 262,144 | 262,144 | 6 | 32 |
| Skewed | Sparse | 409,600 | 425,984 | 14 | 50 |
| Skewed | Archetype | 374,784 | 393,216 | 31 | 45.75 |
| Skewed | Hybrid | 344,064 | 360,448 | 12 | 42 |

Common active/cold directories cost 65,584/1,048,624 bytes in the balanced fixture and 524,336/8,388,656 bytes in the skewed fixture. Cold storage was about 64 bytes per entry under this hash-map layout and allocation boundary. The common scheduler retained 152,576 and 245,760 bytes, respectively. Summing these common allocations and the SoA records gives 1,299,552 and 9,420,896 live requested bytes. The cold directory exceeded the active record contribution. That cost favors absent entries for immutable air and real tombstones only where a saved revision or edit requires durable state. Production field directory storage uses the independently implemented `Field` directory. Its actual bytes and lifetime need their own instrumentation. The 32-byte measured active record is the benchmark's layout with synthetic local IDs and optional role IDs. The production registry stores full signed keys, generation and revisions in separate columns and has a different physical byte count.

## Actual worker handoff probe

[MetadataJobProbe.cpp](../../tests/terrain/metadata/MetadataJobProbe.cpp) links the production registry, `JobSystem`, and `ExecutionDomain`. Each wave captures 64 tickets from 1,024 active chunks and submits immutable tickets to one or two workers. Main advances eight source geometry versions during the wave. It verifies the Worker domain, matching product checksum, exactly 56 accepted results and eight rejections. It compares submitting one or eight tickets per job. A wave remains below or equal to the job queue's initial capacity. The probe waits synchronously to measure elapsed wave time. Terrain runtime needs bounded asynchronous admission and draining rather than that benchmark barrier. There are 101 measured waves per setting and one warm-up.

| Workers | Tickets/job | Submit p95 µs | Main admission p95 µs | Wave p50 µs | Wave p95 µs | Wave p99 µs | Worst µs |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 21.1 | 1.5 | 12.3 | 34.8 | 36.1 | 39.2 |
| 1 | 8 | 0.4 | 0.9 | 2.9 | 3.0 | 3.4 | 5.6 |
| 2 | 1 | 40.0 | 1.6 | 26.4 | 50.8 | 53.1 | 55.0 |
| 2 | 8 | 2.3 | 1.0 | 3.4 | 4.8 | 18.8 | 22.6 |

The immediate implication is to test small bounded waves or batches for metadata and ticket work. Mesh extraction, provider conditioning, persistence and uploads are unequal-duration phases with different contention and cancellation behavior. Batch size, worker count, and queue depths for those stages require terrain-scene traces. [Full worker output](../../tests/terrain/metadata/MetadataJobProbe.csv).

## Production choice and reusable boundaries

- Store active metadata in a terrain-scoped packed registry of key, private generation, geometry revision and material revision. Creation of an active row goes through chunk admission, and eviction swap-removes the row and fixes the key index. Handle allocation never depends on a chunk's prior slot or coordinate.
- Leave immutable lattice pages and nonresident tombstones in the `Field` owner. A `ChunkWorkTicket` checks terrain epoch, registry-owner identity, activation generation and both local revisions. The coordinator pairs tickets with immutable field snapshots, distinct field identity plus covered dependency revisions, and any request serial needed for repeat same-version LOD or provider work. Ticket validation and snapshot dependency checks are both necessary when a neighbor supplies a halo.
- Keep resources with their subsystem owners. Render projection publishes immutable frame data and tracks device ownership and fence retirement. Physics projection keeps provider handles behind the physics safe point and tracks admitted collision versions. Bounds, LOD and content or persistence state can live in optional owner-side records or indexes keyed by active chunk or published descriptor. A resource or readiness change can update an in-place state or a ready list without archetype migration.
- Use separate incremental dirty candidate, ready-for-upload, collider-ready and retire queues where the complete path benefits. Index spatial coverage independently. Bound jobs by counts, work bytes and completion capacity. Main must revalidate before provider publication and account for retained old resources until replacement is ready or cancellation releases their dependency.
- Retired or reset tickets cannot reappear. Registry epoch only advances, row generations never wrap, and a newly constructed registry gets an independent owner token even if its world epoch and keys match. The registry reports lowered revision or budget denial without replacing old state. Dense buffer high-water capacity stays available for reuse until the registry is destroyed.

One reusable pattern is a stable semantic key plus a bounded private active-record set, immutable worker packets, generation- and revision-aware Main admission, and disposable consumer-side projections. Scattering can use deterministic region and point keys plus batched render handles. Navigation can admit tile builds against collider and profile revisions. Virtual-texture pages and decals can retire against texture/geometry revision and device ownership. Path splines can keep durable authored object identity while scheduling bounded page-local stamp work. Prefer small owner-scoped stores for these systems and compare actual dataflow before introducing a shared framework.

## Reproduction and limits

Run the following in an x64 Visual Studio Developer Command Prompt from the production worktree, with output directed to a task-owned build directory.

```bat
cl /nologo /std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX tests\terrain\metadata\MetadataBenchmark.cpp /Fe:%TEMP%\TerrainMetadata.exe /Fo:%TEMP%\
cl /nologo /std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX /Iinclude tests\terrain\metadata\ActiveChunkRegistryTests.cpp /Fe:%TEMP%\ActiveChunkRegistryTests.exe /Fo:%TEMP%\
cl /nologo /std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX /Iinclude tests\terrain\metadata\MetadataJobProbe.cpp src\runtime\JobSystem.cpp src\runtime\ExecutionDomain.cpp /Fe:%TEMP%\MetadataJobProbe.exe /Fo:%TEMP%\
%TEMP%\TerrainMetadata.exe
%TEMP%\ActiveChunkRegistryTests.exe
%TEMP%\MetadataJobProbe.exe
```

[Registry checks](../../tests/terrain/metadata/ActiveChunkRegistryTests.cpp) passed for invalid bounds, capacity rejection, idempotent activation, independent edits, revision rollback rejection, row relocation, retire/recreate, reset, and same-epoch independent owners. Both Release probes completed with cross-candidate or worker result parity. The probes leave renderer/collider lifecycle, persistence, production memory peaks, dependency snapshot bytes, parallel extraction under load, and cold network residency to Foundation 1 integration qualification.
