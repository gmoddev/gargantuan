# Animation Foundation 2C: update policy and measured pose jobs

Status: current architecture as of Animation Foundation 2C.

This milestone separates logical animation time from expensive pose production. A
bounded, renderer-neutral policy now decides which active rigs need a new pose;
the remaining independent rig solves use the existing `JobSystem` when the
measured batch is large enough. Bones remain canonical asset records rather than
DataModel Instances, pose state remains transient, and one accepted pose revision
is shared by the main view, shadow pass, semantic consumers, and any future portal
views.

The runtime flow is:

```text
Main: advance tracks and detect events
  -> classify FullRate / ReducedRate / VisualFrozen / SemanticRequired
  -> snapshot bounded immutable rig inputs in Animator ObjectId order
  -> workers: sample, blend, solve hierarchy, and build the skin palette
  -> Main: reject stale results and merge in Animator ObjectId order
  -> semantic joint cache and animated anchors
  -> immutable RenderPublication
```

There is no new Luau or authored gameplay API. `Animator.LOD`, authored update
rates, and an `AlwaysAnimate` override remain intentionally absent.

## Measured pre-policy cost

`gargantuan_animation_foundation_benchmark --profile-matrix` runs the complete
1/10/100/500/1000-rig, 16/64/128/256-bone, and 1/2/4-track matrix. The table below
selects the representative one-track, 64-bone rows from the complete 7950X3D
Release run. This is the single-worker path so it remains comparable to the 2A
baseline.

| Rigs | Runtime ms/frame | Total ms/frame | Sample/blend | Hierarchy | Skin matrices | Publisher | Projection |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.0079 | 0.0119 | 0.0019 | 0.0031 | 0.0022 | 0.0022 | 0.0014 |
| 10 | 0.0782 | 0.1044 | 0.0178 | 0.0317 | 0.0217 | 0.0173 | 0.0086 |
| 100 | 0.8030 | 1.0575 | 0.1798 | 0.3241 | 0.2203 | 0.1680 | 0.0861 |
| 500 | 4.4223 | 6.0760 | 0.9460 | 1.6488 | 1.1695 | 1.0876 | 0.5646 |
| 1000 | 10.4344 | 15.0171 | 2.2171 | 3.4883 | 2.5144 | 2.8285 | 1.7512 |

Ten one-track rigs measured 0.0250/0.0782/0.1505/0.2902 ms of runtime work at
16/64/128/256 bones. Ten 64-bone rigs measured 0.0782/0.0856/0.0984 ms with
1/2/4 tracks. The hierarchy is the largest lean substage, but it is not an
isolated dominant cost: sampling/blending and skin-matrix generation together are
larger.

Fine-grained clocking is opt-in with `--detailed-profile`, because timing every
key lookup and interpolation materially perturbs small operations. At 500
one-track, 64-bone rigs it reported 9.5657 ms runtime, including 0.5658 ms lookup,
0.5186 ms interpolation, 3.2483 ms residual track blending, 1.5430 ms bind-pose
fallback, 1.6467 ms hierarchy, and 1.1933 ms skin matrices. These values establish
attribution, not lean frame cost. Track advance was 0.0248 ms, semantic joint-cache
commit 0.0754 ms, and palette publication 0.0139 ms. CPU skinning remained zero on
the normal GPU-palette path.

## Policy inputs and classes

`AnimationUpdateContext` is a bounded native scheduling input. It contains the
current runtime environment, one current view origin, a generation/publication
tagged visibility aggregate, and the complete sorted set of rigs required by
semantic anchors. A future multi-view or portal system can union accepted view
demands into the same aggregate; it must not request a separate animation sample
per view.

| Class | Selection | Pose cadence |
| --- | --- | --- |
| `FullRate` | Graphically visible in the near band, or graphical feedback is missing/malformed/stale | Every logical change |
| `ReducedRate` | Graphically visible in the mid, far, or very-far band | 30, 15, or 10 Hz respectively |
| `VisualFrozen` | Visual-only and outside the recently-visible grace, or visual-only in a headless runtime | No pose solve; the last graphical palette is held |
| `SemanticRequired` | The rig owns at least one indexed joint-bound semantic Attachment, or the semantic summary is incomplete | Every logical change, independent of visibility |

The active view camera world position is the current distance source. Distance is
never derived from framebuffer coordinates. Missing graphical feedback fails
conservatively to `FullRate`; malformed or over-capacity semantic feedback fails
conservatively to `SemanticRequired` for all active rigs. Visibility is internal
scheduling advice and is not exposed to Luau as authoritative state.

### Distance and transition policy

The defaults are deliberately conservative and remain internal:

