---
status: experimental qualification
owner: terrain
---

# Lossless terrain height column storage

This prototype evaluates internal storage on Engine `88aafd4c3913cc4400dc6f2b0a90a41d5d5c6a1d` using the real `Field`, canonical GTCH encoder, native MC33 and collision conditioning. Each storage mode reads a committed field capture. Coordinate ownership, edit authority, active metadata, rendering, provider integration and persisted formats remain production contracts. [Ownership decision](../../docs/adr/TerrainFoundation1.md) · [Prototype](../../tests/terrain/storage/HeightColumnPage.hpp) · [Qualification source](../../tests/terrain/storage/HeightColumnQualification.cpp)

## Exact representations

- Whole-tile storage tries one height profile for each of the 32×32 horizontal columns in a 32³-sample page. A column admits a contiguous solid prefix followed by air. One affine vertical predictor uses integer density units and clamps to the field saturation value. A bit mask and exact signed residual values preserve every density the predictor misses, including positive values near the surface and exact zero. Up to four contiguous layers retain their original uint32 material IDs.
- Selective storage applies the same representation to each mixed 8³ brick. Implicit air and constant bricks keep their existing compact cases. A brick with an incompatible sign sequence or too many material runs uses a dense density and material palette layout like production. Each scope selects the candidate only when its accounted storage is smaller than the corresponding brick payload. Cave and overhang samples can coexist with local height bricks in the same page.
- The selective edit probe copies a single pointer table and encodes the changed bricks. Original readers continue to hold the old immutable page and unchanged bricks. Terrain repair uses the same selection path and can demote the edited brick. Whole-tile repacking examines every owned sample of the changed page and replaces the whole backing when any column is incompatible.

The scope is one fixed 32³ page and 64 smaller bricks. Each column has at most 32 density overrides and four material runs. A prototype caller supplies valid storage coordinates from a captured `Field` and the dirty brick set. This design would require integration with production `Field::Prepare` to construct and check that set under its existing edit budgets.

## Windows Release results

MSVC 19.50.35728 x64 `/O2` on a Ryzen 9 7950X3D. The executable also passed Windows AddressSanitizer on the same source. Each row below aggregates one to three pages. `Field bytes` is actual logical `Field::GetStats().OwnedBytes`. Candidate estimates use their measured allocations and vector capacities plus the same per-key directory accounting. Allocator metadata, tree node overhead and transient constructor allocations are outside both counts. `GTCH bytes` is the sum of actual production artifact bytes for those pages as a separate on-disk comparator.

| Workload | Field bytes | GTCH bytes | Whole tile bytes | Selective brick bytes |
| --- | ---: | ---: | ---: | ---: |
| Flat surface | 136,704 | 66,312 | 43,328 | 78,000 |
| Rolling surface | 176,276 | 72,760 | 43,328 | 96,225 |
| Cliff at a chunk boundary | 136,640 | 66,216 | 43,328 | 72,880 |
| Four material layers | 193,524 | 87,409 | 74,048 | 132,420 |
| Exact in-band density noise | 176,276 | 72,652 | 69,316 | 115,547 |
| Uncorrelated solid materials | 332,164 | 210,594 | 332,212 | 295,544 |
| Uniform solid pages | 4,224 | 84 | 4,272 | 4,272 |
| Excavated cavern and separate island | 169,060 | 78,478 | 169,132 | 101,072 |
| Clipped world edge, 0.5-unit authored spacing | 87,116 | 35,868 | 21,504 | 47,131 |

Across these single-surface inputs, whole-tile storage used 25–39% of the existing logical Field bytes. Selective storage used 53–68%. The cavern and island disqualified all whole tiles in their three-page capture. Independent 8³ selection compressed 76 mixed bricks and kept three dense mixed bricks. Uniform pages already have cheap storage and a 42-byte canonical artifact per page. The prototype wrapper adds 24 bytes per fallback page.

The benchmark times complete per-page candidates constructed from production snapshots. It also times the incumbent GTCH encoder on the same snapshots. Profile packing is an in-memory conversion and its output bytes in the table are in-memory estimates. Restoring either candidate into an ordinary `Field` produces the exact existing GTCH bytes. A new artifact encoding is outside this experiment.

| Workload | Field read ns | Whole read ns | Selective read ns | GTCH encode µs/page | Whole pack µs/page | Selective pack µs/page |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Rolling surface | 17.78 | 14.51 | 18.25 | 169.50 | 377.55 | 140.50 |
| Four material layers | 17.60 | 16.37 | 19.91 | 235.00 | 464.05 | 180.60 |
| Exact in-band density noise | 17.66 | 15.98 | 19.23 | 159.55 | 395.75 | 156.90 |
| Excavated cavern and separate island | 17.79 | 14.51 | 19.71 | 101.27 | 301.83 | 156.53 |

