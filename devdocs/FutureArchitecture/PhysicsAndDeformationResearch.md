---
status: research
authority: non-normative
---

# Gargantuan Physics and Deformation Research Brief

## Status

**Purpose:** Research and design input for the future Gargantuan physics architecture after the current Box3D coupling is removed behind an engine-owned physics boundary.

**This document is not an implementation specification.** It records desired capabilities, candidate backends, architectural options, performance principles, and experiments that should inform a later design.

---

# 1. Executive Summary

Gargantuan should remain a **rigid-body-first game engine**. Ordinary Parts, assemblies, characters, constraints, raycasts, and collision queries must remain inexpensive enough to support large game worlds.

At the same time, Gargantuan should eventually expose deformation capabilities substantially beyond a conventional rigid-body platform:

* rigid-body physics;
* high-quality constraints and assemblies;
* raycasts, shape casts, overlaps, and character movement;
* visual-only deformation;
* permanent/plastic deformation such as vehicle dents and crushed metal;
* elastic/rubber-like deformation;
* cloth and thin deformable surfaces;
* optional physically significant particle simulation later;
* large-scale visual-effect particles handled separately by the renderer;
* Studio deformation editing;
* saving deformable state;
* baking deformation into a new static mesh;
* converting the current deformed shape into a new rest shape.

The guiding principle should be:

> **Expensive physics is acceptable when the result justifies it, but expensive physics must never become the default cost of ordinary objects.**

The architecture should therefore support **capability tiers and simulation LOD**, rather than attempting to make every object a full deformable body.

The current strongest rigid-body candidates are **Box3D 3D and Jolt Physics**. PhysX is an important reference and possible specialized backend, particularly for deformables, but its current deformable and particle implementations require CUDA-capable GPUs, making those features unsuitable as Gargantuan's cross-platform contract.

The likely long-term direction is:

```text
                     WorldRoot
                        │
                Gargantuan Physics API
                        │
       ┌────────────────┴────────────────┐
       │                                 │
Rigid/Collision Backend            Deformation Layer
       │                                 │
 Box3D or Jolt               Visual / Plastic / Elastic
                                         │
                               Cloth / future volumes

Renderer
   │
   └── GPU VFX particle simulation
```

This keeps Gargantuan's public semantics independent of whichever third-party solver is currently underneath them.

---

# 2. Current Gargantuan Situation

Gargantuan currently exposes Box3D implementation details directly inside `WorldRoot`. The class stores `b3WorldId`, `b3BodyId`, and `b3JointId` and includes Box3D headers directly.

`WorldRoot.cpp` directly performs:

* world creation;
* body creation;
* shape creation;
* joint creation;
* stepping;
* movement-event handling;
* contact-event handling;
* impulse application;
* body/joint destruction;
* Box3D↔Gargantuan transform conversion.

This is acceptable for an early implementation, but it should not become the permanent engine contract.

Before selecting a long-term backend, Gargantuan should establish engine-owned concepts such as:

```text
PhysicsWorld
PhysicsBodyId
PhysicsConstraintId
PhysicsShapeId

PhysicsBodyDesc
PhysicsShapeDesc
PhysicsConstraintDesc

PhysicsContact
PhysicsBodyMotion
PhysicsQueryResult
```

`BasePart`, `Constraint`, `WorldRoot`, Luau APIs, serialization, replication, and Studio should not expose `b3*`, `JPH::*`, `Px*`, or other backend-native objects.

---

# 3. Primary Product Goals

## 3.1 Rigid bodies remain the baseline

The majority of objects should use ordinary rigid-body simulation.

The engine should be designed so that a world containing no deformable objects does not meaningfully pay for deformable-body infrastructure.

Rigid simulation needs to scale well for:

* Parts;
* large assemblies;
* vehicles;
* props;
* characters;
* constraints;
* static environment geometry;
* server simulation;
* collision queries.

---

## 3.2 Deformation should be opt-in

An ordinary Part should not automatically acquire:

* deformation nodes;
* mutable mesh buffers;
* expensive collision recooking;
* deformable solver state;
* additional replication state.

