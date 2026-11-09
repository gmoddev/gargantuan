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
| `StructuralEdges` | Objects `{A,B,RestLengthM,Kind}`; `Kind` is `axial`, `shear`, or `bend`. `StiffnessNPerM` may be supplied for a clearly labeled spring-energy surrogate. |
| `Pins` | Objects `{Vertex,Target}` for fixed world-space targets. |
| `ThicknessM` | Nonnegative collision shell added to the geometric sphere radius for independent surface clearance. |
| `GravityMPerS2` | Finite xyz gravity vector. |
| `Colliders` | Objects `{Id,Kind,RadiusM,MassKg,Motion,Trajectory}`. V1 supports sphere; `Motion` is `static`, `kinematic`, or `dynamic`. `MassKg` is null for infinite-mass motion. `Trajectory` records authored input, including its interpolation rule; actual state is in each frame. |
| `Gates` | Prespecified `MaximumAxialStrain`, `MaximumPinErrorM`, and `MaximumSpherePenetrationM`, plus any separately justified area, energy, or impulse limits. A failed legacy limit is retained, not silently adjusted. |

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
substituted for a physical impulse. `External` records force/actuator work,
gravity and pin reactions, and any static-pose teleport projection work with
an explicit measurement method. An omitted work term means energy closure is
`INCOMPLETE`, including for a static pose jump whose nominal velocity is zero.
`Stages` may contain sparse or full pre/post structural/contact snapshots for
the exact offending edge; stage samples do not replace the full frame arrays.

`Source.json` records exact Engine and native hashes, solver variant, OS,
compiler/configuration, worker cap, seed policy, case manifest SHA-256, trace
SHA-256, and process receipt hashes. The validator reports missing provenance
as `INCOMPLETE`; it never infers it from a directory name.

## Independent checks and limits

The validator computes axial and all-edge strain from rest lengths, pin drift,
finite full 3D position/velocity, sphere distance to every triangle (including
edge and corner closest points), triangle area ratio, projected orientation
relative to the rest normal, and trajectory stability. A negative projected
area is a planar inversion only while the trace remains in the rest plane;
in 3D it can be a valid fold and is reported without automatic failure. Full
self-intersection requires separate triangle-triangle evidence, not a claim
from projected winding alone. Kinetic energy uses measured masses and actual
velocities. Optional edge spring energy is labeled a surrogate, not XPBD or
Jolt internal stored energy. External-work and contact-impulse balance are
checked only when complete signed terms and phase timing are present.

V1 bounds each case to 16,384 vertices, 600 frames, 32,512 triangles,
100,000 structural edges, 256 pins, and 8 spheres. A trace line is at most
8 MiB decompressed. Malformed, nonfinite, oversized, missing, out-of-order,
or out-of-directory content fails closed. Geometry may pass while energy,
impulse, or self-intersection remain `INCOMPLETE`; the overall result cannot
claim full physical qualification in that state.
