---
status: experimental-qualified-profile
authority: non-normative
date: 2026-11-08
---

# S1 resolution and bounded unified Jolt evaluation

The remaining tested instability mechanisms are resolved at the source and
fixture level. A corrected experimental Jolt profile passes independent Linux
and Windows qualification, including every internal soft integration, long
free motion, faster impacts, gravity, repeated contact, worker budgets and
negative controls. **Retain production Box3D + XPBD.** The measured cost and
production adapter gaps do not justify switching factories now. Unified Jolt
is a demonstrated reciprocal-coupling option, rather than a solver rejected
because S1 remains unexplained.

Production physics and main were not changed. PR19 was not merged or advanced.
All implementation changes are isolated experiment/build tooling, including a
generated upstream numerical patch. This report supersedes the deferred
qualification conclusion in the earlier investigation checkpoints.

## Qualified experimental configuration

| Property | Qualified setting |
| --- | --- |
| Upstream | Jolt v5.6.0, `e77f175595e64cb44218cc9d9d56fc365ad0e36a` |
| Frame | Authored float `1/60` second |
| World updates per frame | 2 |
| Collision steps per update | 8 |
| Soft iterations per collision step | 12 temporal integrations, not 12 convergence sweeps |
| Internal soft integrations per frame | 192; approximately 86.806 microseconds each |
| Rigid advances per frame | 16; approximately 1.042 milliseconds each |
| Stretch / distance-bend compliance | `1e-6` / `1e-4` |
| Soft damping rate | `2.4` per second |
| Velocity reconstruction | `delta`: damped velocity plus accumulated intended constraint displacement divided by the internal timestep |
| Constraint scope | Positive-compliance edges and tetrahedral volumes in the tested fixtures |
| Admission | Dihedral, skin, rod and LRA position constraints reject alternative reconstruction before stepping |

The cloth has 169 vertices, 13 fixed top-row pins, 156 movable vertices of mass
0.1, and 886 distance constraints: 600 structural/diagonal and 286 bend edges.
Native contact uses dynamic spheres of radius 0.45 and mass approximately
0.26719245. The qualification is finite and fixture-specific; it is not a
claim that all Jolt materials, shapes, constraints or timesteps are qualified.

## Failure mechanisms and discriminating evidence

### Exact-hard fixture and projection work

**Confirmed root mechanism for the tested hard free-cloth failure:** the
center-only normal-velocity seed is incompatible with the exact distance
model at its flat rest state, and ordered edge projection followed by velocity
reconstruction performs positive kinetic work while attempting to resolve it.

The hard graph's first-order condition `J v = 0` is insufficient. Its
second-order equation `J a = -b` has relative residual `0.8395135`; the recorded
left-null witness independently demonstrates incompatibility. The local
four-vertex complete distance graph has a nonzero signed-volume derivative
under the seed despite its six fixed distances. Native Jolt reproduces the
0.05-to-1227.84 kinetic-energy excursion without contact. Double and long-double
replays, including recomputed rest lengths, retain the hard-network failure.

Per-edge accounting closes the correction work identity
`DeltaK = sum(m v dot DeltaV + 0.5 m |DeltaV|^2)`. The measured stretched edge
402 reinforces already-closing motion and adds about 0.00758 kinetic energy;
the source of gain is not limited to compressed diagonals. Deformed-state
linearization also finds local expansion, without pretending that one local
eigenvalue proves the entire long trajectory.

An admissible whole-cloth rotation about the pinned row remains bounded on the
same hard graph at both original schedules, including exposed internal states.
Thus hard edges are not universally unstable. The qualified elastic fixture
uses the intended finite compliance. Deleting diagonals or changing pinning is
not presented as a material-preserving correction.

### Finite-compliance contact loading

**Confirmed root operation:** contact-position projection inserts edge-model
energy at the frozen rigid pose. Subsequent edge projection and reconstruction
convert some of that loading into kinetic energy. The measured dynamic
particle/rigid impulse pairs dissipate kinetic energy.

At compliance `1e-6`, C4/I48 and C8/I24 have the same 192 soft integrations.
All 28,800 free-cloth internal physical states match exactly over 150 frames.
Their coupled trajectories diverge at contact, isolating the coupling path.
The operation trace matches pristine dynamics and closes kinetic accounting
below `7.4e-8` and edge energy at saved precision.

| First accepted excursion | Coarse C4/I48 | C8/I24 comparison |
| --- | ---: | ---: |
| C2, frame 18, zero-based ordinal 0: total projection edge-energy insertion | 10.521895 | 1.358986 |
| C2 vertex 100 projection depth | 16.217 mm | 5.896 mm |
| C2 paired contact kinetic change | -0.910540 | -1.805240 |
| C2 accepted `K+U` | 15.376058, above 7.013812 ceiling | 6.233557, below ceiling |
| C1, frame 18, zero-based ordinal 48: projection insertion | 2.708541 | 0.708293 |
| C1 accepted `K+U` | 4.271205, above 3.506911 ceiling | 2.925065, below ceiling |

