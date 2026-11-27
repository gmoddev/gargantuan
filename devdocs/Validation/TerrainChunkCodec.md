# Terrain Foundation 1 native chunk artifact

`GTCH` v1 is a canonical native artifact for one terrain chunk at one authored grid spacing. It serializes editable scalar/material samples and binds the bytes to the expected signed coordinate. [`ChunkCodec.hpp`](../../include/gargantuan/terrain/ChunkCodec.hpp) exposes inert encoding and decoding, with authoritative import through [`Field`](../../include/gargantuan/terrain/Field.hpp).

## Wire layout

Every multi-byte scalar is little-endian. Signed values use two's-complement representation. The 36-byte header has these fields.

| Offset | Field | Canonical value |
| ---: | --- | --- |
| 0 | Four bytes | ASCII `GTCH` |
| 4 | `u16` version | `1` |
| 6 | `u8` body kind | `0` all default air, `1` uniform non-default sample, `2` 64 encoded bricks |
| 7 | `u8` spacing code | `0`, `1`, `2` for 0.5, 1, 2 world units |
| 8 | `u32` body byte count | Exactly the remaining bytes |
| 12, 20, 28 | Signed `i64` X, Y, Z | All three owner chunk coordinates must match the expected key |

Chunk kind 0 has no body. Kind 1 has exactly one `i16` density and one `u32` stable material ID. Kind 2 has 64 brick records without gaps. Bricks are ordered X fastest, then Y, then Z, in a 4³ grid. Each brick has the same X-fastest order over its 8³ samples at every signed coordinate.

| Brick tag | Data |
| ---: | --- |
| 0 | Implicit saturated air (`32767`, material `0`) |
| 1 | One `i16` density and `u32` material ID repeated throughout the brick |
| 2 | 512 `i16` densities, `u16` positive palette count, sorted distinct positive `u32` material IDs, then LSB-first packed palette indices for negative samples in traversal order |
| 3 | 512 `i16` densities, then one positive `u32` material ID per negative sample in traversal order |
| 4 | 64 LSB-first mask bytes, then `i16` density and `u32` material for each set bit in traversal order |

A palette of size `P` uses `ceil(log2(P))` bits per negative sample. A single palette entry needs zero index bytes. The last byte's unused high bits are zero. A palette entry must be used. An unset sparse-mask bit means the default air sample. Set bits identify different samples, including nonsaturated nonnegative densities where material stays zero. In all modes a negative density requires nonzero material, a nonnegative density requires material zero, and `-32768` is reserved. Palette indices keep their local role only on the wire. Decoded writes retain the stable `u32` material identities.

Uniform chunks and uniform bricks always use their shortest kind. A mixed brick compares the actual wire lengths for packed, raw and sparse encodings, selecting the shortest with tie order packed, raw, sparse. This keeps high-diversity material pages within the raw bound while preserving deterministic bytes across edit ordering, distinct field instances and runtime revisions. The decoder validates canonical tags, palette ordering and usage, padding, coordinate range, values and exact length before returning a complete result.

The minimum legal coordinate can fall inside a negative edge chunk. Sample points outside the permitted domain are represented as saturated air and omitted from `Writes`. Every valid owned point is included, with explicit air writes for replacement of existing content. The artifact contains no halo or adjacent samples.

## Resource and admission boundaries

- Maximum encoded size is 196,708 bytes, equal to 36 header bytes plus 64 raw 3,073-byte bricks. Decoder input larger than that cap is rejected before expansion. Explicit caller key/spacing binding rejects misplaced artifact bytes. A coordinate included in the header makes identical samples at different keys different artifacts.
- The decoder expands a valid chunk to at most 32,768 `SampleWrite` entries, about 1 MiB at 32 bytes per write on the tested x64 ABI. This is a bounded expansion even for a 36-byte default-air artifact. Account for decoded writes, captured snapshots, and prepare working memory when admitting concurrent jobs. The implementation uses fixed-size brick scratch and short primitive reads. No general compressor, decompressor window, or external dictionary is required.
- The caller captures an immutable `Snapshot` covering the exact owner key. The encoder rejects an uncovered request. Main captures the destination before calling `Field::Prepare` with all decoded writes and commits the prepared result against its field identity and versions. The codec never mutates `Field` or activates consumer resources. The current v1 representation binds only authored spacing and coordinate. Package identity, content hash, file capability, stable registry resolution, permissions, admission and render/collider validity remain caller responsibilities.
- Cancellation is checked before work, between bricks and before returning ownership of an artifact or decoded values. Failure or cancellation discards local buffers. Errors distinguish invalid binding, uncaptured source, unsupported version, malformed, noncanonical, limit exceeded, allocation failure and cancellation. A future format needs a distinct version and explicit migration adapters. Existing manifests can preserve the earlier content identity.

## Verification

