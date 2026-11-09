---
status: experimental-protocol
authority: non-normative
date: 2026-11-09
---

# Cloth contact qualification trace V1

This is an isolated experiment format. It does not change the Engine physics API,
authored geometry, persistence, or production defaults. A case directory contains
`ContactFixtureV1.json`, `ContactTraceV1.jsonl.gz`, optional float32 sidecars,
and `Source.json`. Capture quality traces separately from timing runs.

## Fixture

All vectors use world-space metres and `[x,y,z]` order. The JSON object has:

| Field | Meaning |
| --- | --- |
| `Schema` | Exactly `Gargantuan.ContactFixture.V1`. |
| `CaseId` | Stable nonempty case name, including symmetry sign and solver control. |
| `Frames`, `DeltaTimeSeconds` | Positive physical step count and fixed duration per step. Frame 0 is the first poststep state. |
| `RestPositions` | One finite xyz triple per cloth vertex in stable index order. |
| `InitialVelocities` | One finite xyz triple per vertex, before frame 0. |
| `VertexMassKg` | One finite nonnegative mass per vertex; pinned vertices may have zero mass. |
| `Triangles` | Oriented triples of distinct vertex indices. Winding is shared by Jolt and XPBD fixtures. |
| `StructuralEdges` | Objects `{A,B,RestLengthM,Kind}`; `Kind` is `axial`, `shear`, or `bend`. Positive `StiffnessNPerM` may be supplied for a clearly labeled spring-energy surrogate. |
| `Pins` | Objects `{Vertex,Target}` for fixed world-space targets. |
| `ThicknessM` | Nonnegative collision shell added to the geometric sphere radius for independent surface clearance. |
| `GravityMPerS2` | Finite xyz gravity vector. |
| `Colliders` | Objects `{Id,Kind,RadiusM,MassKg,Motion,Trajectory}`. V1 supports sphere; `Motion` is `static`, `kinematic`, or `dynamic`. `MassKg` is null for infinite-mass motion. Optional positive scalar `InertiaKgM2` is the isotropic sphere inertia for kinetic accounting. `Trajectory` records authored input, including its interpolation rule; actual state is in each frame. |
| `Gates` | Prespecified `MaximumAxialStrain`, `MaximumPinErrorM`, and `MaximumSpherePenetrationM`; optional `MinimumContactCount`, `MaximumContactCount`, and `MaximumContactImpulsePairResidualNs` must be declared before results. A failed legacy limit is retained, not silently adjusted. |

The original 128×128 planar test is one fixture, not a replacement for 9×9,
17×17, and 32×32 discriminating cases. Cases cover exact Z=0, signed
±10 µm and ±1 cm offsets, rotated cloth planes, face/edge/corner alignment,
top and alternative pins, radius/mass/speed changes, static/true kinematic/
dynamic spheres, oblique, repeated and simultaneous contact, no-contact
controls, and long settling. Solver, geometry and authored motion parameters
must be explicit in each fixture. Signed offsets are paired mirror controls.

## Full physical trace

`ContactTraceV1.jsonl.gz` contains exactly `Frames` JSON objects in order. A
record has `Frame` (0-based), `TimeSeconds` (poststep time), full `Positions`
and `Velocities` as flattened xyz arrays, and `ColliderStates` with actual
`Id`, `Center`, `OrientationWxyz`, `LinearVelocity`, and `AngularVelocity`.
`TimeSeconds` is `(Frame + 1) * DeltaTimeSeconds` within float32 tolerance.
Positions and velocities may instead use `PositionsRef` and `VelocitiesRef`:

```json
{"File":"positions.f32le","OffsetBytes":32,"Count":49152,"Sha256":"64 lowercase hex characters"}
```

The reference reads exactly `Count` little-endian float32 values. `File` is a
relative path confined to the case directory. `Sha256` covers that frame's raw
bytes. Existing `PJTRACE1` position payloads can be referenced at their
32-byte-header plus frame offset. A companion velocity sidecar is allowed.
The validator accepts inline or referenced arrays, never an omitted position
array. Missing velocities mark kinetic and full physical qualification
`INCOMPLETE`; position differences must not be called impulses or velocities.