Jolt reconstructs velocity before this positional contact correction, and the
source explicitly leaves current velocity unaffected by that projection.
Rigid pose, collision collection, soft-context reset and cached velocity
writeback all change with outer cadence. The evidence attributes the immediate
energy-loading operation; it does not uniquely blame plane refresh or rigid
writeback in isolation. Removing positional correction is not a fix because
it abandons the geometry requirement.

The corrected numerical solver still needs sufficient coupling resolution in
the tested faster-impact case. Two bounded 12-frame checks with the same
corrected reconstruction and 192 soft integrations show C2 speed 10 failing
at W1/C8/I24 (`29.967985 > 28.055218`, frame 9) while W2/C8/I12 passes. These
checks used the already-qualified Windows binary, one worker, hidden processes
and a three-second limit; actual process durations were 0.188 and 0.156 seconds.
Sixteen rigid advances are the selected qualified cadence, not a proved global
minimum or a claim that no cheaper configuration can exist.

### Long-horizon float feedback

**Confirmed numerical root class:** quantized position updates feed net
displacement back into velocity through `(Pfinal - Pprevious) / h`. Small float
differences can be amplified by the ordered compliant network over time.

The previously promising unmodified C8/I24 profile fails free motion at frame
1374, internal ordinal 6, reaching maximum `K+U = 0.067866376` against 0.05251.
Halving the soft timestep makes it worse: maximum 0.523782787, first failure
frame 326. No contact, pending rigid impulse, pin drift or nonfinite state is
involved. More retained-lambda sweeps do not reliably fix float dynamics.

The float scalar replay matches native edge energy at every one of 1,800 frame
endpoints, with kinetic difference below `9.8e-10`. Double and long-double
replays pass the corresponding endpoint checks. Keeping positions and
constraints in float while changing only velocity reconstruction also removes
the failure in both scalar and native tests.

The local source equation demonstrates an amplification channel directly:
at `x=2f`, post-damping velocity `0.003f` and `h=1/(60*384)`, intended movement
`1.30208e-7` rounds to one position ULP, `2.38419e-7`; reconstructed velocity
becomes `0.005493164`, about 3.35 times the intended kinetic energy. Such
coordinates occur at movable native cloth vertices. Nearby-float subtraction
can itself be exact under Sterbenz; the issue is quantized position updates
being interpreted as velocity, not a claim that subtraction rounding alone
causes the error.

An applied-displacement reconstruction retains projection-assignment rounding
and passes. A further quantized-base control passes N192 but fails N384,
showing that predictor quantization and summation grouping interact with the
long nonlinear trajectory. No single float instruction is claimed uniquely
responsible across schedules. The source-compatible incremental update is a
demonstrated bounded correction to that feedback class.

## Isolated numerical correction

[MakeDeltaVelocityInstrumentation.py](MakeDeltaVelocityInstrumentation.py)
generates the reviewable [two-file patch](JoltDeltaVelocityExperiment.patch).
Its SHA-256 is
`7602f2956aa4941936fef8a409a7ee76841bc599a07d8d6eea47b1db6cd05f83`.

Each vertex captures damped velocity at integration start and accumulates
edge/volume displacements before reconstruction. Mode 1 accumulates intended
corrections; mode 2 accumulates actually assigned float corrections. Mode 0
keeps the original expression. Contact-position projection and recentering
remain outside current velocity reconstruction. Accumulators reset each
integration and use vertex-owned storage rather than a global lifetime map.

The experiment rejects unsupported position writers before stepping. This
patch and its callback/admission code are an experimental implementation,
not a drop-in production or general upstream patch. Float-scale stored-position
kinematic residuals remain explicitly reported; the method is equivalent in
real arithmetic, rather than a claim of exact float conservation.

## Independent qualification

