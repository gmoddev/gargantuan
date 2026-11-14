# Engine Foundation 3–6 mainline integration

This is the integration ledger, not a replacement for source-bound historical
qualification. Fresh execution results are attached to the integration PR and
its exact-source Actions artifacts. Pending, failed, blocked and unmeasured
gates are not passes. A draft PR must remain unmerged while mandatory core gates
fail. Device-specific certification has the explicit scope defined below.

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
6. Record the source-bound qualification evidence and explicit core versus
   device-certification acceptance policy. This documentation-only follow-up
   preserves the fifth commit's executable/build/test/shader source tree.
7. Keep phase attribution stateless after investigating the metadata-vector
   variant's reproducible Linux CPU concern. Retain the prior variant's receipts
   and require fresh source-bound comparison and rendering gates before promotion.

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
resolve phase. Walk the immutable constructor definitions with the same HDR
filter used during construction; pass order and Draw calls remain unchanged.
No extra phase vector, backend layout or metadata allocation is introduced.
Real Block/Ball/Cylinder tests now
assert HDR/fallback and forced-SDR phase attribution, and execute in software
Vulkan as well as physical qualification. Shader semantics, depth comparison,
GPU resources and renderer defaults are unchanged. This timing correction
requires fresh measurements; historical phase timings are not relabeled.

The pre-existing `SetAssetMeshChanges` invalid-tail recovery defect is recorded
in KI-RND-002. It predates Foundation 3, is already present in main, and is not
silently attributed to this squash or repaired through an unrelated publisher
transaction redesign.

## Acceptance gates and evidence ownership

Mandatory fresh core qualification for the consolidated source:

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
- Available physical default-path validation: identify device, driver and
  binary/shader/fixture hashes, then run the retained correctness, lifetime and
  real-Part controls. The available RX 7900 XT supplies the AMD evidence here.
  Run the corresponding GTX controls when worker admission is available;
  unavailable NVIDIA execution remains a named blocked per-device certification.
  Software Vulkan and AMD never qualify NVIDIA. Preserve raw failures and aborts.
- PR merge-checkout CI and the exact candidate's source markers/artifacts must
  be terminal green before supported merge; post-merge main CI and smoke remain
  separate obligations. No admin bypass or check weakening is authorized.

### Explicit core integration acceptance policy

The standing build contract in `ContinuousIntegration.md` defines Native/GNS
required checks and places hardware GPU execution outside the headless gate.
Renderer Foundation 3 distinguishes functional software Vulkan from matched
physical hardware rankings. Renderer Foundation 5's earlier "both physical
GPUs" pending statements are explicitly historical checkpoints, superseded by
its recorded source-bound final evidence. Runtime 6's accepted disposition
retains GTX real-Part qualification as blocked while qualifying its bounded
implementation and keeping instancing opt-in. None establishes a standing fresh
NVIDIA prerequisite for every core source integration. D03's separate physical/
provider networking policy is not a renderer waiver.

For this consolidation, core mainline acceptance requires all mandatory exact-
source and PR CI above, complete Linux Release, renderer software/sanitizer
validation, available AMD physical correctness, source equivalence and independent
review, plus disposition of meaningful performance differences. A blocked NVIDIA
lane does not prevent that scoped core merge. It does prevent any claim of fresh
NVIDIA parity, performance, real-Part eligibility or device certification.
Unchanged shader behavior and renderer defaults do not transform historical GTX
receipts into current-source execution. Instancing remains opt-in; no unsupported
feature, physical/provider release or universal-device claim is promoted.

This is an explicit scope correction, reviewed independently against the user's
"where required and available" and "or documented acceptance policy" directives.
The first version of this ledger accidentally imposed a stronger fresh-both-
adapters requirement than the existing core policy. That wording is superseded
by this section; no actual result, threshold, required CI job or invariant changes.

### Source-bound fifth-commit evidence

The following fresh runs execute compatibility checkpoint
`6e1197240eb107d1cc7b7eb904054858c378a4ab`; their identities are not relabeled
as qualification of later documentation-only heads. Fresh final-head CI and
receipts remain required. The integration PR retains original metadata, ZIP
digests, source markers, raw samples, independent reviews and final disposition.

| Checkpoint evidence | Result / boundary |
| --- | --- |
| GNS sanitizer run `43592337572` | 11/11 CTests, production admission matrix; exact source marker |
| Full Linux Release run `43592342380` | 120/120 unfiltered, no failures/skips; both new distribution tests executed |
| Native Linux sanitizer job in `43592335284` | 109/109 unfiltered, no failures/skips; exact source marker |
| Renderer run `43592339907` | Software Vulkan, Windows package/focused tests and renderer sanitizers pass |
| Physical RX 7900 XT | 13/13 correctness/lifetime cases, including HDR/SDR metrics and two ordered 4096-frame churn cycles; 16/16 matched draw-mode processes |
| Windows core comparison `43592354376` | 66/66 process receipts; 50K dense publication 53.683 to 54.345 ms, adoption 1434.363 to 1472.850 ms, counts/checksums equal |
| Linux core comparison `43592356339` | First cohort has 3–8% dense-path timing differences; no hot-path source change; normalized selected instructions match but linked layout differs |
| Linux matched follow-up `43594510430` | Requested to investigate that first-cohort difference; preserve both cohorts regardless of result |
| GTX execution | Not started: another campaign holds the cooperative worker lease; no inferred result |

The selected unshadowed static AMD fixture still records 50,000 to 98 opaque
world draws, with two additional non-world draws per frame. Dense 50K dynamic
instancing regresses whole-frame tails; fewer draws do not promote the option.
These are comparisons of modes of one executable, not source-revision speedups.

### Stateless phase-attribution follow-up

Both retained Linux cohorts (`43592356339`, `43594510430`) run the same frozen
Runtime 6 / metadata-vector binary pair on different Xeon hosts. Counts, checksums
and receipts match, but high-density publication is consistently about 3–5%
slower and the allocation-probed dense dirty path about 9–11% slower in the
candidate. The affected headless benchmark does not execute SDL pass recording.
Selected runtime/benchmark methods have matching normalized instructions while
linked placement changes, so an indirect layout effect is plausible, not proven.
These results remain a performance concern, not a correctness or F1 failure.

The bounded follow-up removes the extra backend phase vector and its associated
layout, template and allocation changes. Phase lookup instead walks the existing
immutable definitions using construction's HDR filter. It preserves all metric,
pixel, ordering, lifetime and negative controls. Fresh matched CPU and renderer
execution must evaluate this source before promotion; the integration PR owns
the exact resulting source identity and disposition. Neither earlier cohort is
deleted, relabeled as passing parity, or treated as final-head execution.

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