| Band | Exit distance | Re-enter distance | Visible cadence |
| --- | ---: | ---: | ---: |
| Near | 64 | 56 | runtime cadence |
| Mid | 160 | 144 | 30 Hz |
| Far | 320 | 288 | 15 Hz |
| Very far | beyond 320 | below 288 | 10 Hz |

The distinct enter/exit thresholds prevent threshold oscillation. A visible rig
keeps its current visible class for 0.25 seconds after it leaves the accepted view;
after that bounded grace it becomes `VisualFrozen` unless semantics override it.
Policy transitions and cadence slots use stable ObjectId-derived ordering. Reduced
updates are phase-distributed from the ObjectId hash rather than concentrating all
far rigs on the same frame. Cadence is based on elapsed nanoseconds, not render
frame count, and is covered at 30, 60, 144, and variable frame delivery.

Foundation 2C does not add a global animation CPU budget. The active Animator map
is already bounded to 4096 and is iterated in ObjectId order. Semantic work is
never deferred behind visual work, and reduced visual work has no queued debt or
catch-up list.

## Track time, events, and explicit controls

Track advancement is always a lightweight Main-thread phase. Playing tracks
advance `TimePosition`, loop, and detect natural completion even when pose work is
reduced or frozen. `Ended` remains exactly once and is emitted on Main after the
deterministic pose merge; worker completion order cannot call Luau. A looped track
therefore retains phase offscreen, and a non-looping frozen track reaches and holds
its final logical time. Re-entry samples one pose at the current time and never
replays skipped frames.

The renderer holds the previous palette between reduced samples. A frozen rig
publishes no unchanged palette. These explicit control changes mark a fresh pose:

- play, pause, resume, stop;
- seek (`TimePosition`);
- speed, weight, and loop-mode changes;
- track-set or compatible asset changes;
- rig/source-content or skeleton compatibility changes;
- an explicit native `RequestPoseRefresh`;
- escalation from a lower-demand policy class to a higher-demand class.

Graphical control changes perform one immediate refresh even while frozen and then
return to policy cadence. Stop removes the now-unused pose, and a paused unchanged
mix performs no repeated solve or publication. A headless visual-only rig still
skips GPU-oriented pose work after ordinary controls because no native consumer
exists; a semantic requirement or explicit native refresh evaluates it.

Owner world-transform changes do not resample clips or rebuild the hierarchy. The
render object transform moves independently, while `SemanticSpatialResolver`
recomposes registered animated Attachment world transforms from the cached joint
model pose.

## Semantic override and headless behavior

`SemanticSpatialResolver::PrepareAnimationRequirements` incrementally refreshes
only dirty Attachment topology, then publishes a complete sorted rig set before
animation. It does not rescan the DataModel. A joint-bound Attachment is treated
conservatively as semantic because it may host a positional `Sound`, a
`ProximityPrompt`, or a future native/gameplay consumer. Removing its `JointPath`
removes the requirement on the next preparation phase.

Semantic-required rigs evaluate the full skeleton at full logical cadence in 2C.
This preserves Sound position, ProximityPrompt range/LOS/hold validation, and
headless gameplay correctness. Selective semantic-joint plus ancestor solving was
measured as unnecessary complexity for this milestone and remains a possible
future crowd optimization.

A headless runtime has no visibility or view distance. It advances every track,
fully evaluates semantic rigs, and performs no pose/palette work for visual-only
rigs. No GPU-oriented skin matrices are generated for those cosmetic rigs. The
same native explicit refresh used by tests can request one pose when required.

## Renderer-neutral visibility feedback

`RenderProjection` builds the smallest backend-neutral previous-publication
summary: a sorted bounded set of posed rig ObjectIds intersecting the accepted clip
volume. `SDLRenderer` owns the set and a monotonically unique renderer generation;
`Engine` reads it on the following runtime step. The Animator never queries a
renderer, and no gameplay service receives raw renderer state.

The test is conservative. For GPU-paletted poses it transforms all eight source
bounds corners by every accepted palette entry before clip rejection, so a mesh
deformed back into view cannot be incorrectly frozen from bind-pose bounds. A
missing mesh or non-finite projection is treated as visible. Overflow marks the
summary incomplete and forces conservative policy. Renderer destruction clears
the summary; an old generation/publication is rejected. A new renderer generation
can begin at a lower publication number safely.

One visibility aggregate may include demand from multiple accepted views in the
future. Shadow or portal view count must never become animation evaluation count.

## Why pose jobification landed