| Gate, per platform | Result |
| --- | --- |
| Original-mode 1,800-frame controls at N192 and N384 | Patched observed and original unmodified binaries match all frame/world-update states; pure patched matches all frames; both old failures reproduce exactly |
| Native positive cases | 24/24 pass every internal kinetic/model/geometry/finite gate |
| Native repeated processes | C1/C2: workers 1/2/8, three repeats, 600 frames; one physical hash per fixture |
| Native stress | Free/C2 1,800 frames; C2 speed 10, friction 0 and restitution 0.5 pass |
| Native negative controls | Original S1 and coarse free U96 reject, using the original reconstruction |
| Native spring meters | Both material calibrations match exact float32 source P/V/K/U at all 1,536 states; zero maximum replay error |
| Unified positive cases | Five fixtures, workers 1/2/8, three repeats: 45 paired runs; three paired 600-frame extensions also pass |
| Unified negative controls | Hard-reference precondition and missing-internal-observation controls reject |
| Observation independence | Separate pure corrected and observed corrected libraries have exact physical frame parity |
| Platform parity | All 26 native and 48 positive unified frame hashes match Linux/Windows; quality objects and callback coverage match |

The unified fixtures are RigidIntoCloth, Multiple, RubberPush, SphereOnCloth and
Repeated. All internal callbacks are counted and ordered. The meter includes
rigid angular kinetic energy and cached rigid kinetic changes exactly once
before writeback. It uses positive-compliance edge/volume reference energy,
recorded gravity potential and signed restart work. The unchanged closed-case
kinetic and reference ceilings retain the 5% plus `1e-5` allowance. Passing is
not a complete Jolt solver-internal conservation proof. Global momentum
conservation is not claimed with damping and fixed supports.

The independent review is preserved at
`C:/Users/aiden/.codex/artifacts/jolt-solver-resolution-20261108/IndependentFinalQualificationReview.md`.
Large raw streams are lossless gzip with verified original/archive SHA-256,
retained inside the platform artifact ZIPs. Failed profiles, counterfactuals
and unsuccessful harness attempts remain available.

## Quality cost and integration limits

Corrected standalone unified T1 timings below are medians of the three fresh
process **mean World.Step times**. The pure library has no energy-observer
hook. These are the fixture's native world costs, not complete Engine frames.

| Fixture | Linux ms/frame | Windows ms/frame |
| --- | ---: | ---: |
| RigidIntoCloth | 2.8214 | 2.1783 |
| Multiple | 2.8243 | 2.1825 |
| RubberPush | 1.2186 | 1.2116 |
| SphereOnCloth | 4.6324 | 3.6501 |
| Repeated | 2.8198 | 2.2236 |

The separate frozen Engine rigid comparison completes 86 fresh processes per
platform, including all 16 cases at the candidate cadence and repeated
same-host baseline/candidate costs. All candidate quality dispositions agree
across platforms: Jolt 15 pass plus expected concurrent-admission unsupported;
Box3D 14 pass, inherited strict joint-motor angle failure, and expected
concurrent-admission unsupported. Query screens without step samples remain
valid screens and are excluded from timing comparisons. This does not certify
bitwise rigid trajectories across platforms.

| Jolt rigid scene | Linux P50 baseline -> candidate, ms | Windows P50 baseline -> candidate, ms |
| --- | ---: | ---: |
| Stack | 1.9094 -> 7.4504 | 1.6011 -> 6.0639 |
| Dense contacts | 5.2179 -> 17.2504 | 4.7296 -> 14.8109 |
| Mostly static | 0.0689 -> 0.2733 | 0.0607 -> 0.2348 |

The tested global cadence costs about 3.1–4.0 times the same Jolt adapter's
U1/C4 baseline in these scenes. Tail costs also rise. Box3D also pays for the
cadence; its near-zero dense-contact candidate P50 is not a speedup claim:
P95 and mean step duration increase, with a distribution consistent with a
changed active/sleeping phase. T2 helps some Jolt scenes; T8 is not consistently
beneficial. The data is specific to the recorded hosted hardware and scene
definitions. It is not an optimization bound or a universal worker policy.

The earlier corrected XPBD and Jolt comparison has bounded passes at matched
declared schedules, but materially different rebound trajectories and timing
boundaries. Contact callback counts and projection counts also have different
units. It supplies no physics-equivalent coupling speed ranking. The rigid
Jolt adapter additionally omits production contact-event publication, so even
its admitted timing is not a feature-complete production-provider cost.

The separate corrected allocation pass has exact allocation/pristine frame
parity for all five fixtures on both platforms. It records roughly 95,600–115,800
calls and 76.8–77.9 MiB cumulatively requested per 150-frame case, including
both contact/control worlds and harness JSON. Live bytes at capture are not
post-teardown retention; peaks are process-lifetime. It is not a leak test,
solver-only census, or backend memory ranking. Imported library bytes and
patch identities were independently matched to preserved qualified builds.

## Architecture recommendation

**Retain production Box3D + XPBD.** Apply any selected improvement through its
own bounded production change and qualification. The reason is measured cost
and integration scope, while the tested Jolt instability now has a qualified
experimental correction.

