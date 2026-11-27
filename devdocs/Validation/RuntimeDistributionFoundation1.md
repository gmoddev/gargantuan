# Runtime Distribution Foundation 1 qualification

Engine development branch `foundation/runtime-distribution-1` starts at `ff5cf28f8089426251e761fb1ce532bf572a4698` with the exact 10A candidate `28a3c254ba41a79f2ee1a79cb4d434a3d2644a72` applied in `85b95ff69`. Studio candidate `bf9abf07434501a028260bd209fba0a6667a2ff9` remains separate from shell modernization. Node remains at the inspected `cd6294818670f3ddad5a4d93d949c711e495ec22` baseline. Its optional content and entitlement services require no change for local and host-provisioned keys.

| Surface | Focused evidence |
| --- | --- |
| Compiler origin | Windows standalone compiler probe linked the pinned Luau archives and verified stock byte equality, debug metadata separation, syntax rejection, incompatible compiler identity, and VM execution. |
| Crypto | Windows standalone enabled and disabled MSVC Release builds passed. The enabled test used a digest-verified OpenSSL 3.6.5 SDK and checked independent Ed25519 and HKDF/AES-GCM known answers, context substitution, wrong keys, malformed inputs, limits, and scoped enrollment. It also passed a separate Windows AddressSanitizer run. |
| Stage logic | Synthetic Windows Player, network Player, and Server stages produced exact executable digest receipts. The OpenSSL SDK import library resolved its DLL and staging rejected conflicting destination bytes. |
| GSES framing | A standalone Windows build of the native wire codec passed legacy and managed round trips, exact identity checks, downgrade rejection, and truncated-frame denial. The Engine package, host, replica and integration-test translation units passed MSVC syntax checks against the retained dependencies. This scope does not execute GNS or a game. |
| Studio | Pinned Luau 0.744 checks and .NET 10 infrastructure tests passed. An exploratory component run used the unchanged baseline inventory of 64 manifests. The standard runner expects 63. The approved LUI pin was preserved. |
| Independent review | Static review found late binding timing, generation confusion, source digests in GUI provenance, and external launcher and enrollment trust limits. The candidate moves binding ahead of replica notifications, pins rematerialization to its original source identity, verifies managed GUI provenance, and rejects incompatible known bindings. These corrections need native re-verification. |

The separate research campaign measured 20.4% lower stripped bytecode size in its 2,048-module corpus. That observation precedes format 4 and cannot establish a production frame budget.

[Windows crypto Release](assets/runtime-distribution-1/WindowsCryptoRelease.log) ·
[Windows crypto AddressSanitizer](assets/runtime-distribution-1/WindowsCryptoAsan.log) ·
[Windows GSES codec](assets/runtime-distribution-1/WindowsGsesCodec.log)

## Linked gates

Use source-pinned Windows and Linux Engine builds with tests and approved OpenSSL 3 and GNS. Preserve compiler, Luau gitlink, dependency, platform, and binary SHA-256 receipts. Build both native runtime distribution targets and run Packaging, Foundation, content, GameSession, and crypto tests. Include Linux ASan, UBSan, and leak runs. From the Engine root, run the native acceptance harness:

```powershell
python tests/RuntimeDistributionQualification.py `
  --packager <gargantuan-packager> `
  --player-distribution <RuntimeDistribution> `
  --server-distribution <ServerRuntimeDistribution> `
  --reports <new-output-directory> `
  --repeats 3 --extra-scripts 256 --loopback
```

The runner builds legacy Source, Open, Compiled, signed Compiled, Protected and Hybrid controls. Its FirstCompleteGame fixture covers readable crafting and examples, compiled GUI and callbacks, protected procedural logic, mixed requirements, and private Server authority. Synthetic key provisioning separates the build and runtime roles. Preserve actual artifact, startup, and rollback receipts.

Public acceptance also needs Foundation 10A publication qualification, Studio packaging through the exact EditorHost pair, actual initial and live GNS source-marker observation, wrong-pair rejection, generation-safe rematerialization, and reconnect. Record cold and warm treatment, use existing `Scripts` and frame scopes, and record p50/p95/p99, peak working set, allocation, GC and cache effects. Compare eager and lazy eligibility before changing the default decryption boundary.

**Promotion remains open.** The shared Windows native-build worker was occupied by an independent LUI campaign during implementation. The reachable Ubuntu host lacked a C++ and OpenSSL development toolchain. The focused checks leave protected Player/Server execution, Linux packaging and integrated performance unverified.

## Release licensing

See [runtime release licensing and verification](RuntimeReleaseLicensing.md) for the stage and export checks on this branch.