Policy removes ordinary visual-only work, but scenario E intentionally leaves 500
offscreen semantic rigs at full rate. Before jobs, 500 one-track, 64-bone rigs
still cost 4.4223 ms of lean runtime work on the 7950X3D. This is above the point at
which the existing worker pool provides a demonstrated benefit, so jobification
was retained after policy rather than assumed before it.

The selected granularity is a bounded contiguous batch of independent rigs. Fewer
than 32 pending rig evaluations remain inline. At 32 or more, at most one batch per
available worker and never more than 64 batches is submitted. A step snapshots at
most 4096 rigs, waits synchronously, and merges before semantic resolution or
render publication, so the design has no multi-frame backlog and no seconds-old
animation debt. The `JobSystem` queue has 64 preallocated ring slots and expands
only for generic callers that exceed that capacity; Animation 2C never exceeds it.

Worker input consists only of immutable clip/skeleton/mesh revisions, shared
immutable key arrays, copied scalar track sample state, and exclusive runtime-owned
output buffers. Workers sample, blend, solve the local hierarchy, build position
and normal palettes, and optionally run the pre-existing CPU fallback. They do not
read mutable Instances or AssetService catalogs, publish renderer handles, mutate
semantic services, or invoke Luau.

Main validates all of the following before apply:

- Animator ObjectId is still live and maps to the same tracked record;
- target MeshPart ObjectId is unchanged;
- active track creation sequence, logical revision, and control revision match;
- source mesh identity and content revision match;
- skeleton compatibility identity matches.

Any mismatch increments `StalePoseJobDrops` and leaves the last committed pose
untouched. Destroyed Animators retire that pose on the next authoritative scan.
Shutdown drains and joins the pool, clears pending results, and invalidates tracks.
Tests deterministically inject a track change, Animator destruction, incompatible
source-group reimport, and shutdown between worker completion and merge. Ten cycles
of 500 active Animators also prove all jobs and pose state retire before the next
Play world.

### 7950X3D worker scaling

Release, 64 bones, one track, 60 frames, identical full-rate work:

| Rigs | Serial runtime ms | Normal-pool runtime ms | Runtime speedup | Submit ms | Wait ms | Merge ms | Summed evaluator CPU ms | Batches/frame |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 0.0800 | 0.0801 | 1.00x | 0 | 0 | 0.0025 | 0.0738 | 0 (inline) |
| 100 | 0.8177 | 0.2508 | 3.26x | 0.0622 | 0.1001 | 0.0369 | 1.6883 | 32 |
| 500 | 4.4217 | 1.0500 | 4.21x | 0.0772 | 0.4312 | 0.2096 | 9.6902 | 32 |
| 1000 | 9.7585 | 2.9129 | 3.35x | 0.1122 | 0.8831 | 0.7391 | 20.4501 | 32 |

`runtime` is wall time observed by Main and includes synchronous wait. `Summed
evaluator CPU` adds each rig evaluator duration and therefore can exceed wall time
when jobs overlap. End-to-end serial/parallel wall time was 0.1067/0.1070,
1.0750/0.5168, 6.1247/2.5877, and 14.0271/7.2996 ms respectively. Stable runs had
zero stale drops and zero steady-state buffer or general allocations.

The 10-rig threshold result is intentionally neutral: constructing or waking a
pool for that batch did not earn its overhead. The current default uses the
existing `JobSystem` normal count (32 logical workers on the primary machine), not
a 7950X3D-specific authored quality setting.

### 500-rig policy scenarios on 7950X3D

Release, 64 bones, one track, 240 measured frames after grace/warmup:

| Scenario | Class average | Poses/frame | Runtime p50/p95/p99 us | Total p50/p95/p99 us | Anchors/frame |
| --- | --- | ---: | --- | --- | ---: |
| A: all near/visible | 500 full | 500.0 | 1091.7 / 1317.0 / 1415.2 | 2703.8 / 3057.0 / 3288.6 | 0 |
| B: 100 near + 400 far visible | 100 full, 400 reduced | 200.0 | 550.2 / 671.4 / 736.1 | 1121.3 / 1270.5 / 1328.9 | 0 |
| C: 50 visible + 450 offscreen | 50 full, 450 frozen | 50.0 | 290.0 / 322.8 / 372.6 | 424.7 / 463.8 / 512.7 | 0 |
| D: all offscreen visual-only | 500 frozen | 0.0 | 134.5 / 137.3 / 146.4 | 136.0 / 138.9 / 148.6 | 0 |
| E: all offscreen semantic | 500 semantic | 500.0 | 1352.4 / 1781.2 / 1940.8 | 3887.6 / 4628.2 / 5220.2 | 500 |
| F: 50 semantic + 450 far visible | 50 semantic, 450 reduced | 162.5 | 732.3 / 1007.2 / 1139.3 | 1453.7 / 1879.3 / 2321.2 | 50 |

