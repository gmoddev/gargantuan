---
status: current
owner: runtime
last_verified: 2026-09-01
related_code:
  - include/gargantuan/animation/RootMotion.hpp
  - include/gargantuan/animation/AnimationRuntime.hpp
  - include/gargantuan/animation/AnimationTrack.hpp
  - src/animation/AnimationRuntime.cpp
  - src/animation/AnimationTrack.cpp
  - src/Engine.cpp
  - tests/AnimationFoundationTests.cpp
  - tests/AnimationFoundationBenchmark.cpp
related_adrs: []
---

# Animation Foundation 3A: Character root motion

## Contract

`AnimationTrack.RootMotionEnabled` designates one retained track per Animator
as the root-motion source. Enabling another source atomically disables the old
one and resets both extraction baselines. The source clip must animate the one
canonical topology-derived skeleton root. The implementation never assumes
joint array index zero and rejects zero or multiple topology roots.

Animator does not own world transforms or physics. Pose workers may sample an
immutable root delta candidate, but Main validates Animator, track/control
revision, source Mesh/content revision, skeleton compatibility, Character
ObjectId/generation, and current ancestry before it creates a
`CharacterRootMotionRequest`. Engine applies that request through Character
admission. A second Animator targeting the same Character in one step is
dropped deterministically.

## Extraction math

The canonical clip trajectory is relative to the root pose at clip time zero.
For a sampled translation `P(t)` and rotation `R(t)`:

```text
T(t) = inverse(R(0)) * (P(t) - P(0))
Y(t) = world-up yaw(inverse(R(0)) * R(t))
Q(t) = CFrame(T(t), yawRotation(Y(t)))
delta(a,b) = inverse(Q(a)) * Q(b)
```

Translation preserves X, Y, and Z. Rotation extraction supports only world-up
yaw. Imported glTF handedness conversion determines yaw sign before this
renderer-neutral stage. Near-zero/non-finite quaternions, undefined horizontal
forward, non-finite translation, over-bound deltas, missing root channels, and
invalid topology fail closed.

Tracks retain an unwrapped double-precision logical time separate from public
wrapped `TimePosition`. For a looped clip with duration `D`, cycle transform
`C = Q(D)`, cycle count `N = floor(t/D)`, and remainder `r`:

```text
trajectory(t) = pow(C, N) * Q(r)
```

`pow` uses exponentiation by squaring, so a frame crossing many loops does not
iterate once per loop. Runtime delta is clamped to 60 seconds, track speed is
bounded to `0..16`, and the final movement-request bounds still apply. A loop
therefore carries its cycle displacement instead of subtracting the clip
length at wrap.

Play restarts at logical zero. Stop invalidates the baseline. Seek moves the
baseline to the sought logical time. Pause advances neither logical time nor
motion; Resume continues from the paused baseline. Loop-mode and source
changes reset intentionally. Rejected/stale candidates still advance the
source sample baseline on Main, so collision or lifecycle rejection cannot
become a later lurch. Active tracks pin the immutable animation revision they
loaded; a post-reimport track resolves the new revision.

## Root-pose decomposition

The designated track's sampled root translation is replaced with its time-zero
translation in the visual pose. The extracted world-up yaw is removed from the
root rotation, while pitch/roll residual is preserved. The remaining visual
pose continues through ordinary track blending, hierarchy solve, semantic
joint cache, and GPU/CPU publication.

This establishes one application of trajectory motion:

```text
accepted Character transform
  * residual joint model pose
  * Attachment local transform
```

Collision clipping can produce foot sliding or pushing against a wall. That is
preferable to bypassing physics. Foundation 3A does not rescale animation time
or implement motion warping.

## Frame order and safe point

The current Engine order is:

1. PreSimulation/default or game Luau movement policy;
2. physics step and camera/PostSimulation;
3. PreRender and semantic-requirement preparation;
4. animation logical advance and update-policy classification;
5. pose/root candidate evaluation, optionally on workers;
6. deterministic Main merge and root-request creation;
7. Main Character admission and physics/controller query;
8. accepted Character/RootPart transform commit;
9. semantic Attachment, Sound, Prompt, and interaction update; and
10. renderer-neutral pose and world publication.

`WorldRoot::ResolveKinematicMotion` flushes pending body changes before the
serialized capsule query. Workers never mutate Character, WorldRoot, physics,
semantic state, render state, or ChangeJournal. Character movement is complete
before semantic composition and render extraction.

## Animation 2C policy