Deformation should exist because the developer explicitly requested it.

---

## 3.3 Gargantuan should provide graceful quality tiers

The engine should not treat physics quality as binary.

A possible eventual hierarchy is:

| Mode                |        Cost | Behavior                                   |
| ------------------- | ----------: | ------------------------------------------ |
| Rigid               |    Very low | Ordinary fixed-shape rigid body            |
| Visual deformation  |         Low | Appearance changes; rigid collider remains |
| Plastic deformation |  Low–medium | Persistent dents/crushing                  |
| Elastic deformation | Medium–high | Shape continuously compresses/restores     |
| Full deformable     |        High | Fine-grained deformable simulation         |
| Cloth               | Medium–high | Surface deformation                        |

This is more useful than forcing a developer to choose between:

> completely rigid

and:

> full volumetric FEM simulation.

---

# 4. Performance Philosophy

The target should not be "all advanced physics is cheap."

That is unrealistic.

Even NVIDIA's PhysX performance guidance states that deformables and particles are considerably more expensive than rigid bodies and recommends approximating them with rigid bodies, compliant contacts, or joints where possible.

The appropriate Gargantuan philosophy is:

> **The engine may expose expensive capabilities, but their cost must be legible, controllable, scalable, and avoidable.**

A developer should be able to knowingly spend performance for something visually or mechanically valuable.

This means Gargantuan should eventually support concepts such as:

* simulation quality;
* solver iteration budgets;
* maximum deformation-node counts;
* distance-based deformation LOD;
* sleep/freeze behavior;
* server-versus-client simulation policy;
* visual fallback modes;
* collision fidelity levels.

---

# 5. Candidate Physics Backends

## 5.1 Box3D 3D

### Current strengths

Box3D 3D currently provides:

* continuous collision detection;
* convex hulls;
* capsules;
* spheres;
* triangle meshes;
* height fields;
* raycasts;
* shape casts;
* overlap queries;
* sensors;
* a character mover;
* several common joint types;
* island sleeping;
* multithreading;
* SIMD;
* cross-platform determinism;
* recording/replay;
* a data-oriented C17 API.

It uses the MIT license.

Those properties align very well with Gargantuan's desired rigid-body architecture.

Particularly valuable are:

* data-oriented handles rather than exposed C++ object ownership;
* deterministic behavior;
* explicit movement/contact events;
* straightforward integration;
* strong focus on game physics.

### Main risk

The current 3D Box3D implementation is extremely new.

The first official release, `v0.1.0`, shipped June 30, 2026, and Erin Catto explicitly describes it as **alpha software**.

That creates risk around:

* API churn;
* edge cases;
* missing functionality;
* long-term deformable support;
* production experience.

### Deformation fit

Box3D currently presents itself primarily as a rigid-body/collision system and does not provide the deformable feature set Gargantuan ultimately wants.

This does **not** automatically disqualify it.

Box3D could remain an excellent rigid backend while Gargantuan owns deformation separately.

### Overall

**Excellent conceptual fit for rigid physics, but currently high maturity risk.**

---

# 6. Jolt Physics

## 6.1 Rigid-body strengths

Jolt is explicitly designed as a multithread-friendly rigid-body and collision library for games and VR.

Its design emphasizes concurrent interaction with the physics world, including performing collision queries and preparing/loading physics objects concurrently with simulation activity. It is currently used in production projects including *Horizon Forbidden West* and *Death Stranding 2*.

Jolt uses the MIT license.

Relative to Box3D, Jolt currently has a stronger production track record.

---

## 6.2 Soft-body functionality

Jolt also includes a soft-body system.

Its representation supports:

* particle vertices;
* edge/spring constraints;
* dihedral bend constraints;
* tetrahedral volume constraints;
* skinned constraints;
* long-range attachments/tethers;
* rod stretch/shear;
* rod bend/twist.

That makes it useful for experimentation with:

* cloth;
* soft balls;
* rubber-like objects;
* ropes/rods;
* skinned secondary deformation.