Read timings are medians across three runs of up to 200,000 deterministic random points through keyed directories. Their small working set fits in cache. The prototype and real Field share the same lookup shape with different wrapping and error contracts. These timings establish the size of warm-read costs in this fixture. They do not establish production read or meshing throughput for a streamed large world.

One buried-sample excavation changed the whole tile from 21,608 to 68,324 owned payload bytes. Selective promotion changed its page from 38,944 to 39,864 bytes. It rebuilt 512 samples and reused 63 sibling brick references. Repair returned both candidates to the column representation and exact original GTCH bytes. In 24 excavation/repair cycles the whole-page constructor averaged 282.56 µs per candidate. Selective rebuild averaged 8.26 µs per candidate, including the shared-pointer table copy. These timings begin with prepared field snapshots. An Engine edit also incurs the normal field preparation, mesh and provider work.

Every valid owned sample matched before and after reconstruction. Adjacent owners met at negative coordinates and across zero. The clipped exterior remained implicit air. All candidate halos produced identical MC33 vertices, normals, material source references and indices. Native collision conditioning yielded identical outcomes from those meshes.

## Disposition

Selective brick promotion is the better candidate for editable pages that can acquire caverns, overhangs and arbitrary regional edits. Whole-tile columns are a useful cold storage candidate for known surface-only terrain when memory density matters and the entire page can be repriced on mutation. The prototype supports a targeted production follow-up that adds a private optional brick storage mode and retains the canonical signed-field and GTCH boundary. It does not justify changing the field authority, chunk identities, mesh resolution, or peer artifacts. Production adoption would still need larger warm and cold samples, direct field-brick build timing, scheduled edit and halo read traces, and save/network re-encode costs on the integrated path.

## Native adaptive brick policy

`BrickStorageMode::LosslessColumns` is available to native Field owners.
The default policy remains dense palette for mixed bricks. The optional
policy shares the profile implementation with the prototype and uses
the existing immutable Field pages, edit snapshots and GTCH codec.
Uniform and air cases retain their original accounting.

| Workload | Dense bytes | Adaptive bytes | Dense → adaptive warm read |
| --- | ---: | ---: | ---: |
| Flat | 136,704 | 77,952 | 17.64 → 21.12 ns |
| Rolling | 176,276 | 96,177 | 17.79 → 21.25 ns |
| Four material layers | 193,524 | 132,372 | 17.38 → 22.79 ns |
| Cave and island | 169,060 | 101,000 | 17.51 → 19.71 ns |
| Uniform | 4,224 | 4,224 | 13.45 → 13.46 ns |

The native Windows Release and AddressSanitizer replay passed for all nine
workloads. The native path reproduced the exact GTCH bytes and derived
MC33 and collision outputs. A single-brick excavation and repair restored
the original accounting and preserved each older snapshot. The native
receipt and complete timings are under
`C:/Sandbox/Codex/Artifacts/TerrainHeightNative`.

The sparse-world probe places rolling ridges across separated clusters and
samples 200,000 random reads. It verifies matching read checksums and
canonical artifacts for every sixteenth page.

| Materialized pages | Dense bytes | Adaptive bytes | Dense → adaptive authoring | Dense → adaptive warm read |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 2,747,788 | 1,502,391 | 49.20 → 51.77 ms | 31.19 → 38.52 ns |
| 128 | 11,118,636 | 6,072,024 | 207.52 → 215.92 ms | 85.02 → 56.35 ns |
| 512 | 44,940,788 | 24,535,756 | 774.35 → 803.81 ms | 114.10 → 143.53 ns |

The Release run reduced accounted storage by about 45% at all three
sizes. Random access varied with the working set. The sample of 32 GTCH
encodes at 512 pages took 4.44 ms for dense and 6.86 ms for adaptive.
Replay with `--sparse-world`. These timings cover field authoring and
codec work. The rendered and playable world budget is a separate profile.

## Replay

Compile the test with the three existing terrain implementation files. Run from a task-owned output directory. A recent C++23 toolchain with `std::expected` is required.

```sh
c++ -std=c++23 -O2 -DNDEBUG -Iinclude -Isrc \
  src/terrain/Field.cpp src/terrain/ChunkCodec.cpp src/terrain/Surface.cpp \
  tests/terrain/storage/HeightColumnQualification.cpp -o HeightColumnQualification
./HeightColumnQualification
```

MSVC uses `/std:c++latest /O2 /EHsc /DNDEBUG /Iinclude /Isrc` for the same files. The Windows sanitizer replay used `/O1 /Zi /fsanitize=address /INCREMENTAL:NO`. Full Linux Engine replay remains open. The Windows build worker SSH timed out, Windows could not start its Ubuntu WSL instance, and the LAN Linux host had insufficient disk capacity for a full Engine build.
