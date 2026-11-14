# Engine Foundation 3–6 mainline integration

This is the integration ledger, not a replacement for source-bound historical
qualification. Fresh execution results are attached to the integration PR and
its exact-source Actions artifacts. Pending, failed, blocked and unmeasured
gates are not passes. A draft PR must remain unmerged while mandatory gates fail.

## Baseline and mainline reconciliation

Inspected on 2026-11-14. Canonical remote is `gmoddev/gargantuan`.
Latest mainline baseline is `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`.
Development fork is `5b6782e35c4e24fd80bc8d5526104c615c72d84d`.
The exact mainline/development divergence is one versus 271 commits.
The mainline-only commit changes four README lines clarifying project provenance;
it touches no build, runtime, renderer, network, physics, dependency, CI or test
contract. Every consolidated stage preserves that README blob. No invariant
change or architecture reconciliation is required.

Foundation 3 is not already merged. The complete accepted source chain is linear:

| Stage | Original head | Provenance |
| --- | --- | --- |
| Renderer 3 | `81a5c5aff36e2149c3181ada6858d7143da19ea0` | `foundation/renderer-3` |
| Renderer 4 | `94f920485564798742bc22a061cbc3a415034fa3` | PR #21, based on Renderer 3 |
| Renderer 5 | `da902ea481e627d7c4d789033076b3be7dc38334` | PR #22, based on Renderer 4 |
| Runtime 6 | `741146b810d0eb59e1589248f8ccefa31f2bcf1d` | PR #23, based on Renderer 5 |

Runtime implementation checkpoint `f686a4fbce5c21d6cec89bbb08499586b1893378`
precedes five documentation-only commits. Their cumulative changes affect only
KNOWN_ISSUES, FoundationRuntime and RuntimeFoundation6Qualification. Preserve
the later accepted documents and all earlier failed/rejected evidence.
Physics experiment PRs #19 and #20 are separate draft work and are not promoted.
Existing development and experiment branches/worktrees are retained. The user's
dirty `foundation/3l-content-availability` checkout is not the integration tree.
Studio source is outside this campaign.

## Source-controlled reconstruction

Branch `integration/engine-foundations-3-6` starts at the mainline SHA above.
The first four commits are the entire accepted deltas for each foundation:

1. `git merge --squash 81a5c5aff36e2149c3181ada6858d7143da19ea0`, then commit.
2. Apply `git diff --binary --full-index <Renderer3> <Renderer4>` with
   `git apply --index --3way`, then commit.
3. Apply the identical process for `<Renderer4> <Renderer5>`, then commit.
4. Apply the identical process for `<Renderer5> <Runtime6>`, then commit.
5. Commit the bounded integration compatibility, checks and ledger changes.

The four intermediate trees each equal their respective accepted source plus
the mainline README. Original attribution is retained in coauthor trailers;
source-range and original PR trailers identify the complete provenance.
No historical branch is rewritten or force-pushed. Four capability commits are
coherent; VM, GUI and runtime safety remain with the runtime implementation
because separating their shared lifetime changes would create artificial states.

Run `python tools/ci/VerifyEngineFoundationIntegration.py --candidate HEAD`.
It compares every tracked Git entry, including file mode, blob and submodule
identity; checks each intermediate stage; requires the mainline ancestor and
README; rejects missing or unclassified changes; preserves every historical
validation blob; enforces the ten-commit maximum; and emits a JSON report with
the exact candidate tree and per-path differences. Keep that report with the
fresh receipts. Source equivalence does not imply binary qualification.

## Classified integration changes

The verifier carries the exact path allowlist. There are no unreviewed runtime
feature changes. Main-owned Instance/DataModel, full ObjectId generations,
immutable RenderPublication, renderer-owned RenderProjection, SDL GPU, existing
3I/3J/3L and F1 contracts, solver ownership and scripting/EditorHost boundaries
are preserved. Instancing, compact traversal and the scene envelope remain opt-in.
Rejected SoA/ECS, global dirty bus and control/readback experiments stay rejected.

The independent packaging review found three compiled shaders omitted from the
canonical distribution: `opaque-portable.frag`, `opaque-instanced.vert` and
`shadow-instanced.vert`. The portable material and opt-in instance paths load
the first two executable-relative files. Stage all 18 outputs, verify both
player and server distributions separately, and check actual package presence
and fail-closed rejection before output publication. Existing package schemas,
historical qualified schema-v4 fixture and ordinary schema-v5 capture remain.

The renderer review found forced-SDR GUI timing assigned to the omitted HDR
resolve phase. Retain each constructed pass's phase alongside its lifetime;
pass order and Draw calls remain unchanged. Real Block/Ball/Cylinder tests now
assert HDR/fallback and forced-SDR phase attribution, and execute in software
Vulkan as well as physical qualification. Shader semantics, depth comparison,
GPU resources and renderer defaults are unchanged. This timing correction
requires fresh measurements; historical phase timings are not relabeled.