Jolt's tetrahedral volume constraints explicitly preserve the rest volume of tetrahedra and expose compliance as inverse stiffness.

### Limitation

The documentation explicitly labels soft bodies as **work in progress**.

Current documented limitations include:

* no soft-body versus soft-body collision;
* several body-level impulse/velocity APIs do not apply normally because velocity is stored per particle;
* ordinary constraints cannot directly operate on soft bodies;
* buoyancy is not implemented.

Therefore Gargantuan should not make Jolt's exact soft-body API its own permanent deformation model.

### Overall

**Probably the strongest current candidate if Gargantuan wants a mature rigid backend plus immediate access to experimental deformable capabilities.**

---

# 7. NVIDIA PhysX

PhysX represents the broad-feature option.

Current PhysX supports:

* CPU rigid bodies;
* multithreaded simulation;
* GPU rigid bodies;
* FEM deformable volumes;
* deformable surfaces;
* PBD particle systems;
* additional destruction/flow-related SDKs in the broader PhysX repository.

The core SDK is BSD 3-Clause licensed.

---

## 7.1 Deformable volumes

PhysX deformable volumes are particularly relevant to the long-term Gargantuan concept.

PhysX separates:

* a lower-resolution **simulation tetrahedral mesh**;
* a **collision tetrahedral mesh**.

The simulation mesh only needs to approximate the object's overall shape and should use the lowest resolution that still produces acceptable deformation.

That is exactly the architectural principle Gargantuan should borrow even if it never uses PhysX:

> **The visible mesh should not be the simulation mesh.**

PhysX supports deformable volumes intended for objects such as soft rubber, while surface deformables target cloth, sheets, and shells.

---

## 7.2 Major cross-platform problem

PhysX deformable surfaces and volumes currently require GPU simulation implemented through CUDA.

CPU simulation of the deformable system is not supported.

That would effectively make first-class Gargantuan deformables dependent on compatible NVIDIA hardware.

That is unsuitable as the core semantics of a general-purpose engine/platform.

PhysX can still be:

* a research reference;
* a benchmark target;
* a future optional hardware-accelerated backend;
* a useful source of architectural ideas.

But Gargantuan should not define `DeformableBody` as "whatever CUDA PhysX supports."

---

## 7.3 PhysX particle systems

PhysX also provides PBD particle systems for dynamics including particles, fluids, and deformable objects, but those systems likewise require CUDA-capable GPU execution.

This reinforces the recommendation that Gargantuan's ordinary visual particle system should be renderer-owned and backend-independent.

---

## 7.4 Determinism consideration

PhysX documents deterministic rigid simulation under controlled conditions on a given platform/build, but does not guarantee determinism for scenes containing deformable surfaces, deformable volumes, or PBD particle systems.

That would complicate authoritative multiplayer deformation if exact solver replication were expected.

---

# 8. Bullet Physics

Bullet remains worth understanding because it includes a broad physics feature set and has historically supported both rigid and deformable simulation.

Bullet 2.89 introduced FEM-based volumetric deformable objects and cloth with two-way coupling to rigid/multibody simulation.

The current repository still contains its dedicated `BulletSoftBody` implementation.

However, Bullet's own historical maintainer comments describe parts of the deformable/PyBullet soft-body stack as experimental and indicate that development resources have primarily focused on rigid bodies, multibodies, collision detection, and robotics.

### Overall

Bullet is useful as:

* an implementation reference;
* a possible benchmark;
* evidence that integrated rigid/deformable simulation is practical.

It is **not currently the preferred Gargantuan backend candidate**.

---

# 9. Backend Comparison

