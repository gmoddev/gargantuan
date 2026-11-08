# Experimental shared Jolt provider: feature evidence

This provider is injected only by the isolated Physics Jolt evaluation targets.
The production Box3D/XPBD factories and WorldRoot default remain unchanged.
The shared Jolt world is owned by the rigid adapter; the soft adapter extracts
state from that same world after the qualified two half-frame updates with
eight collision steps and 12 soft integrations per step. The table describes
what the neutral Engine interfaces and current fixtures actually exercise.

| Behavior | Current evidence | Limit |
| --- | --- | --- |
| Rigid identity, shape replacement, mass, contact begin/end, sensors | `RigidIdentityAndEvents` physically checks state, masses, events, and retired generation | No broad game workload characterization yet |
| Capsule mover and canonical static mesh | `CapsuleAndMeshResidency` checks collision, ray hits, resource sharing/charges, atomic transition rejection and release | Imported `MeshPart` path is separately exercised by `DataModelMixedGameplay`; its hosted result is pending |
| Weld, connected collision policy, distance spring, articulated chain, authored hinge/slider motors, moving-body CCD | Dedicated provider fixtures and `DataModelMotorsAndCcd`; the baseline 13 groups passed on the Windows Engine worker at `93d2139b5` | Bounds and solver response are fixture specific |
| Rigid force cadence | `ForceAcrossHalfUpdates` compares a no-contact force/mass trajectory after both native half-frame updates | Other force/contact combinations need workload measurement |
| Cloth/rubber and rigid primitives | `ClothAndRubberCoupling` and authored lifecycle/streaming fixtures cover admission, deformation publication, pins, and selected collision response | `DataModelMixedGameplay` adds authored dynamic strikes and pushes; hosted result is pending |
| Save/reopen and semantic identities | `DataModelAdmissionAndStreaming` saves a Part/Cloth subtree, reopens, readmits, streams out/in, and rejects stale IDs | Save format intentionally excludes native IDs |
| Dynamic rigid-soft attachment | Optional `PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS` composes the separate native helper behind neutral IDs; fixture checks reciprocal impulse, unlinked control, auto-detach and generation churn | Macro-off build has no dynamic attachment bridge; macro-on hosted rerun of the strengthened fixture is pending |
| Soft damping and point impulse | `NeutralSoftDampingRetention` checks free-velocity frame retention; `NeutralPointImpulseDistribution` checks four-nearest-node distribution | Forced trajectories and rubber shape matching are not numerically identical to XPBD |
| Large soft workloads | `ProviderBenchmark` has 4K/16K cloth and rubber plus four 4K cloth bodies; `EngineBenchmark` times authored `WorldRoot::StepPhysics` | A performance winner requires matched binary/host/budget, measured quality and reviewed absolute ranges; coupled and rubber winners remain withheld |

`DataModelMixedGameplay` is a new discriminating fixture. Its real asset import,
stack, character capsule, cloth strike, and rubber push observations should be
reported as qualified only after the hosted target completes. The Jolt adapter
does not implement all of XPBD's global rubber shape recovery: Jolt adds local
cell diagonals, changing constraint count and response. Jolt 5.6 also does not
support soft-body versus soft-body contacts; those pairs are explicitly outside
the provider's admitted collision behavior. Static triangle meshes remain
anchored only; unanchored `MeshPart` continues through WorldRoot's inherited
box fallback.
