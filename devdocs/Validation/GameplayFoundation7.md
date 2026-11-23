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

## Windows Release qualification

The worker matched Engine source `56364bb3954e3f6cb2480f866eb0c99b8577af85`
and built x64 Release with MSVC 14.51.36231 and pinned GNS
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. The build used eight jobs in
`C:\Sandbox\Codex\Builds\gargantuan-gameplay-7\msvc-release-gns`.
Build and test logs are in `C:\Sandbox\Codex\Logs\gargantuan-gameplay-7`
on the worker.

- The player runtime and physics backend tests passed, covering the native
  step guard, Lua possession authorization, same-entity revision changes,
  destruction, detach and configurable stock movement.
- The GPIN and GPOS tests passed. They cover replay, reordering, bad input,
  capacity, replacement generations, epoch changes and interpolation.
- The simulated and real GNS GameSession cases passed. Two clients moved
  separate parts and observed each other's authoritative pose, transferred
  possession, retired a destroyed target, and reconnected with a new Player
  identity and lease. In simulation, both controllers resumed after the server
  paused while the clients continued for 24 ticks. The Player runtime test
  denies conflicting stock and explicit ownership in both assignment orders.
  The full `GameSessionTests` suite and existing GNS GameSession case passed.
- The packaged Relay Rescue server and two headless clients started over
  GNS loopback and each exited with code zero. The project snapshot matched
  the Luau sources. Both scripts independently compiled with Lute. Packages are
  under `C:\Sandbox\Codex\Artifacts\gargantuan-gameplay-7\relay-20261123-084958`.

Worker binary SHA-256:

| Binary | SHA-256 |
| --- | --- |
| GameSession test | `311163bbf534fc91a8d0940381761a23a9ab04734b266d38695dd4f6b7d8e513` |
| Relay Rescue mission test | `6cbc551f08e33a5820a6b051792558b83cce8574fe05da237b3f7d2f415712aa` |
| Possession allocation workload | `c3b9ee6f4ec624e67b5cfcea51f095b75defb27b1abd7fe5cc77d67ef6931af7` |
| Dedicated server | `7dc9934cd0b0805918cc9f71303959a3d403b8be759ed85d163ef12d4304b777` |
| Packaged player | `fae26b3e215a261d283f9e7f75be459d8bd5860c09e92dca6fe531658e0c898a` |

## Possession workload

Run `gargantuan_gameplay_allocation_workload --generic-possession-workload`
from the Release build. Each measurement is a warmed 120-tick headless
workload with separate client Engines and a simulated transport. Every peer
in the fixture can see every body.

| Measurement | 2 peers | 8 peers |
| --- | ---: | ---: |
| Engine tick p50 / p95 / p99 | 21.1 / 27.7 / 39.4 µs | 54.2 / 60.7 / 71.2 µs |
| Session step p99 | 15.0 µs | 83.8 µs |
| Physics phase total | 339.1 µs | 807.9 µs |
| Input admission total | 34.3 µs | 104.4 µs |
| Accepted pose commit total | 260.9 µs | 990.2 µs |
| Pose payloads accepted | 160 | 2,560 |
| Pose payload bytes accepted | 9,600 | 153,600 |
| Server main-thread scalar C++ allocation bytes | 779,936 | 3,177,592 |
| Initial possession binding | 1 tick | 1 tick |
| Maximum consumed input age from admission | 1 tick | 1 tick |

Timing excludes rendering, socket transit and native GNS overhead. Payload bytes
are scheduler-admitted GPOS bytes. The allocation count covers scalar C++
allocation requests on the server Main phases. Luau, aligned allocation,
worker and provider memory have separate accounting.

## Full Relay Rescue mission

The follow-on source `fd84bb398048412593cb896a8c892d5cd7b81902` adds the
saved-project mission fixture. The first full mission run exposed a Luau
lifetime problem. Instance userdata wrappers compare equal but use raw
identity as table keys. The sample state and shipped playback and cleanup
policies now key by `PlayerId`.

The corrected mission passed in simulated transport and in real GNS loopback.
Both runs used the saved project and hydrated its client Luau into two headless
client Engines. The test places the stock Characters on the launch pad, switch
and hazard. It uses ordinary host input for flight and landing. The drone
stopped at the divider, reached the relay, and awarded three cells to the
pilot. The other client observed the completed objective. Landing restored
the stock Character. Server damage triggered death and full-health respawn.
Disconnect retired the player, possession, drone, and inventory.

| Respawn measurement | Simulated transport | GNS loopback |
| --- | ---: | ---: |
| Wall time | 1,505 ms | 1,509 ms |
| Server ticks | 48 | 84 |

The mission evidence is headless loopback. Studio graphical playback, physical
multi-host coverage, and realtime poses for dynamic bodies remain separate.

## Hosted qualification

The final runtime source is `56364bb3954e3f6cb2480f866eb0c99b8577af85`.
All three workflows passed.

- The Windows Release headless suite and all five Release workload steps
  passed. The suite exercised Relay Rescue over simulated transport and GNS.
  Linux Clang 19 ASan/UBSan passed all 116 headless tests.
  [Native Windows and Linux gates](https://github.com/gmoddev/gargantuan/actions/runs/45187948310).
- Linux Clang 19 ASan/UBSan/LSan passed the 13-test GNS contract, including
  generic possession and the saved Relay Rescue mission, plus the profiled
  and byte-admission runs.
  [GNS sanitizer gate](https://github.com/gmoddev/gargantuan/actions/runs/45187948759).
- The documentation workflow passed.
  [Documentation gate](https://github.com/gmoddev/gargantuan/actions/runs/45187949548).

The intermediate candidates retain their original runs:

| Source | Native | GNS sanitizer | Docs |
| --- | --- | --- | --- |
| `2e6a8a0af45c331d58dbc25638ab8f9a7d6893bf` | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45184285000) | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45184284590) | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45184284875) |
| `fd84bb398048412593cb896a8c892d5cd7b81902` | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45186656454) | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45186657069) | [Run](https://github.com/gmoddev/gargantuan/actions/runs/45186661371) |
