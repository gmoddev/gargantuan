# Gameplay Foundation 7

## Phase 0 inventory

The starting source is `e7bec187def55e79c6ca956b17feb82926e5d1fe`, the merge of
PR #26 on remote `main`. Its [native CI](https://github.com/gmoddev/gargantuan/actions/runs/43870187334)
passed Windows/MSVC Release and Linux/Clang ASan/UBSan. Its
[GNS sanitizer run](https://github.com/gmoddev/gargantuan/actions/runs/43870187327)
and [documentation run](https://github.com/gmoddev/gargantuan/actions/runs/43870187304)
also passed. The open PR #27 head is outside the baseline's ancestry. These
checks qualify the starting source only.

| Area | Status at the baseline | Evidence and first action |
| --- | --- | --- |
| Player identity, spawn and lifecycle | Usable | `Players`, `Player`, `GameSession`, `PlayerRuntimeTests` and `GameSessionTests` cover runtime identities, trusted LocalPlayer assignment, creation, disconnect and Character lifecycle. Reuse these lifetimes. |
| Generic possession | Missing | `Player.Character` owns teardown and accepts `Character` only. Add a non-owning server-authorized relation to an eligible same-Workspace Instance. |
| Transforms, rigid bodies and queries | Usable | `BasePart` exposes pose, linear and angular velocity, force, torque, impulses, contacts, collision filters and constraints. `WorldRoot` exposes bounded casts, overlaps and a capsule sweep. `PhysicsBackendTests` covers the selected Box3D rigid provider and Luau boundary. XPBD is the separate deformable implementation. |
| Simulation ownership | Requires correction | `Engine::Step` owns PreSimulation, the native physics step, PostSimulation and animation publication. The public `WorldRoot:StepPhysics` binding also permits caller-driven stepping. Preserve the public observation events and put advancement under native ownership. |
| Input and semantic actions | Usable locally, partial online | `ActionMap`, InputContext, InputAction and InputBinding already handle transitions, analog values and context priority. `CharacterControlService` exposes bounded movement and registered actions, but its callbacks and live network targets depend on `KinematicCharacter`. |
| Authoritative networking | Usable for stock Characters | GCHR and `GameSession` already implement per-owner control epochs, input sequencing and bounds, acknowledgments, correction, prediction policy, relevance and interpolation. `CharacterNetworkingTests`, `CharacterNetworkGnsTests` and `GameSessionGnsTests` qualify that lane. Generic possession needs bounded input and authoritative pose publication scoped to its own control lifetime. |
| General realtime motion | Native pieces only | `WorldRoot` publishes dynamic body readback through `BasePart::SetCFrame`, and `BasePart` has a native presentation offset used by Characters. Reuse collision and pose primitives. An opt-in generic realtime path needs immutable snapshots and a client presentation policy that does not drive physics. |
| Animation and sockets | Partial | `Animator` and `AnimationTrack` expose playback, speed, weight, position and completion on skinned meshes. `Attachment` provides world transforms. Native root-motion consumption currently targets a Character ancestor. Generic playback markers, layers and track-state replication are gaps. Existing GCHR action replication remains Character-specific. |
| Gameplay state and interactions | Partial | Instance attributes and tags provide composable data and structural replication. `ProximityPrompt` has local interaction mechanics. The source still needs game-side authoritative lifecycle, damage, cooldown, interaction eligibility and inventory ownership policy. |
| Authoring and distribution | Usable, campaign proof missing | PR #26 qualifies saved player policy, Character creator inspection and animation preview. Existing package, Play and dedicated server contracts are the entry points for a new Luau proof. |

Keep gameplay state in the authoritative DataModel and restrict new networking
to bounded Main-owned intent admission. Reuse the existing scheduler and
transport, structural materialization, physics safe points, and animation
presentation. The first custom proof can compose an anchored controlled body
with a collision query. Dynamic bodies require a separate readback and replica
physics policy before they can use the same realtime publication contract.