| Property                            | Box3D 3D                    | Jolt                 | PhysX                       | Bullet                           |
| ----------------------------------- | --------------------------- | -------------------- | --------------------------- | -------------------------------- |
| Rigid bodies                        | Strong                      | Strong               | Strong                      | Strong                           |
| Production maturity                 | Low/alpha                   | High                 | High                        | High                             |
| Multithreading                      | Yes                         | Strong focus         | Yes                         | Yes                              |
| Queries                             | Strong                      | Strong               | Strong                      | Strong                           |
| Constraints                         | Good                        | Extensive            | Extensive                   | Extensive                        |
| Character support                   | Yes                         | Yes                  | Yes                         | Possible                         |
| Cloth                               | No first-class current path | Soft body            | FEM surface                 | Yes                              |
| Rubber/volume deformation           | No                          | Constraint soft body | FEM volume                  | FEM/soft                         |
| Plastic deformation                 | Custom                      | Custom               | Custom/material work likely | Custom                           |
| GPU deformables                     | No                          | No required          | CUDA only                   | No required                      |
| Cross-platform advanced deformables | Custom                      | Yes, with limits     | No                          | Yes                              |
| License                             | MIT                         | MIT                  | BSD-3-Clause                | permissive/zlib-style ecosystem  |
| Main concern                        | Very new                    | Soft bodies WIP      | CUDA dependency/complexity  | aging/deformable support quality |

---

# 10. Recommended Architectural Model

Gargantuan should not expose:

```text
Box3DBody
JoltBody
PhysXActor
```

It should expose engine semantics.

A reasonable future split is:

```text
PhysicsWorld
├── RigidSimulation
├── CollisionQueries
├── ConstraintSimulation
└── DeformationCoordinator
```

The deformable system should communicate with rigid physics through engine-owned data such as:

```text
ContactPoint
ContactNormal
Impulse
RelativeVelocity
Material
ObjectId
```

rather than exposing solver-native contact structures.

---

# 11. Proposed Deformation Capability Model

## 11.1 Rigid

Default.

No deformation allocation or mutable render geometry.

---

## 11.2 Visual deformation

Low-cost appearance-only deformation.

Examples:

* tire contact flattening;
* character secondary deformation;
* subtle rubber compression;
* hit reactions;
* cheap vehicle panel flex.

Physics remains a normal rigid body.

Conceptually:

```text
Rigid collider
      │
      └── deformation signals
                 │
                 ▼
          visual deformation
```

This could be GPU driven and therefore extremely cheap relative to true deformable simulation.

---

## 11.3 Plastic deformation

This should be considered a **high-priority advanced feature** because its performance/capability ratio is excellent.

Target examples:

* vehicle dents;
* bent metal;
* crushed barrels;
* damaged armor;
* bent structural props.

Unlike elastic deformation, plastic deformation does not necessarily require continuous simulation.

Possible model:

```text
Rigid collision
     │
     ▼
Impact exceeds yield threshold
     │
     ▼
Modify sparse deformation state
     │
     ├── visual geometry updates
     └── optional coarse collision update
```

Once deformation is applied, the expensive calculation can stop.

This makes plastic deformation potentially much cheaper than continuously simulated rubber bodies.

---

## 11.4 Elastic / rubber deformation

Rubber-like objects require continuous internal degrees of freedom.

Examples:

* rubber balls;
* flexible tires;
* foam;
* flexible props.

A likely implementation would use a coarse deformation representation:

```text
Render mesh
40,000 vertices

      skinned to

Deformation lattice
64–256 nodes
```

Only the sparse nodes require physical simulation.

The visible mesh is interpolated/skinned against them.

This follows the same broad performance principle PhysX recommends by keeping simulation meshes much lower resolution than detailed geometry.

---

## 11.5 Cloth

Cloth should remain a distinct surface-deformation problem.

It should not require the same implementation as volumetric rubber bodies.

Possible shared concepts include:

* deformation nodes;
* constraints;
* attachments;
* collision interaction;
* GPU render skinning.

But the solver requirements differ enough that `Cloth` should be allowed to evolve independently.

---

# 12. Lessons from Virt-A-Mate

Virt-A-Mate is useful as evidence for a broader principle:

> **High-detail visible deformation does not require equally high-detail physics simulation.**

VaM's official documentation describes soft physics as localized to selected character regions, with multiple colliders attached to skin vertices rather than the entire visible body being represented as fully physical geometry.

