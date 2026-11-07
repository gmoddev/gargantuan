---
status: current
owner: physics
last_verified: 2026-11-07
related_code:
  - include/gargantuan/physics/PhysicsTypes.hpp
  - src/physics/Box3DPhysicsBackend.cpp
  - assets/classes/JointConstraint.luau
  - src/classes/JointConstraint.cpp
  - src/classes/WorldRoot.cpp
  - tests/PhysicsBackendTests.cpp
---

# Physics constraint expansion

`JointConstraint` extends the existing reflected `Constraint` class. The class
can be created by Luau, Studio's schema editor, persistence and replication
through the normal generated class metadata. `Attachment0` and `Attachment1`
must each be direct children of distinct live `BasePart`s in the same
`WorldRoot` as the constraint. Each attachment's local `CFrame` is the local
joint frame. Reparenting an attachment or changing its frame schedules a
pre-step replacement. Endpoints are resolved to generation-safe neutral body
IDs; detached, destroyed, cross-world and identical endpoints cannot create a
joint.

`JointType` accepts the exact strings `BallSocket`, `Hinge`, `Slider` and
`Distance`; an unsupported value is rejected at the reflected setter before
the authored property changes. A ball socket fixes the two frame origins and permits free
rotation. A hinge fixes their origins and allows rotation about each frame's
local z-axis. A slider allows translation along frame A's local x-axis without
relative rotation. A distance joint connects the two frame origins at
`RestLength` world units. `SpringFrequency > 0` enables a distance spring;
zero gives a rigid distance. `SpringDamping` is a dimensionless damping ratio.

`EnableLimit` activates `LowerLimit` and `UpperLimit` on hinges (radians),
sliders (world units), and distance springs (world units). Distance limits
require the spring to be enabled. `EnableMotor`, `MotorSpeed` and
`MaximumMotorForce` apply to hinges (radians/second and torque) or sliders
(world units/second and force). Ball sockets and welds accept neither limits
nor motors. Constraint breakage and distance motors are outside this slice.

The neutral adapter rejects unknown types, non-finite frames and scalars,
non-rigid local rotations, reversed limits, unsupported option combinations,
zero motor force, out-of-range hinge angles and excessive local offsets. It
never passes these descriptions to Box3D. A rejected authored update leaves
the previous live joint in place and emits at most one
`[Physics:Constraint]` diagnostic for that constraint until a valid update
succeeds. A valid update creates the replacement before retiring the old ID.
Body destruction invalidates attached IDs before body-slot reuse. Joint
creation, replacement and removal occur on WorldRoot's Main-domain safe point;
generated setters only mark dirty intent.

Box3D-specific work remains in `Box3DPhysicsBackend.cpp`: spherical,
revolute, prismatic and distance definition translation, frame and parameter
validation, native joint creation and destruction. The reflected class,
neutral description and WorldRoot lifecycle do not expose Box3D handles. A
later backend comparison must reimplement these four translations and repeat
the native motor, spring, limit, identity and authored-lifecycle tests; this
work does not begin a Jolt evaluation.

The example `assets/examples/constraint-mechanics.luau` creates a hinged door,
driven slider and suspended distance spring through the public authoring path.