Optional `Contacts` contains one record per resolved interaction, including
`ColliderId`, `Feature` (`vertex`, `edge`, `triangle`), `Indices`, `Point`,
`Normal`, `ImpulseOnClothNs`, `ImpulseOnRigidNs`, and the phase/substep time.
The two impulse vectors are signed world-space impulses on their named bodies.
Projection displacement is recorded separately as `ProjectionDeltaM`, never
substituted for a physical impulse. `External` per-frame signed work fields are
`ActuatorWorkJ`, `StaticPoseProjectionWorkJ`, `PinWorkJ`, `OtherWorkJ`, and
nonnegative `DissipationJ`, each with a measurement method in the producer's
source notes. Gravity is represented by potential `-m g·x`, not double-counted
as external work. A static-pose teleport must account for its numerical
projection work, despite zero nominal static-body velocity. An omitted term means energy closure is
`INCOMPLETE`, including for a static pose jump whose nominal velocity is zero.
`Stages` may contain sparse or full pre/post structural/contact snapshots for
the exact offending edge; stage samples do not replace the full frame arrays.

The private XPBD producer may add `External.XpbdContactAccounting`. It records
source-measured cloth normal and friction velocity-derived impulse components,
prescribed collider reaction (defined as their negative, **not** observed
dynamic rigid momentum), contact kinetic change, prescribed kinematic center
work sampled per substep, and an explicit `StaticPoseWorkComplete` flag. Its
kinetic stage ledger splits applied gravity/force, uniform impulse, point
impulse, damping, structural projection, contact projection and pin projection.
Its momentum stage ledger records start/end cloth momentum, those source
impulses and pinned-edge reaction. The independent inspector rechecks all
identities and reports momentum residual scaled by total impulse magnitude.
These identities establish instrumentation consistency only. The later-frame
physical energy diagnostic uses published K + gravity potential + distance
compliance potential and subtracts prescribed kinematic work and measured
damping change. It has no pass threshold in V1. Frame 0 needs separate
authored impulse accounting; changing a static pose leaves work incomplete.

The private precision experiment may add `VelocityPrecisionMode` as string
`"0"`, `"1"`, or `"2"`, with matching `IdealVelocityReconstructionEnabled`
and `DoubleShadowEnabled` booleans. Mode 1 and mode 2 also report finite
`IntegrationPositionRoundingKineticDeltaJ` and
`IdealVelocityReconstructionKineticDeltaJ`; these close a stage bookkeeping
gap and are not physical work. `UnpinnedStructuralNumericalImpulseNs` remains
the would-be float-store error, but the independent inspector computes raw
mode-1 and mode-2 cloth momentum directly from published `ΣmV` and named
impulses without subtracting it. `PinConstraintNumericalImpulseNs` is an
optional separately reported actual-minus-ideal pinned-edge store gap.

`Source.json` records exact Engine and native hashes, solver variant, OS,
compiler/configuration, worker cap, seed policy, case manifest SHA-256, compressed
trace SHA-256, and `Receipt.json` SHA-256. The validator reports missing provenance
as `INCOMPLETE`; it never infers it from a directory name.

## Independent checks and limits

The validator computes axial and all-edge strain from rest lengths, pin drift,
finite full 3D position/velocity, sphere distance to every triangle (including
edge and corner closest points), triangle area ratio, projected orientation
relative to the rest normal, and trajectory stability. The rest plane may have
any orientation. When the complete trace stays in that plane and the mesh is
a single triangulated disk, strictly positive face areas plus a simple outer
boundary certify absence of face-interior self-overlap by the planar degree
argument. The validator checks all nonadjacent boundary segment pairs with a
bounded sweep. Any planar inversion, collapse or boundary crossing fails the
geometry gate. In 3D, a negative projected area can be a valid fold; full
triangle-triangle self-intersection remains `INCOMPLETE`. Kinetic energy uses measured masses and actual
velocities. Optional edge spring energy is labeled a surrogate, not XPBD or
Jolt internal stored energy. External-work and contact-impulse balance are
checked only when complete signed terms and phase timing are present.

V1 bounds each case to 16,384 vertices, 600 frames, 32,512 triangles,
100,000 structural edges, 256 pins, 8 spheres, and 5 million triangle-sphere
checks. Planar boundary testing is capped at 2 million candidate pairs. A trace line is at most
8 MiB decompressed. Malformed, nonfinite, oversized, missing, out-of-order,
or out-of-directory content fails closed. Geometry may pass while energy,
impulse, or self-intersection remain `INCOMPLETE`; the overall result cannot
claim full physical qualification in that state.