The VaM ecosystem also combines sparse physics with morph-driven visual deformation. For example, TittyMagic derives morph behavior from the average positions of soft-physics joint rigid bodies attached to skin vertices.

VaM additionally performs Laplacian mesh smoothing after skinning, allowing relatively coarse deformation control to produce smoother visible surfaces.

This does not make VaM's physics cheap. Its own performance documentation calls physics its most performance-demanding feature and allows soft-body physics to be disabled in lower-quality presets.

The lesson for Gargantuan is therefore not:

> "VaM has cheap soft bodies."

The useful lesson is:

> **Use sparse physically meaningful controls and let cheaper rendering/deformation systems create the high-resolution appearance.**

---

# 13. Proposed Sparse Deformation Representation

A future MeshPart could conceptually contain:

```text
MeshPart
├── MeshAsset
├── RigidPhysicsRepresentation
├── optional DeformationState
│
└── optional DeformationAsset
     ├── Rest nodes
     ├── Skin weights
     ├── Material regions
     └── Quality metadata
```

For example:

```text
Visible mesh:
    40,000 vertices

Deformation cage:
    80 nodes

Collision representation:
    12–40 primitives / hulls / coarse regions
```

The render mesh follows the deformation cage using precomputed weights.

This allows:

```text
80 physics nodes
      ↓
GPU interpolation/skinning
      ↓
40,000 visible vertices
```

rather than solving 40,000 physical vertices.

---

# 14. Plastic + Elastic Material Model

The same engine-facing material description could potentially support both temporary and permanent deformation.

Possible conceptual parameters:

```text
Elasticity
Stiffness
Damping

YieldStrength
Plasticity
MaximumDeformation
Recovery

DeformationRadius
DeformationQuality
```

Behavior:

```text
Force below yield point
        ↓
temporary elastic deformation
        ↓
returns toward rest shape

Force above yield point
        ↓
elastic response
        +
rest-shape modification
        ↓
permanent dent/bend
```

This would give developers intuitive control without exposing FEM parameters directly.

Internally, more advanced material parameters could exist later.

---

# 15. VFX Particles Must Remain Separate

"Particle support" needs two definitions.

## VFX particles

Examples:

* smoke;
* sparks;
* dust;
* fire;
* rain;
* cosmetic debris.

These should primarily live in the renderer/GPU simulation system.

Creating rigid physics bodies for every spark is inappropriate.

Possible architecture:

```text
ParticleEmitter
      │
      ▼
Particle simulation description
      │
      ▼
GPU compute/update
      │
      ▼
Renderer
```

Optional collision quality could eventually include:

```text
None
Depth
ApproximateScene
PhysicsQuery
```

---

## Physical particle simulation

Examples:

* sand;
* granular material;
* potentially fluids much later.

This is a different feature and can remain future work.

Gargantuan does not currently need fluids as a platform requirement.

---

# 16. Studio Deformation Authoring

Studio should eventually expose deformation as both a runtime capability and an authoring tool.

A developer should be able to:

```text
Original Mesh
     │
     ▼
Deform manually or through physics
     │
     ▼
Current DeformationState
     │
     ├── Reset
     ├── Save editable deformation
     ├── Set current shape as rest shape
     └── Bake to new MeshAsset
```

---

## 16.1 Save editable deformation

This preserves:

```text
Base mesh
+
deformation cage/lattice
+
node offsets
+
material/deformation configuration
```

The developer can reopen Studio and continue editing.

---

## 16.2 Bake to mesh

`Bake Deformation` should convert the current visible shape into permanent mesh geometry.

The resulting object becomes an ordinary mesh and no longer requires deformation simulation unless re-enabled.

Use cases include:

* wrecked vehicle variants;
* bent pipes;
* damaged walls;
* crushed props;
* permanently posed cloth;
* procedural environmental variation.

This provides a strong optimization path:

```text
Expensive simulation during authoring
             ↓
          Bake
             ↓
Zero deformation-solver cost at runtime
```

---

## 16.3 Set current shape as rest shape

This is distinct from baking.

