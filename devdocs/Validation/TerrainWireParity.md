# Cross-platform native terrain wire qualification

The standalone Field, GTCH chunk and GTMF manifest paths passed under GCC 13.3.0 on Ubuntu 24.04 with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection enabled. The fixture also produced matching SHA-256 digests and byte counts on Windows x64 MSVC 19.50.35728 and Linux x64 GCC 13.3.0. [Chunk and manifest formats](TerrainChunkCodec.md), [fixture source](../../tests/terrain/codec/TerrainWireParity.cpp), [Linux probe](../../tests/terrain/codec/RunTerrainWireQualification.sh).

| Artifact | Bytes | SHA-256 on both systems |
| --- | ---: | --- |
| `Air.gtch` | 36 | `815b0b608fbea8257cfcce4a56481d34c9200fbcedcfb0b6b6446d57cd191e3b` |
| `Uniform.gtch` | 42 | `9d33bd40ac5ae84a62ebda49b58c961b763a58f96d4debee251fea24f7a58a85` |
| `Mixed.gtch` | 4,460 | `f1607263266d351661442ba8983a263250dfd4a9ff78076ad23019945b3d7b67` |
| `Terrain.gtmf` | 216 | `7a1c80bb4707386585a25d3af7555fc606b88374f08dfddf2781c4c44dd7e135` |

The deterministic integer-authored fixture includes a half-unit lattice, negative owner coordinates, a saturated-air leaf, uniformly solid material, dense packed and raw material bricks, sparse samples, and nondefault zero density. GNU `sha256sum` and Windows `Get-FileHash` independently hashed the encoded bytes. The fixture used those real leaf digests when authoring the two-entry manifest. The separate Linux sanitizer suites decoded and re-encoded exact field data, applied replacement through `Field`, checked the largest artifact and manifest, and exercised malformed input, version, cancellation, binding, and size denial.

Windows used `/std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX`. Linux used `-std=c++23 -O1 -g1 -fsanitize=address,undefined -fno-pie -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror` with leak detection and halt-on-error. The Linux container used the existing Ubuntu toolchain image `sha256:d71a9fc779ab954d99b9ddce26a497e2b5e83921484cd645540dd0e973bd8176`, restricted to two CPUs, 2 GiB memory, no network, read-only root and source, and private tmpfs build storage. The 123,392-byte source archive contained only these standalone test dependencies. The temporary Linux source directory was removed after verification.

The receipt establishes tested portability of the native field and wire formats. Full Engine builds, Project/GAS save and reopen, filesystem durability, live physics and render reconstruction, and package startup on Linux remain separate qualification gates.

Run the Linux probe from a private writable build directory with the standalone sources mounted read-only and pass their absolute directory as the runner argument. For Windows, build `TerrainWireParity.cpp`, `Field.cpp`, `ChunkCodec.cpp`, `Manifest.cpp`, and `ProjectIdentity.cpp` with the Engine include root. Run `TerrainWireParity.exe` once in a task-owned build directory to emit the leaves. Compute `Uniform.gtch` and `Mixed.gtch` SHA-256 values and pass them, in that order, as the executable's two arguments to emit `Terrain.gtmf`.
