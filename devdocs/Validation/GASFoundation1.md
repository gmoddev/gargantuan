# GAS Foundation 1 qualification

The development branch is `foundation/gas-1`. The baseline and separate
Relay Rescue source are pinned in the [capability matrix](../../docs/architecture/GASFoundationInventory.md).
The [protocol](../../docs/protocols/GAS-v1.md) and [authoring contract](../../docs/protocols/GAS-Authoring-v1.md)
describe the behavior exercised here.

## Focused codec

The Windows x64 run used MSVC 19.50.35728 with `/std:c++latest /EHsc /W4 /O2`
at Engine `57bff55ca2758e7c1b02b1335b1844120f36bf7c`. Its executable SHA-256
was `6e306e89fcf2f32fee9d86ac29e403fb58af07b480c35935f3dfeb1884698aea`.
The Linux x86-64 run used GCC 13.3.0 on Ubuntu 24.04 with
`-std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.
It built Engine `1750c03ba98c3e60a152e01627a7f85bf07e07cc`, which adds the
isolated focused CI job to the Windows source revision. Both compiled
`GasDocument.cpp`, `ProjectIdentity.cpp`, `AssetTypes.cpp`,
`ProtocolInput.cpp`, and `GasDocumentTests.cpp` against GLM
`8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` and nlohmann JSON
`2e23687092840a66876b4bea48060bd79595ea4b`. [Linux job and log](https://github.com/gmoddev/gargantuan/actions/runs/45252274625/job/137964775066)

Both runs passed the golden, both-kind round trips, bounds and malformed
fixtures, and 512 deterministic mutations with recomputed integrity digests.
The minimal fixture's expected digest comes from an independent Python
`struct` and `hashlib` encoding. The three outputs matched.

| Fixture | SHA-256 |
| --- | --- |
| Minimal GASM body | `9b8d96c208673652bb1face5f229c11b12c8ac99dcd1956d7178446bbcde9e1c` |
| Typed GASM file | `6fed3d1de66ab6e71cf4019119c13795af3e8872997ed4b322676949c3cd3bb3` |
| Typed GASP file | `64708aa16eb4b83d5c14b9f44256b5fe984f264f4ae2b28de3765230edad80ec` |

## Integrated qualification

| Scope | Source | Evidence |
| --- | --- | --- |
| Engine baseline | `e7bec187def55e79c6ca956b17feb82926e5d1fe` | [Native Windows and Linux](https://github.com/gmoddev/gargantuan/actions/runs/43870187334) and [GNS sanitizers](https://github.com/gmoddev/gargantuan/actions/runs/43870187327) passed |
| Codec commit with complete Linux ASan/UBSan engine suite | `1092de4bd3ac0745b4a234ea3a056df15c1756b9` | [Ubuntu 24.04, Clang 19.1.1](https://github.com/gmoddev/gargantuan/actions/runs/45246986453/job/137946253959) passed 113/113 CTest cases, including the GAS codec |
| Codec commit with complete Windows Release engine suite | `1092de4bd3ac0745b4a234ea3a056df15c1756b9` | [Windows MSVC](https://github.com/gmoddev/gargantuan/actions/runs/45246986453/job/137946253343) passed 166/166 CTest cases |
| Studio handoff and schema documentation | `ba5b60830fb4b51c048b0a839fc53b8ac144fd4e` | [Hosted documentation and site](https://github.com/gmoddev/gargantuan/actions/runs/45249827217) passed |
| GNS protocol regression after the authoring and package integration | `ba5b60830fb4b51c048b0a839fc53b8ac144fd4e` | [Ubuntu 24.04, Clang 19 ASan/UBSan/LSan](https://github.com/gmoddev/gargantuan/actions/runs/45249832764/job/137956212044) passed 11/11 cases |
| Authoring, project, package and ordinary benchmark tests | `ba5b60830fb4b51c048b0a839fc53b8ac144fd4e` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45249822063) |
| Instrumented encode/decode, peak memory and C++ allocations | `b229864c3f1f96afb9be0853d3ecb115f2c96143` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45251021630) |
| Native checks with hierarchy notification deferral | `1750c03ba98c3e60a152e01627a7f85bf07e07cc` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45252274625) |
| Relay Rescue place round trip, packages, two-player mission and animated model | `ba3fd78b9cc273f9044c702fd8edf45f8725b86d` | [Ubuntu Clang 19 ASan/UBSan](https://github.com/gmoddev/gargantuan/actions/runs/45250180300/job/137957435891) passed the Relay test, asset-backed GASM test, package input test and graph benchmark. The full job reported 121/122 due to an authoring lifecycle leak. |
| Native suite after world cleanup and Windows test syntax correction | `70b2953f76e47952323b65914a921ec945e6783b` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45258517284) |
| Windows Release GAS, persistence, package and Relay tests | `cca3d151c09b613ce019574416607e2ca8ab346c` | Worker CTest passed 18/18 cases using GNS, including both missions, both package roles, and animated model import |
| Linux GAS codec, EditorHost and model tests after teardown fixes | `cca3d151c09b613ce019574416607e2ca8ab346c` | Worker CTest passed 3/3 cases under ASan, UBSan and LeakSanitizer |

The focused Linux worker used Ubuntu 24.04 in toolchain image
`sha256:4eeae307153445eff9e085c872580bc65ee05b43d80c9561ddd3e9a20d269f2d`,
Clang 19.1.1 with GCC 14's standard library, CMake 3.31.10, Ninja, and
RelWithDebInfo. The headless build used GNS off and flags
`-fsanitize=address,undefined -fno-omit-frame-pointer`. Both sanitizers
halted on errors and `ASAN_OPTIONS` enabled leak detection. The worker
preserves the test XML at `C:\Sandbox\Codex\Builds\GAS-1\linux-asan\ctest-linux-asan.xml`.

Only rows with stated outcomes above are established qualification evidence.
The Relay Rescue check copies the pinned fixture, exports and restores it,
checks canonical bytes and package manifests, starts both package roles,
and runs the existing mission using the restored project. Its model check
exports RelayBeacon, provisions the source catalog through the existing
AssetService snapshot seam, and verifies that a new world's Animator can
create a track for the pinned asset. The harness prints source and artifact
digests and elapsed subprocess times.

## Relay Rescue and graph measurements

The Linux Relay test used the Gameplay Foundation 7 fixture from
`06d1a2015b26f8610a74a6b6eea39b179a4417a8`. The fixture JSON SHA-256 was
`3cdd093e55c59952ae119c3dd247242b94917c1286d089acc144140a54bedd94`.
The GASP export and the export from restored JSON both had SHA-256
`49945c756aa60c84071e736b41ab0eb6cca2abf5a1432beea1267eeb6774a3a7`.
The gameplay test passed the existing simulated two-player mission.
The asset-backed RelayBeacon GASM had SHA-256
`d6b3c95ccf950e4cef93059ec41d48dbcd9d263ead7d705b5bf31bb3b34c0d12`.
The imported model's Animator built a track using the pinned rig and animation.

| Runtime artifact | Manifest SHA-256 | Build wall time | Startup metric |
| --- | --- | ---: | ---: |
| Player from GASP | `57baa6f393a5249bfce7aaf31d17df4e0071acee9c6651115e213cbfe5615a80` | 4.94 s | 2,483 ms |
| Server from GASP | `11e04dfac11f6808403ed96d91a9e0355c2a03848675e96866dfdf2fa9dd0a00` | 5.10 s | 14 ms |
| Player from restored JSON | Same as the GASP Player | 7.40 s | — |

These are single ordered runs in the ASan/UBSan job. The JSON build followed
the GASP build and reused its destination path. Export took 86 ms and restore
took 50 ms including file publication. The startup values are each host's
reported elapsed startup and use role-specific boundaries. The identical Player
manifest verifies the package built from either source. Runtime storage
continues to use the current distribution.

The same Linux job ran the [reflected graph benchmark](../../tests/SerializationBenchmark.cpp)
for one warmup and five timed iterations per format. Encode includes capture.
Decode includes parse and materialization. Timings are means in milliseconds.

| Instances | JSON bytes | GASP bytes | JSON encode | GASP encode | JSON decode | GASP decode |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 69,453 | 37,434 | 22.84 | 2.14 | 19.91 | 25.34 |
| 1,000 | 696,851 | 373,679 | 251.75 | 23.69 | 200.24 | 257.72 |
| 5,000 | 3,499,089 | 1,873,233 | 1,758.51 | 145.67 | 1,198.76 | 1,689.22 |

In these sanitizer runs, the binary files were about 46% smaller and decode
took longer in all three workloads. The hydration path includes reflected
validation and a private JSON-shaped projection. A direct binary runtime
loader needs independent measurements and admission qualification.

The six separately launched allocation profiles also passed under Clang 19
ASan/UBSan at `b229864c3f1f96afb9be0853d3ecb115f2c96143`. Counts cover
successful main-thread C++ allocation requests over five measured iterations.
Peak resident memory is the whole-process high-water mark before semantic
recapture and includes the sanitizer runtime.
[Linux allocation profiles](https://github.com/gmoddev/gargantuan/actions/runs/45251021630/job/137960368114)

| Instances | JSON encode / decode allocations | GASP encode / decode allocations | JSON peak bytes | GASP peak bytes |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 334,875 / 232,904 | 10,115 / 370,539 | 162,443,264 | 137,723,904 |
| 1,000 | 3,973,850 / 2,333,919 | 98,905 / 3,705,859 | 530,690,048 | 492,883,968 |
| 5,000 | 21,508,020 / 11,700,329 | 493,460 / 18,562,284 | 775,815,168 | 662,519,808 |

### Windows Release replay

The Windows x64 worker used MSVC 19.51.36260, CMake 3.31.10, Ninja, Release,
and pinned GNS `2cb93a06350bb065db53abdb0d87cf297e0bfd34`. It had a Ryzen 9
5900X with 24 logical processors and 34,282,344,448 bytes of physical memory.
The build used eight jobs and ran the measured CTest cases sequentially.
Evidence is in `C:\Sandbox\Codex\Logs\GAS-1\ctest-msvc-final.xml` on the
worker. The package regression failed with the first, longer worker temp
directory. The unchanged binary passed with `C:\Sandbox\Codex\Temp\GAS`.

Both Windows Relay Rescue missions passed, including real GNS loopback.
The canonical GASP and RelayBeacon GASM hashes matched the Linux results
above. The Windows Player manifest SHA-256 from GASP and restored JSON was
`62f4e95cacb304e6e25c08649c99d9e9df70c7d5f41445ea51f9c463471c172e`.
The Windows Server manifest SHA-256 from GASP was
`4fc02f7169dbb81de5fede1916cf44e3bcca3c10e8ed215e84de1252caaf853c`.
The GASP Player and Server builds took 293 ms and 291 ms, respectively.
The subsequent JSON Player build took 413 ms. Host-reported Player and
Server startup was 113 ms and 7 ms.

| Instances | JSON encode ms | GASP encode ms | JSON decode ms | GASP decode ms |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 4.96 | 0.70 | 4.84 | 5.62 |
| 1,000 | 69.29 | 8.56 | 50.97 | 64.36 |
| 5,000 | 408.71 | 49.17 | 286.42 | 332.20 |

The allocation profile ran one backend and size per process. Counts are
successful main-thread C++ allocation requests over five measured iterations.
Peak resident memory is the whole-process high-water mark measured before
semantic recapture. The ordinary timings above use a separate executable
without the allocation probe.

| Instances | JSON encode / decode allocations | GASP encode / decode allocations | JSON peak bytes | GASP peak bytes |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 374,725 / 284,127 | 13,035 / 425,837 | 18,477,056 | 17,281,024 |
| 1,000 | 4,371,325 / 2,830,487 | 127,035 / 4,243,252 | 58,310,656 | 44,380,160 |
| 5,000 | 23,495,700 / 13,782,480 | 633,485 / 20,848,530 | 196,247,552 | 106,405,888 |

The same Windows environment opened the restored Relay Rescue project
from disk with both formats. Each entered the loaded revision and service
state with 20 Instances. Five warm-cache opens after one warmup averaged
3.03 ms for the 21,908-byte JSON scene and 3.94 ms for the 16,494-byte GASP.
Python 3.12.6 gzip at level 6 with `mtime=0` produced 4,817 bytes from JSON
and 4,763 bytes from GAS. This test used the canonical payload as an opaque
byte stream and changed no GAS or runtime format. The measurement and
compression CSVs are in `C:\Sandbox\Codex\Artifacts\GAS-1\relay-load-cca3d151c09b`.