The object remains deformable, but its current shape becomes the new undeformed state.

Example:

```text
straight beam
    ↓
bend in Studio
    ↓
Set Current As Rest Shape
    ↓
starts bent at runtime
and can deform further
```

This should be particularly useful for plastic deformation authoring.

---

## 16.4 Collision handling when baking

Studio should not automatically convert a dense render mesh into equally dense dynamic collision geometry.

A bake operation should eventually allow:

```text
Render Geometry:
    Bake current shape

Collision:
    Keep existing proxy
    Regenerate simplified proxy
    Regenerate convex decomposition
    Preserve deformable proxy
```

Collision simplification should be treated as an asset-cooking step.

---

# 17. Authoring Tools

The same deformation representation could support Studio tools such as:

```text
Push
Pull
Dent
Smooth
Flatten
Bend
Twist
Inflate
Relax
```

The critical architectural rule is that these tools should modify the same engine-owned `DeformationState` used by runtime deformation.

Avoid creating a completely separate Studio-only sculpting architecture unless later requirements justify it.

---

# 18. Rendering Requirements

Current render architecture must not assume every renderable object can always be described solely as:

```text
MeshId + Transform
```

Normal rigid meshes can.

Deformable objects eventually require:

```text
MeshId
Transform
DeformationState / DeformationBuffer
```

A future render object may conceptually resemble:

```text
RenderObject
{
    ObjectId
    MeshId
    Transform

    optional DeformationBuffer
}
```

The deformation buffer could eventually represent:

* deformation-cage nodes;
* skinned simulation nodes;
* per-vertex deltas;
* morph weights;
* GPU deformation parameters.

The renderer should not need to know which physics solver generated them.

---

# 19. Persistence

Deformation should have explicit persistence semantics.

Potential states:

```text
Transient
    deformation disappears when simulation resets

Saved
    deformation state persists with project/runtime state

Baked
    deformation is now ordinary mesh geometry
```

For persistent plastic damage, serialization should store the **compact deformation representation**, not necessarily every render vertex.

For example:

```text
DeformationAssetId
+
node offsets
+
changed rest positions
```

could reconstruct the visible shape.

---

# 20. Networking

Network design must assume deformable geometry is too expensive to replicate vertex-by-vertex.

Do not design:

```text
50,000 vertices
×
60 Hz
×
every deformable object
```

Possible future strategies include:

### Sparse state replication

```text
64 deformation nodes
       ↓
compressed node updates
       ↓
client renders detailed mesh
```

### Impact/event replication

```text
Impact {
    ObjectId
    Position
    Direction
    Impulse
    Radius
}
```

Clients reproduce the deformation locally.

Periodic authoritative checkpoints correct drift.

### Hybrid

Likely most practical:

```text
reliable deformation event
+
local simulation
+
occasional authoritative sparse state
```

Exact design should wait for networking architecture and solver determinism research.

---

# 21. Simulation LOD

Deformation should support aggressive level-of-detail policies.

Example:

```text
Near + active:
    full deformation

Mid distance:
    reduced deformation nodes
    fewer iterations

Far:
    frozen deformation state

Very far:
    rigid approximation only
```

The engine could additionally prioritize:

* locally controlled objects;
* important gameplay objects;
* visible objects;
* recently impacted objects.

Sleeping deformables should become cheap.

---

# 22. Quality/Budget Model

Possible future developer-facing concept:

```luau
part.DeformationQuality = Enum.DeformationQuality.Automatic
```

Internal profiles might control:

| Quality | Approx. simulation complexity |
| ------- | ----------------------------: |
| Low     |                   16–32 nodes |
| Medium  |                   32–96 nodes |
| High    |                  96–256 nodes |
| Ultra   |          explicitly expensive |

Exact numbers must come from benchmarks, not this document.

The important design principle is that **quality is budgetable**.

---

# 23. What Gargantuan Should Not Do

Avoid the following architectural traps.

## Do not make render vertices physics particles

```text
RenderMesh == PhysicsMesh
```

should not be the default model.

---

## Do not expose backend-native handles

