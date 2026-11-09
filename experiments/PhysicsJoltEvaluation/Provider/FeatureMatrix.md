# Experimental shared Jolt provider: feature evidence

The [final performance campaign](../FinalPerformanceCampaign.md) qualified Engine
`ea229c569` / native runtime `7e59df5f` on Windows and Linux ASan/UBSan, including
three provider/cache/scheduling repeats and forty-frame large-cloth checks at
caps 1/8. Published fork checkpoint `0fbdedcc` has identical runtime/test/build
trees. Its optional 1024 grouping is bounded by the report's measured domain;
the default upstream-compatible grouping remains 256. The historical numeric
observations below retain their original source attribution.

This provider is injected only by the isolated Physics Jolt evaluation targets.
The production Box3D/XPBD factories and WorldRoot default remain unchanged.
The shared Jolt world is owned by the rigid adapter; the soft adapter extracts
state from that same world after two half-frame updates with eight collision
steps per update. The baseline profile uses 12 soft integrations and one
constraint sweep per collision step. The opt-in convergence candidate uses
five integrations and two sweeps. Hosted Engine `b2df82ae5` with Jolt
`d8c46588` passed the candidate's physical fixtures in three fresh repeats
on Windows and Linux ASan/UBSan. The table describes what
the neutral Engine interfaces and current fixtures actually exercise.

| Behavior | Current evidence | Limit |
| --- | --- | --- |
| Rigid identity, shape replacement, mass, contact begin/end, sensors | `RigidIdentityAndEvents` physically checks state, masses, events, and retired generation | No broad game workload characterization yet |
| Capsule mover and canonical static mesh | `CapsuleAndMeshResidency` checks collision, ray hits, resource sharing/charges, atomic transition rejection and release; `DataModelMixedGameplay` imports an actual OBJ through AssetService, queries the authored MeshPart, and releases its residency | Mesh workload here is a small bounded floor, not large scene streaming |
| Weld, connected collision policy, distance spring, articulated chain, authored hinge/slider motors, moving-body CCD | Dedicated provider fixtures and `DataModelMotorsAndCcd`; the full candidate qualification passed on the Windows Engine worker at `b2df82ae5` | Bounds and solver response are fixture specific |
| Rigid force cadence | `ForceAcrossHalfUpdates` compares a no-contact force/mass trajectory after both native half-frame updates | Other force/contact combinations need workload measurement |
| Cloth/rubber and rigid primitives | `ClothAndRubberCoupling` and authored lifecycle/streaming fixtures cover admission, deformation publication, pins, and selected collision response; the latest `DataModelMixedGameplay` observes 0.052 m peak cloth deflection, striker reversal, and a rubber-driven target at x=20.88 | These are bounded contacts with 9×9 cloth and 5³ rubber, not 4K/16K contact quality |
| Save/reopen and semantic identities | `DataModelAdmissionAndStreaming` saves a Part/Cloth subtree, reopens, readmits, streams out/in, and rejects stale IDs | Save format intentionally excludes native IDs |
| Dynamic rigid-soft attachment | Optional `PHYSICS_EVALUATION_GANTRIA_ATTACHMENTS` composes the separate native helper behind neutral IDs; the Windows worker observed 480 applied native steps, 0.162 peak impulse, linked rigid remaining near x=1 while an unlinked control reached x=7, then auto-detach and generation churn. The bounded fixture passed in hosted Windows and Linux ASan/UBSan repeats | Macro-off build has no dynamic attachment bridge; broader attachment geometry remains to be qualified |
| Soft damping and point impulse | `NeutralSoftDampingRetention` checks free-velocity frame retention; `NeutralPointImpulseDistribution` checks four-nearest-node distribution | Forced trajectories and rubber shape matching are not numerically identical to XPBD |
| Native soft-rigid contact evidence | A private opt-in Jolt manifold listener records actual contacting vertices by neutral-ID pair; the hosted Windows projectile fixture observed 13 contacting vertices and the bounded fixture passed on Linux ASan/UBSan | The final 16K driven-cloth stress fails the unchanged strain bound in both B/C and is excluded from accepted contact quality; AABB overlap alone is excluded |
| Rigid sleep clock and neutral contact continuity | The isolated Jolt branch `d8c46588` passed native Windows/Linux/sanitizer CI; the Engine provider fixture observes one begin, no end through five sleeping frames, and one end after departure at C1/C4/C8/U2C8. Same-plane box-to-mesh handoff preserves support and contact identity; moving the floor away wakes the ball, emits one end, and lets it fall. New static overlap wakes and separates a compatible sleeper while an incompatible sleeper remains asleep. An authored Cloth/Part attachment sleeps and reawakens from a soft impulse. These gates passed hosted Windows and Linux ASan/UBSan repeats at `b2df82ae5` | Comparative cost remains scene-specific; the legacy provider still uses Jolt's default sleep clock. Broadphase AABB wake can conservatively activate compatible sleepers inside sparse mesh bounds |
| Large soft workloads | `ProviderBenchmark` has 4K/16K cloth and rubber plus four 4K cloth bodies; `EngineBenchmark` times authored `WorldRoot::StepPhysics` | A performance winner requires matched binary/host/budget, measured quality and reviewed absolute ranges; coupled and rubber winners remain withheld |

`DataModelMixedGameplay` passed in the Windows Engine candidate worker and
hosted Windows/Linux repeats using
the opt-in attachment, convergence, and sleep-clock Jolt branch. Its two-box stack settled with lower center
y=0.5 and upper center y=1.5 on the imported floor; the real OBJ ray and
WorldRoot capsule wall check also passed. These observations establish bounded
behavior, not a large-world or long-session error envelope. The Jolt adapter
does not implement all of XPBD's global rubber shape recovery: Jolt adds local
cell diagonals, changing constraint count and response. Jolt 5.6 also does not
support soft-body versus soft-body contacts; those pairs are explicitly outside
the provider's admitted collision behavior. Static triangle meshes remain
anchored only; unanchored `MeshPart` continues through WorldRoot's inherited
box fallback.
