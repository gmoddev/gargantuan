---
status: current
owner: physics
last_verified: 2026-11-07
related_code:
  - assets/classes/SoftBodyAttachment.luau
  - assets/classes/WorldRoot.luau
  - include/gargantuan/physics/SoftBodyTypes.hpp
  - include/gargantuan/physics/SoftBodyBackend.hpp
  - src/physics/XpbdSoftBodyBackend.cpp
  - src/classes/WorldRoot.cpp
  - tests/SoftBodyPhysicsTests.cpp
related_adrs: []
---

# Physics foundation soft-body control

This slice extends the existing [Foundation 2](SoftBodyPhysicsFoundation2.md)
contract. `SoftBodyAttachment.Position`, `Enabled`, and `VertexIndex` remain
serializable, replicated semantic properties. `WorldRoot` still gathers enabled
descendant attachments in stable vertex order, with the lowest `ObjectId`
winning duplicates, and calls `SoftBodyWorld::UpdateBody` at the Main safe
point. No solver state or backend ID is persisted.

When only the attachment set changes, XPBD updates pinned node positions and
inverse masses in the current record. Unpinned vertices retain their positions
and velocities; a newly released vertex starts with zero velocity. The
topology revision and indices remain stable, while the vertex and definition
revisions advance. Invalid or duplicate backend attachment entries reject
before any node is changed. Updates concurrent with an in-flight solve keep
the existing replacement and stale-result policy; the fast path applies when
the current record is available at the safe point.

`WorldRoot:GetDeformableInfo(Body)` returns read-only vertex, constraint, and
active attachment counts, enabled/simulated status, and topology/vertex
revisions. `WorldRoot:GetDeformableVertex(Body, VertexIndex)` returns one
zero-based world-space position. Both return `nil` for unavailable, stale, or
out-of-world bodies; the vertex query also returns `nil` for an out-of-range
index. They require `ReadDataModel`, use the current generation-safe backend
mapping, and return no writable solver storage. Inspection is bounded to one
body record or one vertex, with the existing 65,536-vertex body limit.

`WorldRoot:GetSoftBodyStepInfo()` returns read-only counters from the most
recent completed fixed step: simulated bodies/vertices, dispatched jobs,
collider queries/candidates, frozen bodies, backlog drops, and stale results.
These are per-world step totals, not a second simulation authority or
unbounded per-body history.

The cross-solver boundary remains one-way. `WorldRoot` snapshots up to 4,096
rigid primitive transforms after the Box3D step, then XPBD consumes those
immutable snapshots. Cloth and rubber contacts do not apply forces to rigid
bodies. The soft collision path is discrete; neither moving rigid colliders
nor high-speed soft vertices have swept-contact or tunneling-prevention
guarantees. A WorldRoot fixture moves an anchored box into cloth and checks
that the next fixed step changes a contacted vertex. Supported narrow-phase
primitives are ball, box, cylinder, wedge,
and corner wedge; static triangle meshes and soft/soft contacts are unsupported.
This snapshot bridge, its synchronization order, and the separate Box3D/XPBD
contact code are continuing ownership and maintenance costs to measure in a
future comparison. A future bidirectional interaction requirement must be
tested explicitly; the present attachment and diagnostic work does not claim
it.