A playing/natural-end root-motion source is gameplay-semantic regardless of
visibility. It forces its Animator into `SemanticRequired`; headless and
offscreen execution still advances and evaluates it. Disabling/stopping the
source removes that requirement. With no Attachment/Sound/Prompt consumer, an
offscreen visual-only rig returns to `VisualFrozen` while logical time retains
the existing Animation 2C contract. The mixed benchmark verifies 50
root-motion-required Characters do not escalate 450 offscreen visual-only
rigs.

## Requested versus accepted movement

`RootMotionDelta` records local translation, yaw, source Animator/Rig/track,
animation revision, and unwrapped interval. `CharacterMotionResult` records
requested and applied translation/yaw plus collision, floor, velocity, and
physics timing. Engine metrics separate extraction, request construction,
Character admission, and physics CPU time.

Collision-clipped displacement is discarded for that step. The next interval
starts from the animation baseline, not from the unaccepted delta. This data
shape leaves an internal seam for future intentional motion warping without
making it a public Foundation 3A promise.

## Semantic, headless, lifecycle, and security results

SemanticSpatialResolver observes the RootPart transform committed from actual
accepted movement and combines it with the residual joint pose. Positional
Sound and Prompt consumers already resolve that same Attachment transform, so
range, line of sight, hold validation, and panning cannot use an unaccepted
requested position. The root-motion tests exercise this path with no renderer.

Completed worker output is rejected if Character/Animator is destroyed or
replaced before merge. Invalid binding, incompatible skeleton, missing root
track, stale control/content revision, NaN/Inf samples, enormous deltas, and
multiple roots produce no partial movement. Per-frame request/claim/pending
storage is bounded and retained after warmup. Accepted simulation movement
produces no authoring journal record.

## Benchmark and deferrals

`gargantuan_animation_foundation_benchmark --root-motion` measures Release
matrices for 1, 10, 100, and 500 Characters with root motion disabled and
enabled. It reports total, track advance, pose work, extraction, request,
admission, physics, semantic-anchor time, and allocation count. It also runs
50 enabled Characters plus 450 offscreen visual-only rigs. Existing animation
benchmarks continue to report pose publication and renderer projection costs
separately.

The 2026-09-01 primary Ryzen 9 7950X3D run used 60 measured frames after six
warmup frames, a 64-joint fixture, and no renderer. Times are milliseconds per
frame; pose CPU is accumulated worker CPU and may exceed wall time.

| Characters | Root off total | Root on total | Pose CPU | Extraction | Request | Admission | Physics | Anchors | Steady allocations |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.0088 | 0.0097 | 0.0079 | 0.0004 | 0.0000 | 0.0002 | 0.0001 | 0.0002 | 0 |
| 10 | 0.0854 | 0.0930 | 0.0784 | 0.0041 | 0.0003 | 0.0020 | 0.0012 | 0.0018 | 0 |
| 100 | 0.4025 | 0.5034 | 1.1365 | 0.0599 | 0.0027 | 0.0203 | 0.0117 | 0.0301 | 0 |
| 500 | 1.4477 | 1.6823 | 9.5194 | 0.4453 | 0.0156 | 0.1028 | 0.0581 | 0.2162 | 0 |

The mixed 50-root/450-visual-only case measured 0.4588 ms wall, 0.4573 ms pose
CPU, 0.0252 ms extraction, 0.0014 ms request creation, 0.0108 ms admission,
0.0063 ms physics, and 0.0153 ms anchors with zero runtime, admission, or
spatial allocations. Exactly 50 Animators were semantic/root-required and all
450 visual-only rigs remained frozen; there was no global policy escalation.
The 64-joint memory estimates were 87,006 canonical artifact bytes, 10,240
skeleton bytes, 10,752 Animator-pose bytes, 480 active-track bytes, and 8,192
palette bytes per represented rig where applicable.

Foundation 3B now carries control/input/action identity, pinned AssetId/content
revision, reliable action decisions, server-evaluated root motion, bounded local
prediction, and authoritative restore/replay through the dedicated Character
protocol. Its correction composition test retains the residual pose and
re-resolves animated Attachment, Sound, and Prompt state from the corrected
RootPart without ChangeJournal work. See `CharacterNetworkingFoundation.md`.

Foundation 3C first addresses measured state batching/quantization/cadence,
remote interpolation and presentation smoothing, production session/Luau
bridges, spatial relevance, and authoritative Animator presentation/content
readiness. Motion warping, arbitrary root rotation, IK, retargeting, graphs,
ragdoll, portals, and editor tooling remain deferred until those lower network
and action-presentation contracts are measured.
