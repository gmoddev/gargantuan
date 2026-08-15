---
status: current
owner: physics
last_verified: 2026-08-15
related_code:
  - include/gargantuan/physics/
  - src/physics/
  - include/gargantuan/classes/WorldRoot.hpp
  - src/classes/WorldRoot.cpp
  - tests/PhysicsBackendTests.cpp
related_adrs: []
---

# Physics backend boundary

## Scope

Gargantuan currently uses Box3D for rigid-body simulation. Box3D is an
implementation behind an engine-owned contract; it is not an engine object model,
identity system, or public API. This document describes the implemented boundary,
not a decision to replace Box3D or a specification for future deformables.

```text
BasePart / Constraint semantics
        -> WorldRoot coordination and safe points
        -> PhysicsWorld
        -> IPhysicsBackend
        -> Box3DPhysicsBackend
        -> Box3D
```

`WorldRoot` owns one `PhysicsWorld`. `PhysicsWorld` owns one backend instance, and
the backend privately owns the Box3D world, bodies, shapes, joints, transient
event arrays, and backend-handle mappings.

## Neutral semantic vocabulary

The engine-facing contract is defined by `PhysicsTypes.hpp` and
`PhysicsBackend.hpp`:

- `PhysicsBodyId` and `PhysicsConstraintId` are slot/generation value IDs;
- `PhysicsShapeDesc` describes the supported Part primitives and size;
- `PhysicsBodyDesc` describes transform, shape, anchoring, collision, touch, and density;
- `PhysicsConstraintDesc` currently describes only a weld between neutral body IDs;
- `PhysicsWorldConfig` and `PhysicsStepConfig` define gravity and explicit fixed-step input;
- `PhysicsBodyMotion` and `PhysicsContactEvent` are owned post-step results; and
- `PhysicsOperationResult` reports neutral success, invalid-ID,
  invalid-description, or backend-failure states.

The contract intentionally has no mesh collider, character motor, projectile,
network ownership, prediction, rollback, or deformable vocabulary.

## Identity and mapping

Physics identity is independent of `ObjectId` and independent of Box3D handles.
An Instance may currently have one body, but that association is a `WorldRoot`
coordination detail rather than an identity equivalence.

Each backend ID contains a nonzero slot and generation. Destroy invalidates the
slot and increments its generation before reuse. Invalid, stale, and duplicate
destroy operations fail closed with a structured result. Generation exhaustion
retires the slot rather than wrapping it.

The Box3D adapter privately maps a live neutral ID to a body record containing
the current `b3BodyId` and `b3ShapeId`. Box3D body user data points only to a
backend-owned stable record. The adapter normalizes that record back to a
generation-checked `PhysicsBodyId` before returning an event. No Instance pointer
is stored in Box3D.

Shape replacement does not change `PhysicsBodyId`. This permits size, primitive,
collision, and touch changes without making backend reconstruction observable to
the engine. Destroying a body invalidates attached neutral constraint IDs before
the body slot can be reused.

## Shapes and bodies

`Part::GetPhysicsShape` converts public `PartType` and `Size` into neutral shape
semantics. Box, ball, cylinder, wedge, and corner-wedge Box3D geometry is
constructed only inside `Box3DPhysicsBackend.cpp`.

`BasePart` does not create Box3D shapes or hold a backend handle. `WorldRoot`
builds a `PhysicsBodyDesc` from committed Part state. The backend chooses whether
an update is an in-place transform/body-type change, an event-flag update, or a
private shape replacement.

An unsuccessful shape replacement leaves the previous backend shape and neutral
identity intact. The update reports failure, `WorldRoot` logs it with the
`[Physics:Backend]` prefix, and the dirty update remains eligible for retry. The
engine does not claim silent physics synchronization success.

## Main-domain safe points

Generated setters commit authoritative Instance state and then fire their normal
property signal. For a tracked `BasePart`, `WorldRoot` observes `CFrame`, `Size`,
`Anchored`, `CanCollide`, `CanTouch`, and `Shape` and records only a dirty neutral
body intent. It does not call Box3D from the setter callback.

