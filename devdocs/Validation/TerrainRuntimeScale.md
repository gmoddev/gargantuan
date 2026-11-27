# Terrain storage and native startup scale

The Windows Release probe runs the optional mode
`gargantuan_terrainruntimetests --measure-scale`. Each matched pair uses
the same signed samples, material IDs, project identity, native Box3D
provider, and two-worker terrain runtime. The probe encodes and reopens
the canonical artifact closure, admits the terrain, then applies 20
paint edits across a chunk seam. These timings are local wall-clock
measurements. Native admission begins after the codec has restored the
field and finishes when the installed provider can answer a terrain ray.

The ridge grows a contiguous strip of full signed columns. The sparse
world keeps four connected surface pages and adds isolated positive-only
pages. Each resident memory figure is the reported field payload and
directory accounting.

| Layout | Stored pages | Palette bytes | Column bytes | Canonical closure bytes | Render triangles | Palette native admission | Column native admission |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ridge | 4 | 540,364 | 251,910 | 263,588 | 25,580 | 34.1 ms | 33.8 ms |
| Ridge | 24 | 3,242,176 | 1,510,080 | 1,581,024 | 148,236 | 171.9 ms | 168.9 ms |
| Ridge | 64 | 8,645,804 | 4,027,110 | 4,215,908 | 393,580 | 442.1 ms | 443.1 ms |
| Sparse | 24 | 604,124 | 291,110 | 268,188 | 25,580 | 31.6 ms | 33.5 ms |
| Sparse | 96 | 833,660 | 432,230 | 284,748 | 25,580 | 33.0 ms | 32.1 ms |

Each pair produced the exact same manifest and admitted the same render
and collision meshes. The 64-page ridge retained 15,872,800 renderer-neutral
mesh bytes and 15,960,448 native mesh bytes. Native peak build data reached
16,117,568 bytes. Its paint edit-to-commit p95 was 16.66 ms with the default
field and 16.60 ms with columns. Neither paint sequence created native
geometry. The next incremental capture reused 62 of 64 leaves and
re-encoded 131,626 leaf bytes in both modes. The canonical format is the
same across storage modes. The 512-page field-only probe reduced
44,944,884 bytes to 24,539,852 bytes. Warm reads varied by working set and
canonical sampling took longer with columns. Palette bricks remain the
default.

## Positive-only pages

Native admission for the 96-page sparse fixture dropped from 332.7 ms
to 33.0 ms on the default field. A materialized page now tracks, in eight
bytes, which of its 64 immutable bricks contain a negative density
sample. Startup and replica preparation expand contour owners only from
pages containing a solid sample. Each owner still captures all adjacent
signed values, including zero and positive halo data. Live edits update
the index at commit. A sign change on an existing positive page can add
contours without adding a stored page.

The offline limits remain 512 stored pages and 1,024 candidate contour
owners. Native tests admitted 512 isolated positive pages without
creating collision resources. A separate fixture filled the signed
contour budget, blocked promotion of an existing positive page, cleared
a previous solid page, and then admitted and reopened the replacement.
The independent review checked density-zero classification, halo
coverage and exact artifact restore.

The CSVs are under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/Scale-Windows`.
The 64-page active ridge exercises a single deliberate startup workload.
The package and multiplayer profile continues to bound each peer's
streamed neighborhood separately.
