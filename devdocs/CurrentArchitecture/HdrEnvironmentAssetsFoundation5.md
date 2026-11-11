---
status: qualified-experimental
owner: assets and rendering
related_code:
  - src/assets/AssetImporter.cpp
  - src/services/AssetService.cpp
  - src/render/TextureProvider.cpp
  - src/render/ImageBasedLighting.cpp
---

# Renderer Foundation 5 HDR environment assets

Radiance RGBE `.hdr` is the first HDR source format. The pinned stb decoder is
compiled for HDR only. Import admits a square face (up to 512 × 512) or a 2:1
equirectangular panorama (up to 1024 × 512), under the existing 8 MiB source
limit. A square source creates one Image. A panorama creates six Image records
in `+X, -X, +Y, -Y, +Z, -Z` order with logical keys `positive-x`,
`negative-x`, `positive-y`, `negative-y`, `positive-z`, `negative-z`.
The existing source-group mechanism owns all six records and preserves their
AssetIds across reimport. The import result returns the six references for Sky
or probe authoring. Conversion uses deterministic bilinear sampling with
horizontal wrap and vertical clamp. The panorama center points toward `+Z`,
its right quarter toward `+X`, and its top toward `+Y`.

HDR pixels are scene-linear RGBA32F bytes; no exposure multiplier, sRGB
decoding, or tone map is baked into the canonical artifact. Import rejects
non-finite or negative RGB components while retaining finite radiance above
the half-float range. Alpha is one.
The bounded decoder requires exactly one `FORMAT=32-bit_rle_rgbe` assignment and
the standard `-Y ... +X ...` raster orientation. Neutral `EXPOSURE=1` and
`PIXASPECT=1` are accepted. Nonunity exposure or pixel aspect, and any explicit
`PRIMARIES`, `COLORCORR`, or `GAMMA` assignment are rejected rather than silently
changing radiance or aspect. Unrecognized bounded header metadata and comments
remain accepted; only line-start assignments are interpreted. Radiance's
[picture format specification](https://floyd.lbl.gov/radiance/refer/filefmts.pdf)
defines cumulative exposure, color correction, primaries, and pixel aspect,
which the canonical converter does not apply. The 8 MiB source cap and square
512-pixel / panorama 1024 × 512 limits bound decode and six-face conversion;
larger capture files need a separately budgeted import contract.
Artifact version 7 is specific
to float Image assets: existing version 1 and 2 image encodings remain
unchanged. Version 7 stores width and height as little-endian 32-bit integers,
the format tag `Rgba32Float` as a little-endian 32-bit integer, then tightly
packed little-endian RGBA32F pixels. Hash validation, exact length, bounds, and
finite component validation precede admission. Source paths stay in the asset
catalog, outside renderer publication.

AssetService publishes the format with the same canonical AssetId-derived
texture identity and content revision. Format or dimension change retires the
old GPU identity and creates a new generation; same-format reimport updates
the existing identity. The SDL cache uploads float images to sampled
`R32G32B32A32_FLOAT`. A device without that format rejects the upload through
the existing full-resync recovery path. Material mip views are RGBA8-only;
float images retain their linear base level. Sky and IBL source shaders decode
sRGB only for RGBA8 faces. Each float texel is clamped to 65504 before
filtering into RGBA16F targets, so high finite source values cannot overflow
the derived convolution. The authored exposure applies only at scene resolve.

Base upload transfer buffers keep at most 16 MiB of logical capacity after
each successful publication. When the cache exceeds that cap, it releases the
least recently uploaded buffers after GPU submission. A later patch or reimport
recreates staging on demand; resident textures remain available. The cache
reports retained staging bytes and eviction count. This bounds the long-lived
upload mirror without altering texture residency or material mip budgets.
Peak in-flight allocation during a bulk publication is separate and remains
bounded by the asset and publication limits.

Material mip views have a separate 128 MiB default budget and 128 MiB retained
source budget. The source budget counts strong CPU references or patched copies
held by the SDL cache; an exact-revision weak handle to an immutable published
full image does not pin its bytes. The cache evicts the least recently demanded
cold source when a late visible image needs space. If the publication or asset
owner still holds that full image, demand can relock it without copying or
changing texture identity. An expired backing stays on the resident base GPU
texture until a complete replacement arrives. A partial update may rebuild
the current source only while the exact previous-revision backing is still
available; otherwise it invalidates the weak handle and cannot combine stale
old pixels with the patch. Patched copies are budgeted and may also be evicted,
after which a complete replacement is required to rebuild mips. The cache
reports retained source bytes, source evictions, and rehydrates. The normal
asset service's 64 MiB CPU cap sits below the source default, while direct
renderer publications are also bounded by the demand policy.
Source promotion occurs only when a mip view reaches the per-publication
upload admission loop; a large deferred queue cannot repeatedly evict sources
before the 32-view and 32 MiB limits are applied.

The cache records demand when a draw falls back to the resident base image.
The next publication admits at most 32 derived views and 32 MiB of their
upload bytes, prioritizing recent demand then stable texture identity. An
unadmitted view stays deferred; color sampling uses the established manual
sRGB path and data sampling uses the linear base level. If view capacity is
exhausted, only a view unused during the prior frame can be evicted. Active
views stay resident until demand changes, avoiding repeated upload churn when
all visible textures exceed the budget. Eviction occurs before the next frame's
draw commands are recorded. SDL defers actual texture destruction until safe
for in-flight GPU work. The cache reports view evictions and deferred uploads.
[SDL release semantics](https://wiki.libsdl.org/SDL3/SDL_ReleaseGPUTexture)

OpenEXR is deferred because its broader channel, multipart, compression, and
metadata surfaces require a larger decoder and a separate bounded import
contract. [OpenEXR security guidance](https://github.com/AcademySoftwareFoundation/openexr/blob/main/SECURITY.md)
requires application-defined limits for untrusted files. KTX2/Basis and GPU
block formats remain a measured device and asset pipeline decision; this slice
does not claim block compression or partial mip streaming. KTX2 universal
sources require runtime transcoding into a device format, and SDL exposes a
per-format capability query. Admission needs measured benefit on qualified
adapters and a defined uncompressed fallback.
[Khronos KTX guide](https://github.com/KhronosGroup/3D-Formats-Guidelines/blob/main/KTXDeveloperGuide.md)
and [SDL format query](https://wiki.libsdl.org/SDL3/SDL_GPUTextureSupportsFormat).
Existing RGBA8 source and material fallback behavior remains.

## Qualification

Focused AssetFoundation and publication tests pass in MSVC Release and hosted
Linux Vulkan/sanitizer lanes at `ad58172cbd8516c9f95ba65c223efc123d6330b7`.
Both physical Vulkan adapters pass source-format equivalence, extreme-radiance,
mip-demand/eviction, probe reimport and renderer-recreation gates. The retained
decoder audit reproduces 376 accepted truncated prefixes out of 688 using the
old decoder and zero using the hardened version. Raw receipts and exact source
identities are in `../Validation/RendererFoundation5Qualification.md`.
These gates qualify the stated bounded RGBE/source-view contract; compression,
EXR, arbitrary capture resolutions and complete texture streaming remain deferred.
