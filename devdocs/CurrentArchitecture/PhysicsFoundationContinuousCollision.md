---
status: current
owner: physics
last_verified: 2026-11-07
related_code:
  - include/gargantuan/physics/PhysicsTypes.hpp
  - src/physics/Box3DPhysicsBackend.cpp
  - src/classes/WorldRoot.cpp
  - assets/classes/BasePart.luau
  - tests/PhysicsBackendTests.cpp
---

# Physics foundation: bounded continuous collision policy

`BasePart.MovingBodyCCD` is a saved and replicated Bool, default false. It
requests an additional swept collision pass against moving rigid bodies. The
property is neutral: neither the public value nor `PhysicsBodyDesc` exposes a
Box3D identifier or bullet type. `WorldRoot` projects it to the neutral body
description only while the Part is dynamic and solid. Anchored, kinematic, and
noncolliding Parts retain the authored value but do not activate the option.
At the existing safe point, changes update the same generation-safe body ID.
Direct neutral descriptions reject an effective option on those unsupported
motion or sensor modes before mutating native state.

The current adapter maps an effective request to Box3D's per-body bullet flag.
The pinned default world definition already enables continuous collision for
fast dynamic bodies against static bodies. This change leaves that default
untouched. The opt-in flag extends the sweep to dynamic and kinematic targets.
Shape contact filters, constraint exclusions, and the current solver step still
apply. The source and focused fixtures establish these exact limits:

- The swept source must be a solid dynamic primitive. A static mesh can be a
  target, but cannot be the moving swept source. A dynamic MeshPart requesting
  static mesh collision retains the existing Box fallback.
- The moving-target pass skips other opt-in bullet bodies. Two fast moving
  bodies are not guaranteed to meet because target movement and sweep order
  are not a relative-motion projectile solution.
- Sensor source shapes do not use solid CCD. Sensors are observed through the
  existing overlap/touch path, which has its own event and capacity limits.
- Initial overlap is ignored for continuous collision to avoid freezing a
  body. Teleports and direct `CFrame` writes are not swept motion.
- The pinned world default caps linear speed at 400 length units per second
  with its current length scale. The Engine fixed step is 1/60 second with four
  substeps. Larger requested velocities, thin features below solver tolerance,
  and different step sizes have no tunneling-prevention claim.
- CCD can clip motion at time of impact without promising a touch event in the
  same step. Gameplay requiring precise projectile hit timing should use a
  bounded cast or ray with relative target motion policy.

The focused tests exercise balls at 360 units per second against a thin static
box, a moving dynamic box, and a static triangle mesh. Repeated projectiles
retire and recreate generation-safe bodies. A WorldRoot fixture checks
published `CFrame`, runtime authored-state transitions, and Luau observation.
The supervisor's native worker build and test run are required before these
fixtures can be reported as qualified.

The pinned Box3D solver captures body movement events before its continuous
pass. The adapter uses those events for bounded identity/order notification
but reads each moved body's final native transform after the step. Publishing
the event's earlier transform can show tunneling in WorldRoot despite a correct
native CCD result. The regression checks the published Part pose, and the fix
stays within the adapter without modifying the pinned dependency.

No world-level CCD toggle, new collision authority, XPBD coupling, or backend
replacement is introduced. Later backend comparison should include the
Engine-owned safe-point and persistence work, deformable-to-rigid feedback
requirements, and maintenance cost of bounded upstream source patches.