Far-visible rigs in B average 15 Hz at a 60 Hz input (100 evaluations/frame), and
the far-visible portion of F contributes 112.5 evaluations/frame. Every evaluated
pose publishes once; frozen poses publish zero times. Track advancement remains
about 0.01-0.02 ms for all 500 logical tracks even when scenario D performs no
pose work. All A-F scenarios retained zero animation-buffer growth and zero general
runtime allocations after warmup.

## Static-world, memory, and diagnostics

The 50K regression contains 50,000 static Parts plus 500 active 64-bone rigs: 50
near/visible, 449 offscreen visual-only, and one offscreen semantic Sound/Prompt
rig. One measured step produced 51 pose and animated-object updates, one semantic
anchor resolution, zero static-object updates, zero ChangeJournal records, zero
full resyncs, zero CPU dynamic-vertex updates, and zero steady-state animation or
semantic allocations.

Per-Animator policy state is approximately 40 bytes of scalar class, band, grace,
and cadence data. Pending job snapshots use a fixed 16-track array and retain only
their bounded high-water capacity. Per-bone local poses, model transforms, and
palettes reuse the existing per-Animator pools; jobs do not create a second set of
256-bone output buffers. At the supported 4096-Animator limit there are at most
4096 pending rig records and 64 queued worker batches.

`AnimationRuntimeMetrics` exposes current class counts, pose evaluations, skipped
evaluations, immediate refreshes, policy transitions, visibility drops, headless
skips, active jobs, rig jobs and batches, stale drops, evaluator CPU, submission,
wait, merge, and existing stage/allocation counters. Renderer metrics separately
record visibility-summary CPU. No per-Animator/frame logging or external telemetry
was added.

## Quality, platform implications, and deferrals

Deterministic tests establish full-rate near behavior, 30/15/10 Hz elapsed-time
cadence, threshold hysteresis, visibility grace, loop phase continuity, current-pose
re-entry, exactly-once `Ended`, immediate controls, no paused/no-track work, and
semantic Sound/Prompt correctness. Those state and pose assertions are the release
gate; subjective visual inspection is supplementary.

The policy is directly useful to future phones because it can reduce pose count
before asking weaker cores to parallelize. No ARM/mobile performance or power
claim is made without hardware. Mobile work still needs real-device cadence,
thermal, worker-count, and visual-quality measurements before changing these
conservative defaults.

Root motion, IK, retargeting, state machines, blend trees, authored LOD controls,
timeline editing, selective-joint solving, crowd instancing, replication, cloth,
effects, portals, and mobile quality UI remain deferred. Foundation 3 should
prioritize: (1) authoritative root-motion contract and replication boundaries,
(2) measured selective-joint/ancestor solving for large semantic crowds, and (3)
animation graph/blend-tree design that preserves immutable clips and the current
bounded worker contract.

## Recorded decisions

1. The exact classes are `FullRate`, `ReducedRate`, `VisualFrozen`, and
   `SemanticRequired`.
2. Selection uses environment, complete semantic rig membership, renderer-neutral
   visibility, active camera distance, recent visibility, track/control state, and
   prior class/band state.
3. Projection supplies a bounded previous-publication ObjectId aggregate;
   animation never calls into renderer internals.
4. Distance is active-camera world distance with 64/160/320 exits and 56/144/288
   re-entry thresholds.
5. Distance hysteresis is paired with a bounded 0.25-second visibility grace.
6. Logical track time continues while visual pose is frozen.
7. Loop/end detection occurs during Main track advance; `Ended` fires exactly once
   on Main after merge.
8. Indexed joint-bound Attachments conservatively cover Sound, ProximityPrompt,
   and future semantic socket consumers.
9. Semantic-required evaluation is full skeleton and full logical cadence in 2C.
10. Headless skips visual-only solving but never skips a semantic rig.
11. Track controls, rig/content changes, policy escalation, first publication, and
    explicit native refresh force one immediate current pose where a pose consumer
    exists.
12. Jobification remained necessary because 500 full/semantic 64-bone rigs still
    cost 4.4223 ms serial runtime work after policy classification.
13. Jobs are bounded multi-rig batches over immutable inputs with exclusive output
    buffers; Main advances/callbacks and merges.
14. Animator/target identity, track and control revisions, mesh content revision,
    and skeleton compatibility reject stale results before apply.
15. After 2C, full semantic crowds and downstream semantic anchor/publication work
    remain the measured bottleneck; ordinary invisible visual crowds no longer do.
