---
status: experimental
authority: non-normative
date: 2026-11-09
---

# Final Jolt performance campaign

The final focused optimization produced a reproducible experimental Jolt
checkpoint with cheaper admission, faster parallel large cloth, and a bounded
single-world rigid schedule. Production Box3D/XPBD, factories, defaults and Engine
main are unchanged. PR #20 remains experimental and must not be merged as a
consequence of these measurements.

This report follows the completed [M1–M6 foundation](OptimizedFoundationAndBackendDecision.md).
Its original numerical reconstruction, reciprocal coupling, retained multipliers,
sleep correction and negative controls are preserved. The sealed M1–M6 artifact
root was read only throughout this campaign.

## Source and reproduction

| Role | Exact source |
| --- | --- |
| Frozen B Engine/PR #20 | `cc097a7938ed26584eaab468fa22fabd60848fc9` |
| Frozen B maintained Jolt | `d8b980a9f4c997ff9b715d6f17d48a15ecc6de37` (native code identical to qualified `d8c46588`) |
| Native optimized runtime | `7e59df5fd7f3e23504f2e696e2b095bf706b26b4` |
| Published maintained-fork checkpoint | `0fbdedcc4ccc43a7384c91709b87e4a6c4b3a1a5` (same runtime/test/build trees, documentation and preserved README update) |
| Final Engine runtime, including all cadence guards | `3c864c58d4554f03e6a587d71d6bda3fffe06f1b` |
| Corrected hosted checker and final qualification source | `ea229c56929ad3b009667cd3a4af5071d6dca7a0` |
| Full matrix Engine executable source | `f4ffd911baab53ebee82a3d3d62e040568d17d7e` |
| Upstream compatibility baseline | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` |

The final guard source narrows eligibility beyond the full-matrix source. Matrix
soft scenes already require the high cadence; the rigid timing fixture already
has no joints and disables sleep. Fresh final-source checks confirm its 16K
trajectory exactly matches the accepted C stream and reproduce the speed benefit.
Later report/checker commits do not change this C++ runtime.

Raw evidence is preserved at
`C:/Users/aiden/.codex/artifacts/jolt-final-performance-20261109`.
It contains source bundles, frozen B/C executable hashes, command/exit/stdout/stderr
receipts, traces, predeclared quality protocols, failed and cancelled runs,
allocator profiles, independent reductions, and worker retirement receipts.
`ActiveCheckpoint.json` records progress; the final `CompletionCheckpoint.json`
and SHA-256 manifest identify the completed package. Private lease credentials
are excluded and were deleted after release.

Use the exact native runtime pin above with the existing headless Core evaluation
configuration and these explicit experiment options:

```text
PHYSICS_EVALUATION_GANTRIA_NUMERICS=ON
PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS=ON
PHYSICS_EVALUATION_GANTRIA_CONVERGENCE=ON
PHYSICS_EVALUATION_GANTRIA_SLEEP_FIX=ON
PHYSICS_EVALUATION_GANTRIA_GROUPING=ON
PHYSICS_EVALUATION_LARGE_CLOTH_GROUP_BATCH=1024
PHYSICS_EVALUATION_NATIVE_PROFILE=OFF
PHYSICS_EVALUATION_JOLT_COMMIT=7e59df5fd7f3e23504f2e696e2b095bf706b26b4
```

Supply `PHYSICS_EVALUATION_JOLT_ROOT` for that checkout. Group size 1024 applies
only to cloth with at least 16,384 vertices; smaller cloth and rubber retain 256.
This is an opt-in experiment. The original `Optimize()` symbol and its default
256 grouping remain available, with unchanged settings stream format.

All native compilation and simulation used the leased Windows worker or hosted
CI. The controller performed source work and bounded evidence reduction.
The worker used MSVC Release, deterministic SSE2, four compiler jobs and requested
runtime caps 1/2/8. Hosted qualification adds Windows and Linux ASan/UBSan.

## Accepted implementation

| Change | Commit | Evidence and correctness boundary |
| --- | --- | --- |
| Flat CSR adjacency during preprocessing | Jolt `e02c591b` | Replaces thousands of nested allocations, preserving neighbor/duplicate order and remaps |
| Cached greedy frontier scores/distances | Jolt `32ca7a12` | Preserves first-seen ties and group anchor; default graph bytes and published position streams match B |
| Immutable edge/volume mode and sweep hoists | Jolt `4d9fb598` | Existing group barrier orders sweeps; constraint arithmetic and traversal are unchanged |
| Validated grouping overload | Jolt `90f2a242` | Rejects invalid sizes before mutation; original default/symbol/serialization preserved; selected 1024 ordering explicitly changes the trajectory |
| First-sweep multiplier initialization | Jolt `7e59df5f` | Removes bulk clearing; every first-sweep constraint overwrites its multiplier, including explicit zero on degenerate skips; later sweeps retain the original arithmetic |
| One-entry immutable topology reuse | Engine `a79534968`, `237a9c244` | Exact semantic key, bounded cache, resource checks on every admission, hit counted only after successful native/slot commit |
| Bounded dynamic rigid cadence | Engine `cd8d7eb46` plus guard/staging commits | One authoritative world; fast eligibility and transitions described below |
| Final joint and sleep exclusions | Engine `203d93e4f`, `3c864c58d` | Joint membership and uncertainty force the established cadence; sleep-enabled worlds retain it |

The final native incremental diff from D8 is seven files, +620/−97 lines;
only three runtime files change (+202/−94). The rest is regression coverage and
qualification output. Against pristine e77, the qualified runtime history has 31
commits, 30 changed files (+2168/−94), including 17 files under `Jolt/`
(+681/−94). Later native documentation commits have the same runtime/test trees.
The [fork maintenance record](https://github.com/GantriaEngine/JoltPhysics/blob/gantria/final-performance/Docs/GantriaFinalPerformanceExperiment.md)
records the generic changes and their limits.

The pristine consumer compile establishes compatibility at e77, not a new upstream
version qualification. The earlier controlled upgrade rehearsal describes the
pre-campaign patchset. A later rebase must replay the new graph/permutation,
degenerate-state and physical gates; this report does not inherit that rehearsal
for source it did not test.

## Before and after

The [complete matrix](FinalOptimization/PerformanceMatrix.md),
[machine-readable comparisons](FinalOptimization/ComparisonSummary.csv), and
[source/validation summary](FinalOptimization/PerformanceSummary.json) preserve
448 uninstrumented process rows. These include production A controls, frozen B,
optimized C, isolated group trials, lambda trials and final-source confirmation.

| Target | Frozen B | Final C | Interpretation |
| --- | ---: | ---: | --- |
| 16K cloth cold creation, median ms | 55.072 | 16.985 | 3.24× faster, three fresh processes |
| Exact-definition same-world recreation, median ms | 56.115 | 0.271 | 206.9× faster; cache retained between admissions |
| Four translated independent 16K bodies, median first-body cold ms | 55.879 | 16.585 | 3.37×; translated keys intentionally miss the cache |
| Direct final-source 16K cloth W8 frame P50, ms | 70.128 | 55.833 | Three counterbalanced pairs; median paired improvement 20.38% |
| Direct final-source W8 P95/P99, ms | 76.321 / 76.986 | 57.791 / 58.915 | Median of per-process frame percentiles |
| Full Engine 16K cloth W8 P50, ms | 68.57 | 56.22 | About 18% improvement in separate aligned cohorts |
| Full Engine 16K cloth W1 P50, ms | 186.40 | 190.33 | About 2.1% regression; no universal speedup |
| Rigid-only W1 240-step total, ms | 790.40 | 231.55 | Paired median speedup 3.38× in the eligible fixture |
| Rigid-only W2 / W8 paired speedup | — | 3.35× / 3.81× | Higher caps are not faster for this small rigid scene |

Direct B/C final pairs improve 19.55%, 21.11% and 20.38%. The large full
matrices counterbalance A/B and A/C internally; B/C rows align scene/cap/tail/repeat
across those separate cohorts. They are not direct interleaved B/C pairs.
Thirty measured frames make P99 close to the observed maximum, not a production
tail-latency guarantee. Cold and warm *simulation tails* are separate from warm
*admission reuse*.

A is the unchanged production Box3D/XPBD path. Its default 16K cloth costs about
4.75 ms at requested cap 8 in the warm control, but does not match the selected
Jolt material response. It is not a quality-matched winner. The earlier explicit
XPBD High32 reference remains a separate experimental reference: a direct
comparison of its preserved trace to final C stays inside the same 5 mm RMS/
15 mm maximum envelope (4.35/12.10 mm). Its old approximately 268.8 ms timing is
historical, not a fresh counterbalanced C comparison. Equal requested caps also
do not imply equal CPU use: the one-body reference uses roughly one XPBD worker.

## Admission and memory

[AdmissionOptimization.md](Provider/AdmissionOptimization.md) records phase-level
allocation and timing receipts. Uninstrumented outer creation intervals provide
the latency ranking; allocator/profile executable times are diagnostic only.

At 16K, first-create tracked allocation calls fall from 163,857 to 507. Optimizer
calls fall from 163,642 to 290 and requested optimizer bytes from 10,199,596 to
6,370,856. The native body still needs its own mutable motion, vertex and
multiplier arrays. Its settings and immutable indices can be reused safely.

The cache is backend-local and single-entry: at most 16,384 vertices, 100,000
edge/volume constraints, 50,000 faces, 150,000 indices and 256 attachments.
These are input-count bounds, not a general byte-budget allocator. Position and
ordered absolute pins are part of the exact bitwise key: subtracting translated
world coordinates can change float32 rest geometry. Approximate/canonicalized
cross-position reuse was excluded. Validation, live body/vertex/constraint/layer
and queue budgets still run on a hit.

After body retirement, C retains about 3,094,235 tracked bytes for reusable
topology, versus 2,256 in B. Eviction and world teardown release it. Concurrent
tests admit two bodies sharing immutable indices, impulse only one, compare the
other to an independent world, retire/evict while it remains live, and verify
independent motion, identities and topology.

Fixed world memory is intentionally unchanged: the 32 MiB temporary arena,
approximately 29.99 MB world/body/contact reservations, and worker initialization
dominate the roughly 63.55/63.70 MB tracked world live bytes at caps 1/8. C adds
only 40 bytes to those world snapshots. First-body incremental live bytes remain
about 5.32 MB. The original 65.9 MiB headline included fixed reservations and is
not a like-for-like per-body comparison with XPBD's accounting.

The fixed setup costs roughly a millisecond, while preprocessing dominated the
old 55 ms body cost. Reducing world/contact capacity without an equivalent capacity
contract would change admitted workloads. A speculative allocator/arena rewrite
was not justified by these measurements. Requested allocation bytes, process
working set, phase deltas and process-lifetime peaks remain separate measures;
allocator/OS residency after teardown is not a leak proof. The B world profile
has one admission while C has three, so whole-harness net counts are not directly
comparable.

## Large soft execution and limits

The unchanged cadence is U2/C8/I5/Q2: 80 temporal integrations and 160 constraint
sweeps per frame. Neither resolution, material, geometry, damping nor numerical
quality was reduced for the speed result.

For 16K cloth, group256 has 65 update groups and 8,834 serial edges; group512
has 33/7,406; group1024 has 17/3,810. The selected graph sends 93,216 of 97,026
edges through parallel groups. In the final diagnostic profile, edge scope calls
fall 416,000→108,800, nested `FinishIteration` work falls 41.2%, barrier work 20.1%,
and integration work 35.9%. These nested scopes overlap and are not wall-clock
rankings. [ProfileComparison.md](FinalOptimization/ProfileComparison.md) separates
that mechanism evidence from the uninstrumented results.

Lazy multiplier initialization alone gives a consistent 2.20% median improvement
for W8 16K cloth in three direct pairs at group256, with exact position streams.
Rubber signs are mixed; a further five-pair check has a −1.56% median with one
slower pair. No broad rubber speed claim follows. Small cloth, rubber and W1
results include neutral results and regressions in the complete matrix.

The selected group size is fixed across worker caps, preserving the same constraint
order and position stream across those caps. Automatically switching group size
by worker count would introduce a second trajectory policy. Default256 remains
available when its tradeoff is preferred.

State extraction is a small fraction of this large-body cost. Its owned snapshots
must survive asynchronous readers and retirement; mutable buffer reuse was not
substituted for that contract. Contiguous constraint storage already exists;
broader coloring/SIMD/SoA changes would alter dependency ordering or add upstream
divergence. This campaign measured and implemented the bounded grouping and
invariant-work changes rather than claiming those larger unimplemented designs
were benchmarked failures.

## Single-world selective scheduling

The fast branch requires a sleep-disabled world, zero enabled soft bodies, zero
touch-enabled rigid bodies/sensors, zero live joints/attachments, and no unresolved
native mutation uncertainty. It performs one full-frame update with at least four
collision steps, honoring a higher requested count. Every other scene retains
U2/C8. Membership changes are committed only after successful admission and are
checked before the first affected step. Stale IDs do not adjust counts.

The fixture has 128 dynamic spheres and a floor with real native contact pairs in
240/240 frames. All fast/full runs stay finite. Worst fast floor/pair penetration
is 0.01402/0.01673 m under the unchanged 0.25 m bound; full cadence is more precise
in that fixture. Touch/sensor crossings, invalid updates, uncertainty injection,
soft enable/disable/streaming, joint add/remove/endpoint retirement, and retained
sleep/wake behavior have explicit checks.

This is a bounded dynamic whole-world optimization. It does not restrict the high
frequency to arbitrary active islands. Jolt's collision/update graph, soft
integration boundaries and contact ownership are global at this API boundary.
Lowering global cadence while increasing local sweeps is not an equivalent solve.
The previous explicitly authored independent partition prototype remains intact;
no partition migration, delayed impulse queue or second authority was introduced.
See [SingleWorldSchedulingEvidence.md](Optimization/SingleWorldSchedulingEvidence.md).

## Physical qualification and unsuccessful evidence

Default256 CSR/frontier/hoist/cache/lazy runs retain all tested optimized graph
fingerprints and published position streams. Native unit tests also cover
positions, velocities and constraint deltas across workers, degenerate first-sweep
recovery, default overload parity, valid permutations and rejected group sizes.

The selected 1024 graph deliberately changes the large-cloth trajectory. Its
600-frame W8 comparison to frozen B remains finite with stable identities and
advancing revisions: peak RMS difference 0.646 mm, maximum vertex difference
1.818 mm, pin drift below 0.0001 m, peak axial strain 4.140% and final 0.351%.
The fixed difference gates remain 5/15 mm. Small cloth at 600 frames and rubber
at 300 frames match B bitwise in every published position and logged hash.

The 16K driven-sphere stress fails the predeclared 10% strain limit in both B and
C: about 68% compression and 44% tension, localized at the forced contact shell.
Both remain finite with pins intact, actual native contact vertices and no measured
penetration; relative C/B displacement remains small. The moving sphere forces
about one metre into the cloth edge. None/static controls isolate that forcing;
the static sphere never reaches contact. This is not an accepted driven-contact
quality result. [LargeClothContactStress.md](Provider/LargeClothContactStress.md)
preserves the failed report, edge/frame coordinates, source mechanism and prior
qualification scope. No fixture or threshold was retuned to make it pass.

Other unsuccessful/unselected evidence is retained:

- Group512 passes its forty-frame bounds but gives only about 2.45% direct W8
  improvement, versus about 20.6% for group1024 before the lazy addition; it was
  not selected. Group128 has permutation/unit coverage, not this physical profile.
- Automatic broad joint/sleep eligibility was removed after review because the
  measured fast fixture did not qualify those behaviors.
- Early diagnostic scope, capture-root, private-test-access, MSVC fixture and
  no-exception assertion mistakes were corrected with failed logs retained.
- Hosted provider checks initially used thirty physical frames with a final
  bound prescribed for forty. Both hosts produced identical traces matching C's
  first thirty frames. The corrected checker uses ten warmup plus thirty measured
  frames, verifies all forty hashes and geometry, and retains the original
  5% peak/2% final/0.0001 m pin limits. A rehashed negative trace at 2.91% final
  strain still rejects. The failed hosted archives remain part of the evidence.
- One provider build was cancelled when the final guard source superseded it;
  cancellation is not a qualification pass.

## Hosted gates and independent review

Native source7e passed Windows Release, Linux Release and Linux ASan/UBSan unit CI:
[run 42576009022](https://github.com/GantriaEngine/JoltPhysics/actions/runs/42576009022).
The published checkpoint passed all three again in
[run 42585194264](https://github.com/GantriaEngine/JoltPhysics/actions/runs/42585194264).
The original native observation suite passed 24 positive cases and rejected both
negative controls on each host. The original unified suite passed 45 paired,
three extended and two negative-control cases on each host. Their raw archives
are preserved from [run 42576423815](https://github.com/gmoddev/gargantuan/actions/runs/42576423815);
the overall workflow failed later in the newly introduced thirty-frame checker,
not in those original suites.

Final worker source3c passed three fresh runs each of provider, cache and schedule
qualification. The pristine e77 consumer compiled with every fork feature OFF;
the worker then restored final7e, unprofiled1024 and passed the three qualification
targets again. Corrected hosted final provider Windows and Linux ASan/UBSan both
passed [run 42582508499](https://github.com/gmoddev/gargantuan/actions/runs/42582508499):
three fresh repeats each of provider, concurrent cache and scheduling fixtures,
plus the 40-step large-cloth checks at requested caps 1 and 8. All four traces
are byte-identical across hosts/caps, SHA-256
`7ecea70b984742ef0aae91c67e7c52e55e440ac111f307cb0c057aa22189346a`,
with terminal position hash `12069508638256050222`. Each verifier recomputes all
40 hashes. The [final receipt](FinalOptimization/FinalQualificationSummary.json)
records exact sources, physical gates and worker release.

Independent workstreams cross-reviewed graph order, sweep barriers, degenerate
multiplier state, strict cache keys/caps/lifetimes, native/slot staging, guard
membership, timing/resource scopes, all-frame hashes and the checker correction.
Concrete findings closed include staged soft/rigid storage, all-touch guards,
fail-closed partial mutations, live-joint/sleep exclusions, profiler labeling and
the thirty/forty-frame mismatch. Existing rigid mesh-map and joint-slot allocation
after native mutation remain baseline exception-atomicity limits; the new latches
keep cadence conservative but do not claim to make every old operation atomic.

## Decision and remaining adoption blockers

Keep this optimized maintained fork and Engine experiment as the future unified
backend candidate. Keep production Box3D+XPBD in place. The results establish a
bounded improvement with a small generic patch increment; they do not establish
an optimal universal solver or justify an Engine migration.

16K cloth still costs about 56 ms at eight workers, above a 60 Hz frame budget.
The large driven-contact strain failure, soft-versus-soft/self/swept collision
limits, broader attachment graphs, production workload/streaming/session coverage,
semantic parity and inherited failure atomicity remain adoption constraints.
The [feature matrix](Provider/FeatureMatrix.md) retains the exact admitted scope.
Single-worker regressions and cache-retained memory are explicit tradeoffs.

The measured primary opportunities have implementation-backed outcomes. Preserve
the checkpoint and its regression gates, conclude this optimization campaign, and
return development effort to the other Engine systems. This report does not open
another general physics architecture campaign.

## Worker retirement

Task-owned native jobs were verified idle, frozen B/C hashes rechecked, and the
final source/configuration restored. Persistent source/build/dependency caches
were preserved. Heartbeat process35892 exited; lease generation44 was released
at `2026-11-09T07:38:07.648845+00:00` with state Free. The private token was deleted.
`WorkerRetirement.json` and `WorkerLease-release.json` record the cleanup; unrelated
processes, workloads and leases were untouched. Hosted CI resources terminate
with their own jobs.