Avoid public APIs such as:

```text
GetJoltBody()
GetBox3DBody()
PxActor
b3BodyId
```

---

## Do not make deformation mandatory infrastructure for every Part

Rigid objects must remain cheap.

---

## Do not require CUDA/NVIDIA hardware for engine-level deformation semantics

Hardware-specific acceleration may exist later as an optimization/backend.

It should not define whether a Gargantuan experience can function.

---

## Do not couple VFX particles to rigid physics

A visual ParticleEmitter and physical granular simulation are different systems.

---

## Do not require high-resolution collision geometry to follow every visual dent

Allow approximation.

---

# 24. Benchmark Requirements

Before selecting the long-term rigid backend, implement comparable prototypes behind the new boundary.

At minimum test Box3D and Jolt.

## Rigid-body tests

### Body count

```text
100
1,000
10,000
50,000 where practical
```

Measure:

* creation/destruction;
* idle/sleeping cost;
* active cost;
* memory;
* worker scaling.

### Stacking

Large box stacks and piles.

Measure stability and convergence.

### High-speed collision

CCD stress tests.

### Constraints

Test:

* weld;
* hinge;
* slider;
* motors;
* springs;
* large connected assemblies.

### Queries

Measure concurrent:

* raycasts;
* shape casts;
* overlaps.

### Lifecycle churn

Rapid:

```text
create
parent
unparent
destroy
recreate
```

must not expose stale engine IDs.

### Character movement

Prototype whichever character-controller abstraction the backend provides.

---

# 25. Deformation Experiments

Once the rigid boundary is stable, run independent deformation prototypes.

## Prototype A — visual deformation

Goal:

Demonstrate a rigid collider driving a GPU or CPU deformation cage with no deformable physics.

Success:

* near-zero additional physics cost;
* visible tire compression/dent effects;
* mutable render geometry architecture validated.

---

## Prototype B — plastic denting

Goal:

Event-driven persistent deformation from rigid collision impulses.

Test:

* metal panel;
* car door;
* barrel.

Success:

* localized permanent deformation;
* coarse collision update optional;
* no continuous solver cost after deformation settles.

---

## Prototype C — elastic lattice

Goal:

64–128-node rubber object.

Evaluate:

* Jolt soft-body implementation;
* simple Gargantuan/custom XPBD-style prototype if appropriate.

Measure:

* solver time;
* iteration sensitivity;
* collision quality;
* render skinning cost.

---

## Prototype D — cloth

Small cloth patch attached to rigid points.

This primarily validates:

* deformable attachments;
* collision handoff;
* rendering;
* future clothing/flags/capes.

---

## Prototype E — Studio bake

Take a deformed object and:

```text
Save DeformationState
Bake New Mesh
Set Current As Rest Shape
Reset
Undo
Redo
```

Validate that the asset architecture works independently of runtime solver choice.

---

# 26. Recommended Direction

## Phase 1 — Fix the physics boundary

Do this before expanding physics.

Create engine-owned abstractions for:

* world;
* bodies;
* shapes;
* constraints;
* queries;
* contacts;
* movement events;
* body lifecycle.

Move Box3D translation code behind that boundary.

No public engine class should need Box3D types.

---

## Phase 2 — Keep Box3D working as the reference implementation

Do not throw away a functioning backend just because alternatives exist.

Box3D becomes the regression baseline.

---

## Phase 3 — Implement a narrow Jolt spike

Only enough to test:

* rigid Parts;
* basic constraints;
* contacts;
* raycasts;
* destruction;
* character movement;
* multithreading.

Then benchmark against Box3D.

### Likely decision today

If selecting from scratch today, **Jolt currently has the maturity advantage**.

Box3D has extremely appealing architecture and deterministic/data-oriented properties, but its 3D implementation remains explicitly alpha.

No decision should be made exclusively from feature lists.

---

## Phase 4 — Keep deformation engine-owned

Even if Jolt is selected, do not define:

```text
Gargantuan Deformation == Jolt SoftBody
```

Instead:

