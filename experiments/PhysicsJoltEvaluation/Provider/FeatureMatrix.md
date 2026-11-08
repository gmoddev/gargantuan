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
| Capsule mover and canonical static mesh | `CapsuleAndMeshResidency` checks collision, ray hits, resource sharing/charges, atomic transition rejection and release; `DataModelMixedGameplay` imports an actual OBJ through AssetService, queries the authored MeshPart, and releases its residency | Mesh workload here is a small bounded floor, not large scene streaming |
| Weld, connected collision policy, distance spring, articulated chain, authored hinge/slider motors, moving-body CCD | Dedicated provider fixtures and `DataModelMotorsAndCcd`; the baseline 13 groups passed on the Windows Engine worker at `93d2139b5` | Bounds and solver response are fixture specific |
| Rigid force cadence | `ForceAcrossHalfUpdates` compares a no-contact force/mass trajectory after both native half-frame updates | Other force/contact combinations need workload measurement |
| Cloth/rubber and rigid primitives | `ClothAndRubberCoupling` and authored lifecycle/streaming fixtures cover admission, deformation publication, pins, and selected collision response; `DataModelMixedGameplay` measures 0.086 m peak cloth deflection, striker reversal, and a rubber-driven target moving 0.59 m | These are bounded contacts with 9×9 cloth and 5³ rubber, not 4K/16K contact quality |
| Save/reopen and semantic identities | `DataModelAdmissionAndStreaming` saves a Part/Cloth subtree, reopens, readmits, streams out/in, and rejects stale IDs | Save format intentionally excludes native IDs |
| Dynamic rigid-soft attachment | Optional `PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS` composes the separate native helper behind neutral IDs; the Windows worker observed 480 applied native steps, 0.162 peak impulse, linked rigid remaining near x=1 while an unlinked control reached x=7, then auto-detach and generation churn | Macro-off build has no dynamic attachment bridge; Linux/ASAN and broader attachment geometry remain to be qualified |
| Soft damping and point impulse | `NeutralSoftDampingRetention` checks free-velocity frame retention; `NeutralPointImpulseDistribution` checks four-nearest-node distribution | Forced trajectories and rubber shape matching are not numerically identical to XPBD |
| Large soft workloads | `ProviderBenchmark` has 4K/16K cloth and rubber plus four 4K cloth bodies; `EngineBenchmark` times authored `WorldRoot::StepPhysics` | A performance winner requires matched binary/host/budget, measured quality and reviewed absolute ranges; coupled and rubber winners remain withheld |

`DataModelMixedGameplay` passed once in the hosted Windows Engine worker using
the opt-in attachment Jolt branch. Its two-box stack settled with lower center
y=0.5 and upper center y=1.5 on the imported floor; the real OBJ ray and
WorldRoot capsule wall check also passed. These observations establish bounded
behavior, not a large-world or long-session error envelope. The Jolt adapter
does not implement all of XPBD's global rubber shape recovery: Jolt adds local
cell diagonals, changing constraint count and response. Jolt 5.6 also does not
support soft-body versus soft-body contacts; those pairs are explicitly outside
the provider's admitted collision behavior. Static triangle meshes remain
anchored only; unanchored `MeshPart` continues through WorldRoot's inherited
box fallback.