| Option | Assessment |
| --- | --- |
| Current Box3D + XPBD | Preserve the existing production baseline and ownership/lifecycle contracts. Current one-way soft colliders are not equivalent to reciprocal coupling. |
| Box3D + improved XPBD | Lower-disruption option for selected soft-solver improvements. The repaired private diagnostic does not establish a general production rigid-reaction contract. |
| Box3D + Jolt-soft mirror | Not recommended as a shortcut: it needs collider identity, pose/mass/velocity transfer, reaction ownership, synchronization and duplicate-world cost accounting. |
| Unified Jolt | Technically credible experimental reciprocal solver. A production migration is not justified by this bounded evaluation: the selected cadence is costly, the numerical patch has restricted constraint coverage, and Engine provider work remains. |

The production soft collider contract currently carries shape/transform data,
without a rigid identity, mass, velocity or reaction channel. The experimental
rigid Jolt adapter lacks contact events needed by Touched/TouchEnded and its
kinematic capsule movement is unsupported. Shared-world ownership, authored
soft asset conversion, generation-safe body/constraint mapping, character
motion, queries, publication order, platform behavior and rollback remain
Engine-owned productization work. These are explicit migration boundaries,
not a request to investigate S1 later.

No production authority, public API, schema, persistence, wire format, factory
selection or default physics behavior changed. No production ADR was amended.
The experimental default-off build boundary and earlier qualified mainline
checkpoint remain intact.

## Reproducible source and evidence

| Execution | Immutable source | Acceptance fact |
| --- | --- | --- |
| [42446110873](https://github.com/gmoddev/gargantuan/actions/runs/42446110873) | `8459b076de8b25b0b46a5c01ed1e3a8021fb6763` | Operation-level contact ledger, finer-step rejection, long precision/convergence controls |
| [42447487882](https://github.com/gmoddev/gargantuan/actions/runs/42447487882) | `7c9e9d25371d382edeb4951d90b6a63f9cc413e3` | Native/replay reconstruction ablations and exact mode-specific meters |
| [42448487967](https://github.com/gmoddev/gargantuan/actions/runs/42448487967) | `187325921ddb93672217aac99496b05ea8f5dfa0` | Independent Linux/Windows corrected qualification and long original-mode identity |
| [42450431450](https://github.com/gmoddev/gargantuan/actions/runs/42450431450) | `d5f0eb27b06817c2e973faa44e6dc1e94cfcc7da` | 86-run candidate rigid cadence/cost matrices using frozen e5 Engine executables |
| [42450606543](https://github.com/gmoddev/gargantuan/actions/runs/42450606543) | `967d049e13d171baddb74c39fbb76da5859844c4` | Corrected-profile separate allocation census |

Linux qualification reuses the exact 7c9e numerical binaries; Windows builds
the same numerical implementation independently at 1873. The native runtime
implementation and patch generator are unchanged between these checkpoints;
scalar controls and qualification tooling differ. Rigid cost uses the
frozen `e5ab742eee6775f0bb5213ad11288ec0d8489103` executables; the soft-only
numerical patch does not change their rigid algorithm. SDL alone was rebuilt
from `f87239e71e42da91ca317a12eefb82cfbf3393eb`, then reused after the query-screen
harness correction. No unaffected full 1,520-process matrix was repeated.

The branch is `experiment/jolt-solver-resolution-20261108`, derived from the
preserved e9 checkpoint. Canonical evidence, independent reviews, compressed
raw data, source/build metadata and continuation bundle are under
`C:/Users/aiden/.codex/artifacts/jolt-solver-resolution-20261108`.
The previous sealed S1 artifact directory remains separate and unchanged.

To repeat the corrected qualification on this experimental branch:

```powershell
gh workflow run 410396641 --repo gmoddev/gargantuan `
  --ref experiment/jolt-solver-resolution-20261108 `
  -f schedule_followup_only=true -f unified_qualification_only=true `
  -f schedule_source_run=42447487882
```

For a source rebuild, generate the numerical patch from a clean pinned Jolt
checkout, apply it in a separate worktree, and configure `DiagnosticJolt` with
`DIAGNOSTIC_DELTA_VELOCITY=ON`. Build pure and energy-observed libraries
separately; only the observed variant receives `MakeEnergyObservation.py`'s
patch. The recorded CMake caches and compile commands preserve exact options.
Fresh builds and sustained qualification belong on the coordinated worker or
hosted runners with two build jobs, not the controlling PC.

No shared-worker lease was acquired or released, and no shared-worker native
process was started by this campaign. Hosted jobs are complete. Local native
work was limited to the preserved sub-second hard-motion/compliance probes
and the two twelve-frame cadence checks. All exited and were reaped; no
sustained local build or native qualification was run. Experimental source
and evidence are preserved without merging into production.