```text
Gargantuan Deformation
        │
        ├── Jolt implementation where appropriate
        ├── custom plastic deformation
        ├── renderer visual deformation
        └── future specialized solver
```

This allows the best implementation technique to differ by feature.

---

# 27. Likely Feature Priority

Recommended ordering:

1. **Rigid physics backend boundary**
2. **Backend comparison: Box3D vs Jolt**
3. **Excellent collision/query API**
4. **Assemblies and constraints**
5. **Character movement**
6. **Network-ownership-compatible physics semantics**
7. **Deformation data model**
8. **Visual deformation**
9. **Plastic deformation**
10. **Studio deformation authoring/baking**
11. **Cloth**
12. **Elastic/rubber bodies**
13. **Advanced physical particle simulation**
14. **Fluids only if an actual product need appears**

This intentionally places **plastic deformation before full rubber-body simulation**.

Plastic deformation offers a much better capability/performance ratio and forces Gargantuan to solve the important shared infrastructure:

* mutable geometry;
* deformation state;
* asset representation;
* render extraction;
* persistence;
* Studio editing;
* networking representation.

without immediately requiring an expensive continuous deformable solver.

---

# 28. Proposed Design Philosophy

Gargantuan should aim to be more capable than platforms that expose only rigid-body physics, but should not confuse sophistication with brute-force simulation.

The preferred approach is:

> **Use rigid physics wherever rigid physics works.**

> **Use sparse deformation where deformation adds meaningful value.**

> **Use visual approximation where physical accuracy is unnecessary.**

> **Allow expensive high-fidelity simulation when the developer explicitly decides it is worth the budget.**

The goal is not:

> everything is physically simulated.

The goal is:

> the engine gives developers a useful progression from cheap approximation to real simulation, without forcing them outside the engine when a rigid-body model is insufficient.

---

# 29. Decision Questions for the Design Phase

After the physics boundary is fixed, the design agent should answer:

1. What is the exact engine-owned rigid physics interface?

2. Can both Box3D and Jolt implement that interface without backend-specific compromises?

3. Which operations must remain synchronous versus queued?

4. How should physics identities relate to `ObjectId`?

5. Should deformables share `PhysicsBodyId` with rigid objects or use a separate identity domain?

6. What is the smallest useful `DeformationState` representation?

7. Should plastic deformation modify:

   * node positions;
   * node rest positions;
   * morph weights;
   * or a combination?

8. How should rigid collision impulses feed deformation?

9. When does a deformed collider recook?

10. What collision approximation modes are required?

11. What deformation state belongs in snapshots/journals?

12. What deformation state is replicated?

13. What is deterministic enough to reproduce from deformation events?

14. How should Studio transactions represent large deformation edits?

15. How should deformation assets reference their source mesh?

16. How does `SetCurrentAsRestShape` affect serialization and undo?

17. When should the engine freeze or reduce deformation simulation?

18. What budgets should be globally enforced?

19. Does cloth belong in the same solver abstraction as elastic volumes?

20. Which capabilities must work identically on:

    * Windows;
    * Linux;
    * AMD GPUs;
    * NVIDIA GPUs;
    * headless servers?

---

# 30. Current Recommendation

**Do not select the final deformation solver yet.**

First establish a strict Gargantuan-owned physics boundary.

After that:

* preserve Box3D as one implementation;
* prototype Jolt;
* benchmark both for Gargantuan's actual workloads;
* treat PhysX primarily as a reference for deformable architecture and performance strategy;
* keep plastic deformation and visual deformation engine-owned rather than backend-defined;
* keep VFX particles in the rendering architecture;
* design deformation geometry around sparse simulation/control data rather than dense render meshes;
* make high-cost deformation explicitly optional and budgeted;
* build Studio baking/rest-shape workflows into the eventual deformation design.

The core principle should remain:

> **Performance is a hard requirement, but performance alone is not a reason to omit a feature that produces significant developer value. The correct response to an expensive feature is to isolate its cost, provide cheaper fallbacks, expose budgets, and let developers choose when the result is worth paying for.**
