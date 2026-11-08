---
status: experimental-qualified-bounded
authority: non-normative
date: 2026-11-08
---

# Optimized Jolt foundation and long-term backend decision

**Decision:** maintain unified Jolt as Gargantuan's future candidate for authoritative
rigid/soft interaction, while retaining Box3D + XPBD in production. The maintained
patchset now has a demonstrated numerical foundation, useful optimizations, a real
Engine provider, and a measured ownership cost. It is a credible alternative for
coupled physics. These results do not justify a production replacement or establish
a universally optimal solver. Large soft bodies remain expensive, and the remaining
feature limitations below are material to adoption.

This completes the bounded implementation and feasibility campaign M1–M6. The S1
root cause and its correction are resolved for the qualified scope; S1 is not a
deferred investigation. The final recommendation incorporates successful and failed
experiments rather than treating every passing API call as parity.

## Source and deliverables

The independent, public [GantriaEngine/JoltPhysics repository](https://github.com/GantriaEngine/JoltPhysics)
preserves Jolt ancestry, MIT license, source patches, tests, CI, and upgrade branches.
No private Engine source or credentials were copied into it. Engine work is isolated
in [draft PR 20](https://github.com/gmoddev/gargantuan/pull/20), based on the prior
experimental checkpoint, not main. Production factories, physics defaults, semantic
authority, persistence, and main are unchanged.

| Identity | Exact commit |
| --- | --- |
| Pristine Jolt v5.6.0, `upstream-base` | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` |
| Maintained numerical foundation | `109be79fa82be0b306c56e4b4357675ed7843767` |
| Separate attachment branch | `cd095d408a1cd231ad43fa8725d06a818fd63b13` |
| Separate convergence branch | `25658bcf45f21bb2674d47952638c31c7d0b1467` |
| Combined convergence/attachment source | `e167ba589787fb76f5d9718ae348d4ac1c3b19ee` |
| Qualified optimized native pin | `d8c46588b23700d24ebdaeada0ff460d34b1f830` |
| Published `gantria/physics` ledger, identical native source to d8 | `d8b980a9f4c997ff9b715d6f17d48a15ecc6de37` |
| Previous Engine investigation | `dd078d19d92596dd7f4b6dfff9aed8b6eef40c07` |
| Final provider/cloth/Engine qualification source | `b2df82ae58e1ea1a8eea0245e9e8592b1ed886ff` |
| Stock compatibility and restored candidate source | `057adfeb6fc7b6c3f7f6cd18bd0f789662bad581` |
| Newer upstream rehearsal basis | `0837f118d7248579789a94835407de2d56a93be9` |
| Complete forward-port rehearsal | `c50f06fdb7db27c1a527b97251411ba63d2867a0` |

`057adfeb6` removes two attachment-only preprocessor guards from private accessors
also used by contact instrumentation. The qualified attachment-enabled source path
is unchanged. Both pristine macro-off compilation/rigid tests and restored macro-on
provider tests passed on the Windows worker at that commit. The final docs and
offline receipt-hash correction are descendants of these runtime checkpoints.

## M1: numerical foundation

The prior source-level finding was a feedback loop: position quantization in
`(Pfinal - Pprevious) / h` becomes velocity at very small timesteps. The correction
retains damped velocity and accumulates the intended constraint displacement before
world-position rounding. Contact projection remains outside velocity reconstruction,
matching the existing phase contract. See
[the preserved S1 resolution](SolverResolutionAndUnifiedDecision.md).

The maintained per-body `ConstraintDelta` mode covers edge/distance, volume,
dihedral, LRA, skin, and rod stretch translations; rod angular reconstruction remains
the upstream path. Tests include individual families, mixed constraints over 600
frames, recreation/serialization, contacts, and original-mode controls. This is a
finite qualification, not exact floating-point energy conservation for every graph.

After that correction, coarse-step residual strain was a separate convergence
problem. Seven of eight one-sweep coarse screens still failed physical energy
bounds. Retained-lambda graph sweeps solve that problem at the selected cadence:
each temporal integration resets multipliers once, repeats the constraint graph,
then performs contacts and reconstruction once. Extra sweeps do not repeat gravity,
damping, or time advancement. Q1 preserves the original first-sweep expression.
Q2/4/8 reject unsupported families before body allocation; exact-hard/unqualified
provider graphs reject before native mutation.

The accepted profile is **U2/C8/I5/Q2: 16 collision advances, 80 temporal soft
integrations, and 160 constraint sweeps per frame**, versus I12/Q1's 192
integrations/sweeps. It therefore does not imply a 192/80 wall-time speedup.
N64/Q2's failing free-energy result and other failed candidates remain evidence;
the original acceptance ceilings were not relaxed.

At the final native pin, [run 42482803195](https://github.com/gmoddev/gargantuan/actions/runs/42482803195)
passed on Windows and Linux: 24 native positive cases, two expected negative
rejections, 45 paired plus three extended unified cases, and two unified controls
per OS. Spring source replay has zero float32 P/V error; pristine/observer parity,
frame hashes, scores, and cross-OS checks pass. Final d8 hashes also match the prior
f383 sleep-clock physical captures. Native patch CI independently passed Windows,
Linux Release, and Linux ASan/UBSan at
[d8](https://github.com/GantriaEngine/JoltPhysics/actions/runs/42478036231).

## M2: cost reductions with demonstrated mechanisms

**Selective cadence:** an implemented two-world partition keeps the interacting
soft/rigid/joint group at U2/C8/I12 and unrelated rigid bodies at U1/C4. Both layouts
use the same authored no-cross collision filters and share one bounded Jolt job pool
and temporary arena. Near/crossing/joint controls refuse unsafe partitions before
update. Coupled trajectories are exact across layouts, worker caps, repeats, and OS.
This is an authored independent partition, not arbitrary live island migration.

The dense 128-body gravity/contact scene gives global → selective 120-frame cost
681.14 → 420.22 ms on Linux and 472.98 → 283.93 ms on Windows at one worker:
**1.62× and 1.67×**. Unrelated trajectories differ with cadence; maximum pair/floor
penetration rises to 0.012055/0.006743 m from 0.000155/0.000206 m, within the unchanged
0.25 m gate. Sleep is disabled in this benchmark. Profiled soft work stays nearly
unchanged while rigid broad/narrowphase work falls. Nested profile scopes cannot be
summed as wall time. Additional live working set is about 0.4 MiB, not a leak proof.
See [selective evidence and reproduction](Optimization/SelectiveSchedulingEvidence.md).

**Sleep clock:** Jolt checks sleep only on the final collision step but credited
`UpdateDt/CollisionSteps`. The default-off patch supplies the exact full update
delta to that check, preserving movement/time thresholds. A second fix avoids
waking an attached rigid body for an exactly zero impulse. C1/C4/C8 timer tests,
negative old-clock controls, and attached sleep/re-wake tests pass. In the worker
U2/C8 diagnostic, 128-body stack/dense scenes now reach all-asleep at full-trace
frames 36/39; earlier runs never do in 150 frames. The measured P50 becomes
0.1955/0.4291 ms from 6.3877/14.6084 ms. Dense P95 remains 14.7148 ms: awake transient
cost remains. These before/after cohorts contain other source changes; source-level
timer controls, rather than timing alone, establish causality.

**Soft processing:** retained-lambda convergence reduced temporal work without
weakening native physical gates. Across the 60 matched provider timing groups,
candidate Jolt improved 57 and regressed three; candidate/baseline ratios span
0.826–1.040. That broad provider cohort is Engine `1fd69681`/Jolt d8 versus
Engine `795f3564`/Jolt cd095; the final reference and authored Engine cohorts use b2df/d8.
Warm 16K no-collider cloth at cap 8 goes 80.66 → 67.36 ms. Combined native
world solve dominates; extraction is only about 0.15–0.22 ms. No speculative layout
rewrite, persistent-contact shortcut, or unqualified contact-frequency reduction
was accepted. Native job scheduling uses a fixed total cap, including the main
thread, rather than an additional unconstrained pool.

## M3–M4: actual Engine integration and limits

The [provider](Provider/README.md) implements two existing neutral adapters over one
Jolt world. The rigid owner outlives the soft adapter. Engine IDs, generations,
asset identity, admission, streaming, saves, and publication remain authoritative;
native IDs do not enter persistence. No parallel Instance model or public selector
was added.

[Final provider run 42484261209](https://github.com/gmoddev/gargantuan/actions/runs/42484261209)
passed three fresh repeats each on Windows Release and Linux ASan/UBSan with vptr
and leak detection enabled. Its actual fixtures include imported OBJ collision,
stack support, canonical mesh accounting and retirement, queries/capsule wall,
welds, spring and articulated structures, authored hinge/slider limits and motors,
CCD, reciprocal cloth/rubber contacts, moving and dynamic attachments, stream
release/readmission, save/reopen, teardown, and provider replacement.

The sleep implementation also exposed and corrected real adapter defects: native
contact removal on sleep must not become a false semantic end; native body
replacement must preserve neutral contact identity; joint-pair suppression must not
invalidate unrelated sleeping contacts; geometry removal must wake supported bodies;
new geometry must wake newly overlapping compatible sleepers. The latter path uses
collision filters and releases read locks before waking only dormant dynamics.
Positive support/separation and incompatible-mask negative controls pass. Source
review additionally caught a soft-before-rigid destruction UAF, nested native locks,
derived nonfinite admission, and force/impulse/damping mistakes; corrected fixtures
and failures are retained.

The [feature matrix](Provider/FeatureMatrix.md) distinguishes these demonstrations
from production parity. Adoption limitations have concrete engineering meanings:

| Remaining capability | Current constraint / work required |
| --- | --- |
| General selective cadence | Dynamic contact/joint-connected membership, migration, crossings, and wake propagation require further Engine scheduling work and new physical gates. The measured authored partition is already implemented. |
| Capsule character semantics | The bounded cast/slide mover lacks initial penetration recovery, multiplane response, and equivalent floor/step behavior. Complete the existing neutral movement contract before replacement. |
| Dynamic soft attachments | Current helper permits one live attachment per endpoint; general multipoint/body attachment graphs require a coupled solve and new concurrency/energy qualification. |
| Rubber material parity | Jolt local cell diagonals/tetra volumes approximate XPBD global shape recovery. Calibrate materials and content separately; current rubber timings cannot establish an equivalent winner. |
| Soft/soft collision | Explicitly unavailable at the pinned Jolt collision collector/provider. Requires substantial collision/solver extension or an explicit feature limitation. |
| Self collision / swept soft contact | Unqualified and excluded from the demonstrated scope. Dedicated contact-path implementation/qualification or an explicit content limit is required. Rigid CCD does not establish soft CCD. |
| Mesh/attachment transitions | Static canonical meshes are admitted; dynamic MeshPart inherits the existing box fallback. Transitions with linked constraints reject until transactional constraint recreation exists. |
| Large-world/session coverage | Small imported floor, bounded articulation/contacts, and finite qualification horizons do not establish arbitrary large-world streaming, long sessions, or every material. |

## M5: fair A/B/C evidence

All cost cohorts use the same trusted Windows worker, bounded caps 1/2/8, MSVC
Release `/O2 /Ob2 /DNDEBUG`, common geometry, and fixed simulation durations. Separate
instrumented executables measure allocations; their times are never ranked.
Receipts preserve exact binaries/cache/source, raw stdout/stderr hashes, warmup,
process order, quality, and failed/unsupported rows.

### Rigid functionality available in all three backends

The stock e77 macro-off control and patched d8 run each complete 86 fresh processes.
This table gives single-worker U2/C8 screen step P50, **not awake-only solver cost**:

| Scene | A: Box3D | B: pristine Jolt | C: patched Jolt |
| --- | ---: | ---: | ---: |
| Stack | 0.0006 ms | 6.5882 ms | 0.1956 ms |
| Dense contacts | 0.0007 ms | 15.2870 ms | 0.4378 ms |
| Mostly static | 0.0421 ms | 0.2446 ms | 0.0205 ms |

Sleep eligibility/trajectory differ; near-zero Box3D medians are also limited by
timer resolution. Patched Jolt does not win every rigid workload. The Box3D motor
screen fails the unchanged limit gate: peak hinge 0.460294 rad exceeds 0.45 rad;
it is retained and excluded from timing ranking. Concurrent admission is unsupported
in both experiment adapters. This does not authorize modifying production physics.
Original receipt `PhysicalRowSha256` included nested query timing; the offline v2
correction strips those fields and has sensitivity regression tests. This is a
quality/configuration summary, not a full trajectory hash. Original receipts remain
immutable.

### Bounded quality-matched cloth reference

Production-default XPBD is much cheaper but does not reproduce the selected cloth
response: warm 16K no-collider final maximum strain is about 0.872 versus Jolt 0.017,
and center height 2.221 versus 2.967 m. A solver speedup cannot be inferred from
those unequal responses. Production XPBD also supplies one-way collider snapshots,
so it cannot be ranked as equivalent reciprocal coupling. Stock Jolt's original
reconstruction remains an S1 negative control, not a qualified coupled winner.

An isolated reference uses the actual XPBD implementation at High quality, 32
substeps, and calibrated substep damping retaining the authored frame damping.
The common 128×128, 8×8 m cloth, initial impulse, material targets, and 40-frame
duration are fixed. Following exploratory pilots, the confirmatory protocol was
saved **before fresh runs**, SHA-256
`5d9e2de5814d09a21c6b5100d4d15d2bbdce925bce8993fc533dc295d286f5b8`.
It is an experimental acceptance envelope, not a production material contract.

The independent checker rehashes raw data and reconstructs per-frame state hashes
and geometry. A separate independent rehash of the copied benchmark executable
matches the exact binary digest recorded by both fresh cohorts. All 40 trace frames
are finite with zero pin error. Peak
strain is 4.12%/4.13% under the 5% transient cap; final strain 1.69%/1.57% is under
2%. Peak cross-backend RMS is **3.91 mm** under 5 mm and maximum vertex displacement
**10.91 mm** under 15 mm. Timing runs gate final pin/strain and native per-substep
completeness; they do not record every-frame pin/strain. Shared initial layout is
source-backed; a raw time-zero trace was not emitted.

Three fresh counterbalanced untraced pairs per cap give medians of per-run mean
provider-call time:

| Total requested worker cap | Patched Jolt N80/Q2 | XPBD High32 reference | Reference / Jolt |
| --- | ---: | ---: | ---: |
| 1 | 185.698 ms | 268.476 ms | 1.446× |
| 8 | 70.998 ms | 268.843 ms | 3.787× |

The one-body XPBD workload uses roughly 0.12 of its eight-worker pool; equal caps
do not imply equal utilization. This demonstrates a competitive Jolt result at the
declared quality target over **0.667 seconds without contact**, not superiority over
an optimal future XPBD implementation or a full game frame. The comparator retains
`timing_winner=null` for references. Separate driven-contact tests establish actual
Jolt soft-contact callbacks and trajectory divergence from a no-collider control;
those instrumented contacts do not enter this timing table. Static-sphere cloth
rows with Jolt geometric contact band zero and no demonstrated native contact are
only collider-presence overhead evidence.

### Actual Engine cost, tails, and resources

Each authored Engine matrix has 90 fresh pairs. The candidate improves 22/30
matched Jolt groups and regresses 8/30, with ratios 0.865–1.114. No-collider 16K
cloth at cap 8 costs **70.71 ms in `WorldRoot::StepPhysics`**; cap 2 regresses from
114.12 to 127.12 ms. Rubber cap 8 costs 231.94 ms and is not material-equivalent to
XPBD. Provider and Engine cohort binaries differ, so subtracting their times would
not isolate integration overhead. Raw receipts retain frame samples/P95s and phase
fields; the reductions retain grouped cost, quality, lifecycle, and worker-use
summaries. They do not turn process samples into isolated publication timings.
These numbers do not establish a 60 Hz large-soft-body budget.

In the separate cold three-frame allocation pass, 16K cloth admission is 53.05 ms
Jolt / 2.61 ms XPBD, tracked admission live bytes 65.87 / 4.21 MiB, and tracked
process peak 68.20 / 7.03 MiB. Jolt includes a fixed 32 MiB temporary arena and world
reservations. Uninstrumented warm 16K cloth live working set is 21.44 / 15.70 MiB;
reserved/tracked bytes are not resident bytes. Post-retirement tracked net deltas
are 5,622 / 120 bytes, including harness/process globals. Hooks do not cover every
malloc/OS allocation, and working set is not proof of leak-free retirement. The
provider's Linux leak/sanitizer runs provide separate evidence. Full five-scene
phase allocation and P95 data are retained; quick rigid allocation runs failing
their short sleep gate are excluded from quality conclusions.

## M6: ownership economics and recommendation

The qualified d8 patch is **19 commits over e77, 27 changed files, +1,499/−8 lines**.
The Jolt tree contains 15 changed files/+487/−8, including a two-line build-list edit;
actual runtime C++/headers are 14 files/+485/−8. Five unit-test files add 798 lines.
The four functional units are reconstruction, retained-lambda convergence, bounded
attachments, and sleep timing/zero-impulse wake behavior. Defaults retain upstream
physics selection, but this is not binary ABI compatibility: soft vertices gain two
`Vec3` fields, opt-in convergence adds float multiplier arrays, and unversioned soft
creation serialization grows by five bytes. Engine semantic save formats are unchanged.

The controlled upgrade combines the earlier two numerical conflicts with one new
settings conflict when upstream removed `mDeterministicSimulation`. All 11 post-109
commits were replayed onto newer upstream; the rehearsal totals 29 files/+1,585/−9
and passes [all three native unit configurations](https://github.com/GantriaEngine/JoltPhysics/actions/runs/42481427201).
This measures one real upgrade, not recurring engineer-days. Newer upstream has not
inherited the old physical qualification, and the Engine pin remains d8/e77 ancestry.
The opt-in patches are independently reviewable/upstream-submission candidates;
upstream acceptance is not assumed.

Independent reviews covered numerical replay, graph scheduling/concurrency,
Engine authority/lifecycle, memory, benchmark fairness, and patch maintenance.
Demonstrated defects were corrected, including the final stock compile guard and
timing-contaminated summary hash. No unresolved S1 root-cause finding remains.

**Long-term choice:** for the intended future of reciprocal rigid/deformable gameplay,
build toward this maintained unified Jolt provider rather than first committing to
a second bespoke Box3D/XPBD reciprocal contact solver. One native world already
supplies shared contacts, rigid identity, reaction ownership, and scheduling. The
measured fork is small enough to be a credible ownership proposition today.
That advantage is demonstrated functionality and maintainable source, not a claim
that a fork automatically beats specialized XPBD or removes Engine adaptation work.

**Production choice now:** keep Box3D + XPBD. Adoption requires the character and
attachment contract work above, explicit decisions on unsupported collision families,
content/material calibration, and representative frame/memory budgets. Cosmetic or
loosely constrained soft content may continue to favor XPBD. Large-scale general
soft collision could substantially increase the fork's ownership cost. Those are
new adoption requirements; they do not reopen the completed S1 investigation.

## Evidence, reproduction, and cleanup

The retained artifact root is
`C:/Users/aiden/.codex/artifacts/jolt-optimized-foundation-20261108`.
Its final checkpoint/manifest index exact source, binary and archive hashes, failed
controls, worker receipts, and review files. Key reductions are:

- `N80SleepCrossPlatformAudit.md`: complete d8 native/unified capture and cross-OS parity.
- `ProviderIndependentLifecycleReview.md`: source-backed lifecycle/concurrency findings and fixes.
- `PerformanceMatrixReview.md`, `PerformanceMatrixReduction.json`, `EngineCostReduction.json`.
- `PrespecifiedClothReferenceProtocol.json`, `CheckPrespecifiedClothReference.py`, result/review files.
- `M6WorkerEvidence.md/json`, `M6ReduceEvidence.py`: stock/A/C, sleep, allocations, and hash caveats.
- `PatchMaintainabilityReview.md` and complete newer-upstream CI metadata.

Manual workflow profiles and local runner commands are documented in the
[provider](Provider/README.md), [benchmarks](Provider/BenchmarkREADME.md), and
[scheduling evidence](Optimization/SelectiveSchedulingEvidence.md). Native
compilation/qualification used the trusted worker or hosted CI, not sustained local
builds. Worker generation 36 is released only after owned work is idle and its
heartbeat stopped; the exact final receipt is `WorkerLease-release.json`. Reusable
task caches are retained. Failed builds, hypotheses, negative controls, and the
earlier sealed evidence remain available. No experimental merge or Engine release
is part of this campaign.
