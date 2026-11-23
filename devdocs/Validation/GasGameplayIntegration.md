# GAS Foundation 1 and Gameplay Foundation 7 integration

The combined development branch starts from Engine main
`e7bec187def55e79c6ca956b17feb82926e5d1fe`. Gameplay Foundation 7
`06d1a2015b26f8610a74a6b6eea39b179a4417a8` and the follow-up through
`8df7db2f9675cd2fa92f37d8dafb80af10e7cf9a` provided the Relay Rescue
project and its network runtime. GAS first joins that line at `23f8436c6`.
The later development checkpoint `293dc8daf3891f6b7cfe3676b20a5f4572c3c45d`
includes the subsequent GAS qualification updates through `32159720b`.
The [GAS receipt](GASFoundation1.md) retains the replay at the final
standalone GAS branch `966bc9cb11c1e58d6cf8b6f5f061b26c406612f5`.

## Integration checks

- The [GAS authoring contract](../../docs/protocols/GAS-Authoring-v1.md)
  describes canonical GASM/GASP, reference and asset pins, transactional
  insertion, place opening, and the existing immutable package builder.
  The optional protected-export proposal has a separate scope.
- The [Gameplay qualification record](GameplayFoundation7.md) covers the
  replaceable Luau Character, generic possession, collision, animation, and
  the saved Relay Rescue mission. Server-origin authority and replica trust
  remain defined by the [possession protocol](../../docs/protocols/gameplay-possession.md).
- The combined Relay test loads the exported GASP directly into the trusted
  client script source and authoritative server graphs while the JSON scene
  is absent. It also compares canonical re-export bytes and GASP versus
  restored JSON package manifests for both roles. Packaged Player and Server
  startup is a separate check within the test. It exercises simulated
  transport and uses real GNS loopback when built with GNS.
- The promotion PR carries the tested source and its tree-equivalence proof,
  native Windows Release and Linux sanitizer results, GNS sanitizer and
  documentation results, and Windows Release workload outcomes.

## Retained runtime evidence and limits

The Windows Release worker receipt at `4eddc265ac34663b26cb3b3a988dd1f20df73460`
(tree `293890cbea3df29affc37d8c1697e5308a6eefca`) passed 23 focused CTest
cases, with 0 failures, using MSVC toolset 14.51.36231, eight build jobs and GNS
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. The test XML SHA-256 is
`0ac430153d71a2c1b3a200a14cc24e4b3f729da7e5bf5b1444a137ec40133f76`.

At the same source the [offscreen GPU journey](GameplayFoundation7GpuJourney.md)
passed over real GNS loopback on Dockerbox. The two Vulkan clients completed
317 and 318 frames. The pilot and observer p99 between draw starts was 26.25
and 25.71 ms, respectively, including the fixture's serial peer steps and
deliberate wait. The retained GPU log SHA-256 is
`383212b81fe7332df29afa5f6553f5a1ddf577af9db0b3ae909f4ac1257b2e16`.
Raw artifacts are under `C:\Sandbox\Codex\Artifacts\gargantuan-gameplay-7-maturity`.
Production 60 Hz, physical multi-host traffic, input-to-display latency, and
independently paced graphical clients require their own evidence.

The [opt-in Release work trace](../Compiling.md#local-performance-traces)
keeps the normal Release configuration available and measures construction,
snapshot preflight, live replica apply, and host frame pacing. GAS place load
also pays reflected materialization costs. Evaluate direct typed materialization
and structural preflight allocation reductions with matched correctness and
performance fixtures. GAS import and live replication have distinct authority,
transaction, and observer contracts.

## Studio handoff

Reviewed Studio source `9e0589144720cce2a6951a32e585fd4a53c6582c`. Its
qualified Creator configuration pins the earlier Engine main. Studio already
supports saved Player policy, input actions, Character composition, and Play.
Use a fresh promoted Engine artifact to rerun those paired journeys, including
a developer-authored Character. Relay Rescue can use a packaged Server and two
clients while Studio's multi-client TestSession remains deferred.

1. Gate first-party GAS commands on the four optional method names advertised
   in the EditorHost protocol v1 handshake. Add a native picker and bounded
   binary transfer path for `.gasm` and `.gasp`. EditorHost accepts padded
   Base64 chunks of at most 65,536 bytes and 512 KiB of decoded bytes overall.
2. For model import, stop Play, close edit transactions, pass the live parent
   and expected revision, then reconcile the ordinary journal and select the
   returned root. Expose the existing single Undo/Redo action. Asset-backed
   imports need the original catalog IDs and matching CAS digests, including
   dependencies. Ordinary asset import generates a new ID.
3. For place open, resolve dirty scene, source and PreRun buffers before
   replacing the current project's authoring scope. Drop its cursor, revision,
   selection and scope-bound consumers. Fetch a new initial snapshot without
   polling the old cursor. The place stays dirty until the existing JSON save.
4. Exercise selection order, stale revision, Play refusal, transfer limit,
   missing pins, correct and incorrect package identity, scope replacement,
   and explicit save/reopen through native Studio.

Studio's [native allowlist](https://github.com/GantriaEngine/gargantuan-studio/blob/9e0589144720cce2a6951a32e585fd4a53c6582c/experiments/LuiStudio/Host/StudioBinding.cs#L337)
is the first integration seam. Its
[current reconciliation](https://github.com/GantriaEngine/gargantuan-studio/blob/9e0589144720cce2a6951a32e585fd4a53c6582c/experiments/LuiStudio/App/Src/Main.luau#L835)
polls the current cursor before fetching another snapshot, so place opening
requires a dedicated scope-replacement flow.