At the beginning of `StepPhysics`, on Main and before any Box3D step,
`WorldRoot` applies dirty body updates in sorted `ObjectId` order. Gravity and
constraint changes use the same pre-step boundary. Pending impulses are applied
immediately before a fixed simulation step. Removal or destruction erases dirty
work before destroying the body, so a dead Part cannot be recreated by an older
intent.

Creating or removing a descendant is also a Main-domain non-stepping safe point.
Those operations may create or destroy neutral bodies immediately because the
current engine has one serialized Main simulation domain and cannot execute them
concurrently with `IPhysicsBackend::Step`.

## Constraints

`Constraint` and `WeldConstraint` expose neutral constraint descriptions only.
`WorldRoot` resolves endpoint Parts to neutral body IDs, validates that the
constraint and both endpoints belong to the same world, and asks `PhysicsWorld`
to create the joint.

Changes to enabled state or endpoints are queued for the pre-step safe point.
Body shape replacement preserves the Box3D body and attached joint. Body removal
causes the backend to invalidate attached constraint IDs; `WorldRoot` then drops
the stale association and can reconcile a still-live constraint if its endpoints
later become valid again.

## Step and publication

`WorldRoot` retains the existing 60 Hz fixed step, four substeps, four-step
per-frame cap, and accumulator reset after the cap. The backend owns direct world
stepping and copies transient Box3D move/contact/sensor arrays into neutral
vectors before returning. Each motion/contact collection is capped at 65,536
entries per step; truncation is explicit in the result and logged by `WorldRoot`.

Dynamic body motion is resolved from `PhysicsBodyId` to a live tracked Part. The
result is published through the ordinary authoritative `SetCFrame` path. A
narrow publication guard prevents that physics-authored write from being queued
back into the backend. Fuzzy equality suppresses no-op publication.

This is the current authoritative transform model. It does not yet distinguish
server physics, replicated transforms, prediction, or developer ownership.

## Contact and sensor events

The Box3D adapter enables contact/sensor event collection according to neutral
`CanCollide` and `CanTouch` state. It copies both contact and sensor begin/end
events, resolves their shapes to generation-checked body IDs, and deduplicates
body pairs within the step.

`WorldRoot` resolves both bodies to live Parts before firing `Touched` or
`TouchEnded`. Each Part receives the signal only when its current `CanTouch` is
true. No Box3D callback/event structure or borrowed event memory reaches an
Instance.

## Queries

There is no implemented `WorldRoot` raycast or active scripted raycast API in the
current source. Foundation 1 therefore does not add a speculative query method
to the backend contract. Any first query implementation must return neutral
Gargantuan identity/value results and keep Box3D callbacks and hit structures
inside the adapter.

## Failure and lifetime behavior

- invalid and stale body/constraint IDs fail closed;
- duplicate destruction is safe and returns an invalid-ID status;
- non-finite or non-positive body descriptions are rejected;
- failed shape replacement preserves the prior coherent backend state;
- body destruction first destroys and invalidates attached constraints;
- post-step results own all data needed outside the backend;
- `WorldRoot::Destroying` and the native destructor both run one idempotent shutdown path;
- shutdown disconnects property/descendant observers and clears pending work before backend destruction; and
- the backend destroys joints before bodies and destroys the Box3D world last.

## Box3D confinement and replaceability

Runtime Box3D includes and `b3*` symbols are confined to
`src/physics/Box3DPhysicsBackend.cpp` and its private conversion header. CMake
still names the Box3D dependency and include directory; that is unavoidable
build/dependency integration rather than a runtime semantic leak.

`IPhysicsBackend` can be implemented and tested without including Box3D. The
focused test suite supplies a recording backend to prove neutral creation,
update, and step delegation. A different rigid backend can implement the
contract without changing BasePart, Constraint, networking, rendering,
persistence, or Luau APIs. Selecting the default adapter remains an internal
physics implementation choice.

## Deliberately deferred

This boundary does not implement another physics engine, advanced constraints,
mesh collision, a character motor redesign, projectiles, terrain physics,
deformation, physics networking, ownership, interpolation, prediction, or
rollback. Current rendering still reads the engine-owned `WorldRoot::Parts`
collection and has no dependency on physics backend handles.
