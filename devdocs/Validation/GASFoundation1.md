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
| Studio handoff and schema documentation | `ba5b60830fb4b51c048b0a839fc53b8ac144fd4e` | [Hosted documentation and site](https://github.com/gmoddev/gargantuan/actions/runs/45249827217) passed |
| Authoring, project, package and ordinary benchmark tests | `ba5b60830fb4b51c048b0a839fc53b8ac144fd4e` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45249822063), [GNS sanitizers](https://github.com/gmoddev/gargantuan/actions/runs/45249832764) |
| Instrumented encode/decode, peak memory and C++ allocations | `b229864c3f1f96afb9be0853d3ecb115f2c96143` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45251021630) |
| Native checks with hierarchy notification deferral | `1750c03ba98c3e60a152e01627a7f85bf07e07cc` | [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/45252274625) |
| Relay Rescue place round trip, packages, two-player mission and animated model | `ba3fd78b9cc273f9044c702fd8edf45f8725b86d` | [Native CI on the isolated gameplay integration](https://github.com/gmoddev/gargantuan/actions/runs/45250180300) |

Only rows with stated outcomes above are established qualification evidence.
The Relay Rescue check copies the pinned fixture, exports and restores it,
checks canonical bytes and package manifests, starts both package roles,
and runs the existing mission using the restored project. Its model check
exports RelayBeacon, provisions the source catalog through the existing
AssetService snapshot seam, and verifies that a new world's Animator can
create a track for the pinned asset. The harness prints source and artifact
digests and elapsed subprocess times.