The pre-existing `SetAssetMeshChanges` invalid-tail recovery defect is recorded
in KI-RND-002. It predates Foundation 3, is already present in main, and is not
silently attributed to this squash or repaired through an unrelated publisher
transaction redesign.

## Acceptance gates and evidence ownership

Required fresh qualification for the consolidated source:

- Default exact-source manual Native CI: full Windows Release CTest, scripting,
  VM/GUI/input lifetime, networking/admission, physics, EditorHost, serialization,
  five Release workloads, package generation and both native runtime projections.
- Default exact-source manual GNS sanitizer CI, including all admission cases.
- Complete Linux Release with benchmarks enabled and unfiltered CTest. Existing
  `runtime6-linux-release.yml` on `experiment/runtime6-evaluation` accepts an
  explicit exact source; retain its workflow-source and execution-source identities.
- Native Linux ASan/UBSan/LSan and Renderer 5's sanitizer matrix, separately.
- Exact-source Renderer 5 software Vulkan material/HDR/SDR, probe/shadow,
  depth/order, residency/recovery, real-Part eligibility and shader validation.
- Physical AMD and NVIDIA: identify device, driver, binary/shader/fixture hashes;
  run the retained correctness, lifetime and real-Part controls on each available
  adapter. An unavailable required device is a named blocked gate. Software
  Vulkan and AMD never qualify NVIDIA. Preserve raw failures and aborts.
- PR merge-checkout CI and the exact candidate's source markers/artifacts must
  be terminal green before supported merge; post-merge main CI and smoke remain
  separate obligations. No admin bypass or check weakening is authorized.

Historical counts (Windows 160, Linux Release 118, Linux sanitizers 107, GNS 11)
describe their original sources. New distribution tests may change current
registration counts. Use retained unfiltered JUnit and command exits, not copied
counts, to determine fresh results.

Performance references remain historical: 50K dense publication 84.20 to 53.35
ms; 50K adoption 56.15 to 1.33 s; selected unshadowed opt-in 50K opaque drawing
50,000 to 98 batches. The live shadowed world is 50,100 total draws, including
50,000 individual shadow casters. Run matched same-machine controls for changed
executable behavior; do not claim numerical reproduction on unmatched hosts.
The bounded changes here affect packaging and metric attribution, not world
mutation, extraction, physics algorithms or shader output. Preserve dense setter,
10K and dense-50K instancing regressions and all tail limits in the original
Runtime 6 and Renderer 5 qualification documents.

### F1 disposition

D03 `POOLED_CORRECTNESS_V1` is the accepted core-mainline contract; see
`tests/Foundation3LFinalAcceptance.md` and the accepted D03 ADR. Correctness,
bounded ownership, reliability, conservation, ordering and cleanup remain fatal.
The 250 ms ACK-gap target is unchanged. Its wall-clock misses are diagnostics
for core acceptance and remain failures of that strict latency criterion.
The Runtime 6 frozen 32-simulated-peer replay missed it in five of eight
processes per arm; it neither proves a candidate-specific regression nor passes
latency qualification. The original strict `HYBRID_F1_V1`, Local32/real-TLS Node32
and physical/provider certification retain their exact separate gates. This
integration cannot certify them. No threshold or network contract is modified.

## Independent review

Three independent reviewers inspected actual source and tests: ancestry/build/
packaging/equivalence; Instance/runtime/physics/networking/VM lifetime; and
renderer/shader/material/resource/EditorHost boundaries. Each verified the fourth
reconstructed tree against Runtime 6. The packaging and metric defects above
are integration corrections; reviewers recheck final changes and test results.
Review records attached to the PR must identify the reviewed SHA, concrete
commands, defects, resolutions and limits. Static source review does not claim
execution coverage. Final gate status belongs to exact-source evidence.

## Studio handoff

Before a successful merge, final Engine main SHA is **not assigned**. Publish
it only after verifying the resulting main tree. Existing authoritative handoff
is `devdocs/CurrentArchitecture/RendererFoundation5StudioHandoff.md`.
Handshake protocol and PropertyBatchVersion must be discovered; schema v7 is
preferred with v6 fallback. Material artifacts preserve old v2/v5 and newer
extended encodings; linear HDR Image artifacts use v7. No new integration
schema or wire version is introduced.

Studio must run the six retained handoff fixtures: legacy open/schema discovery;
HDR import/Sky save/reopen/reimport identity; probe edits/overlap/asset lifetime;
cascade and bounded spot overflow; stale revision/schema and invalid value
rejection; viewport transport/resize/presentation diagnostics. Shader/material
semantics retain linear color/data separation, masks, sidedness, strict LESS,
legacy SDR fallback and HDR resolve. Unsupported features remain point cube
shadows, automatic probe capture/GI, EXR, general LOD/compression authoring and
automatic exposure. Paired Studio integration is a separate campaign; Engine
GPU tests do not certify Studio UI wiring. No Studio source changes are included.
