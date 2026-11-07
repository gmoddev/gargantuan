---
status: current
owner: physics
last_verified: 2026-11-07
related_code:
  - include/gargantuan/physics/PhysicsTypes.hpp
  - include/gargantuan/physics/PhysicsBackend.hpp
  - src/physics/Box3DPhysicsBackend.cpp
  - src/classes/WorldRoot.cpp
  - assets/classes/BasePart.luau
  - include/gargantuan/datatypes/RaycastParams.hpp
  - tests/PhysicsBackendTests.cpp
---

# Physics foundation: body controls and collision groups

`WorldRoot` remains the only bridge from committed `BasePart` state to a neutral
`PhysicsBodyDesc`. `PhysicsWorld` delegates operations to its single rigid
backend. The Box3D adapter owns native bodies, shapes, filters, and mass data;
no native type or handle crosses the public boundary. Body IDs remain slot and
generation values. Deformable physics is unchanged.

## Authored body state

`BasePart` persists and replicates `Kinematic`, `CollisionGroup`,
`CollisionMask`, `LinearDamping`, `AngularDamping`, `GravityScale`, and
`SleepEnabled` through the existing generated native property, reflection,
mutation, and project serialization paths. `Anchored` still means static.
`Kinematic` has effect only while unanchored, so an anchored Part can be
prepared for a later kinematic transition. An unanchored nonkinematic Part is
dynamic. Static triangle mesh collision remains static only; an unanchored
MeshPart requesting that mode retains the existing Box fallback.

The damping and gravity scale range is finite `[0, 100]`. The native adapter
rejects invalid direct neutral descriptions before mutation. A failed shape
replacement keeps the old shape and body ID. `WorldRoot` applies authored
changes at the existing safe point, before a query or fixed step.

## Runtime controls and observation

`BasePart` exposes `SetLinearVelocity`, `SetAngularVelocity`, `SetAwake`,
`ApplyImpulse`, `ApplyAngularImpulse`, `ApplyImpulseAtPosition`, `ApplyForce`,
and `ApplyTorque`. One-shot methods queue on the Part and apply at the next
fixed step. Velocity and sleep requests use last-write-wins pending values.
Center impulses and forces aggregate, while up to 64 point impulses per Part
are kept in call order. Each vector must be finite with squared length at most
`1e12`; invalid or over-limit requests leave the pending state unchanged.
The queued request is discarded if the body has become static or kinematic
when the safe point is reached. A force or torque is applied once before one
fixed solver step and remains a force over its substeps. It is not converted to
an impulse or replayed for subsequent steps. Dynamic bodies accept forces and
impulses. Dynamic and kinematic bodies accept velocity writes. Sleep control
requires a moving body.

For compatibility, the original center `ApplyImpulse` backend operation accepts
a finite static-body request as a no-op and invalidates the static-step cache.
New angular/point impulse and force/torque operations require dynamic bodies.

`GetLinearVelocity`, `GetAngularVelocity`, `GetMass`, `GetLocalInertia`, and
`GetAwake` read the last main-thread body state publication. Local inertia is
the diagonal of the body-local tensor in engine units; this intentionally does
not expose Box3D's matrix type. Mass and inertia are zero before body admission
and after removal. Pending velocity and awake requests are visible to their
corresponding getters before the safe point. These are runtime observations,
not durable properties or a second motion authority. Solver motion continues
to publish `CFrame` through the existing WorldRoot path.

Every runtime control method, including the original `ApplyImpulse`, requires
`MutateDataModel` before it changes pending state. All five inspection methods
require `ReadDataModel`. These method-level checks complement the existing
execution-domain and object-liveness guards. Denied calls leave the pending
physics queue unchanged.

## One collision compatibility rule

There are 16 numbered groups, `0..15`. Each body belongs to one group and has
a 16-bit mask. Two bodies can pair only if each mask includes the other's
group bit. Group `0` and mask `65535` are the defaults. This neutral rule is
`PhysicsCanCollide` in `PhysicsTypes.hpp`; the adapter projects the same values
to a native category bit and mask, with native group override disabled. It
applies to solid contacts and sensor/touch pairs. Runtime mask changes update
all shapes of a body, including chunked static meshes, without changing body
identity. `CanCollide` and `CanTouch` retain their separate existing meanings.

`RaycastParams` has matching `CollisionGroup` and `CollisionMask` fields with
the same bounds and defaults. They combine with the existing semantic
include/exclude set. Query callbacks check the same mutual compatibility rule
against each body's committed filter, so runtime changes affect subsequent
queries. The query object remains VM-local and is not persisted. Invalid
group or mask input fails before the backend query.

## Qualification and future comparison

The focused backend and WorldRoot tests cover velocity, impulse, force,
torque, mass, inertia, sleep, static/kinematic/dynamic transitions, stale ID
rejection, query filtering, and runtime sensor filtering. Normal class
generation and native test execution are required before treating the slice as
qualified; test execution is recorded by the integration supervisor.

Future backend consolidation should compare these neutral semantics and the
Engine-owned safe-point, reflection, persistence, identity, query, contact,
and lifecycle work as well as solver runtime. In particular, a later Jolt
assessment must test deformable-to-rigid feedback and the maintenance cost of
bounded source patches across upstream rebases. This slice adds no new
cross-solver authority or feedback path.