[ChunkCodecTests.cpp](../../tests/terrain/codec/ChunkCodecTests.cpp) encodes and decodes into an independent `Field` that already holds mismatched source data, verifies all authoritative samples after commit, and re-encodes to byte equality. It covers all five brick kinds, three spacing bindings, negative coordinates and partial edge chunks, arbitrary nonzero material identities, explicit clearing, canceled work and the raw size bound. Corruption checks include all strict prefixes of the mixed fixture, repaired outer lengths that truncate inner modes, unsupported versions, extra trailing bytes, duplicate/unused palette entries, invalid palette sizes/indices, nonzero padding, redundant sparse air, redundant full air, invalid sign/material and reserved density, and out-of-domain material.

| Release codec fixture | Artifact bytes |
| --- | ---: |
| All air | 36 |
| Full uniform solid | 42 |
| Mixed uniform, sparse, material palette and raw | 4,426 |
| 32,768 distinct solid material values | 196,708 |

Standalone native qualification passed on MSVC x64 19.50.35728 (`/O2 /DNDEBUG /W4 /WX`). From an x64 Developer Command Prompt at the worktree root, build with the following.

```bat
set "TerrainCodecRoot=%CD%"
mkdir "%TEMP%\GargantuanTerrainCodec"
pushd "%TEMP%\GargantuanTerrainCodec"
cl /nologo /std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX /I"%TerrainCodecRoot%\include" "%TerrainCodecRoot%\tests\terrain\codec\ChunkCodecTests.cpp" "%TerrainCodecRoot%\src\terrain\Field.cpp" "%TerrainCodecRoot%\src\terrain\ChunkCodec.cpp" /Fe:ChunkCodecTests.exe
ChunkCodecTests.exe
popd
```

These checks establish the native artifact and authoritative field round-trip. The package and GAS layer still needs manifest admission, digest and material dependency validation, save promotion and crash recovery qualification.

## Dataset directory artifact

[`Manifest.hpp`](../../include/gargantuan/terrain/Manifest.hpp) defines `GTMF` v1 for a complete chunk directory. `EncodeTerrainManifest` takes typed values in canonical order. `DecodeTerrainManifest` takes the expected Engine `ProjectId` and separately typed `TerrainDatasetId`. The validated directory supplies an authored `GridSpec`. Expected identity comes from an authorized root and is external to the untrusted artifact.

All multi-byte integer slots are little-endian. Project UUID and SHA-256 byte arrays retain their byte order. The 96-byte header has the following fields.

| Offset | Field | Value |
| ---: | --- | --- |
| 0, 4, 6 | Magic, `u16` manifest version, `u16` chunk codec version | `GTMF`, `1`, `1` |
| 8–13 | Six `u8` fields | Spacing code as above, signed-scalar representation `1`, chunk side `32`, brick side `8`, absent/exterior air policy `1`, required feature flags `0` |
| 14, 16 | `i16` default air density, `u32` default air material | `32767`, `0` |
| 20, 28 | Signed `i64` inclusive domain minimum and maximum along each axis | `−1,048,575`, `1,048,575` |
| 36, 44, 52 | Signed `i64` lattice origin X, Y, Z | Three zeroes |
| 60 | 16 project identity bytes | Engine `ProjectId` |
| 76, 84 | Two `u64` dataset identity fields | High, then Low |
| 92 | `u32` entry count | At most 65,536 |

Each 60-byte entry contains three signed `i64` owner coordinates, 32 SHA-256 digest bytes, and one `u32` encoded `GTCH` byte count. Entries are strictly ascending by signed X, Y, then Z. An explicit all-air leaf is omitted. Remaining sizes are between 42 and 196,708 bytes. The header and declared entries must consume the complete byte span. The representation and origin flag pin the v1 lattice and sign rules. A different boundary, field or required feature is rejected.

The directory preserves digest bytes as inert typed values. Source callers compute the SHA-256 digest from validated canonical chunk bytes. Consumers verify the manifest against its authorized root and check each leaf's digest and size before `DecodeChunk` checks its key and spacing. A complete verified directory supplies known absence for omitted keys. Failure to fetch or validate a listed chunk leaves that key unavailable. Material registry pins, file lookup, permission validation, trusted root binding, garbage collection and atomic project/GAS commit are integration responsibilities.

[TerrainManifestTests.cpp](../../tests/terrain/codec/TerrainManifestTests.cpp) passed standalone x64 MSVC Release with `/W4 /WX`. It tests byte equality, explicit little-endian positions, typed project and dataset binding, all strict prefixes of the three-entry fixture, unsupported fields, flags, order and duplicate keys, zero digest, bad coordinates, invalid encoded sizes, cancellation and hard bounds. Empty, three-entry and maximum directories use 96, 276 and 3,932,256 bytes, respectively. Maximum count decodes exactly 65,536 entries. The test links `Manifest.cpp` and `src/filesystem/ProjectIdentity.cpp`. A paired native Linux sanitizer and byte-parity receipt is available. Package integrity and physical file behavior remain separate. [Linux native qualification](TerrainWireParity.md).
