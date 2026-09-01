---
status: superseded
owner: networking
last_verified: 2026-09-01
superseded_by: devdocs/CurrentArchitecture/CharacterNetworkingFoundation3C.md
related_code:
  - include/gargantuan/network/CharacterProtocol.hpp
  - include/gargantuan/network/CharacterNetwork.hpp
  - src/network/CharacterProtocol.cpp
  - src/network/CharacterNetwork.cpp
  - include/gargantuan/classes/Character.hpp
  - include/gargantuan/classes/KinematicCharacter.hpp
  - src/classes/Character.cpp
  - src/classes/KinematicCharacter.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/CharacterNetworkGnsTests.cpp
  - tests/CharacterNetworkingBenchmark.cpp
  - tests/fuzz/CharacterRealtimeProtocolFuzz.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3B

This document preserves the verified Foundation 3B authority and performance
baseline. Foundation 3C supersedes its realtime publication and presentation
sections; see [CharacterNetworkingFoundation3C.md](CharacterNetworkingFoundation3C.md).

## Implemented boundary

Character / Animation Foundation 3B adds one server-authoritative realtime
Character vertical above the existing scheduler and transport:

```text
client Luau/game input policy
    -> bounded semantic CharacterInputCommand / CharacterActionRequest
    -> NetworkScheduler -> IGameTransport
    -> AuthoritativeCharacterNetwork
    -> server Luau/game movement and action policy callbacks
    -> KinematicCharacter admission + server-known root motion
    -> CharacterAuthoritativeState
    -> NetworkScheduler -> IGameTransport
    -> PredictedCharacterNetwork reconciliation
```

`AuthoritativeCharacterNetwork` and `PredictedCharacterNetwork` own only
Character protocol identity, bounds, admission bookkeeping, prediction history,
and reconciliation. They do not own sockets, structural replication, gameplay
state machines, physics implementation, animation content, or renderer state.
All mutation occurs when the owner calls `Step` or `Reconcile` on Main; a
transport callback only decodes and stores bounded immutable values.

This is an internal native foundation, not a schema-backed developer API yet.
The test fixture supplies real Luau callbacks to prove that movement speed and
semantic action selection can be replaced without changing protocol code.

## Character is not Player or Humanoid

`Character` remains the canonical actor type. `KinematicCharacter : Character`
is the current movement specialization, while `Player.Character` is only an
optional association. The same registration/state channel runs an NPC action
without constructing a `Player`.

There is no Humanoid, and one will not be added. Native code owns low-level
authority, identity, finite/bound validation, movement admission, controller
facts, protocol sequencing, prediction storage, and reconciliation. Game or
engine-shipped Luau owns input mapping, walk/jump policy, action selection,
animation selection, locomotion state, traversal, attack rules, and other game
semantics. Health, combat, and traversal are not fields of a native aggregate.

## Structural and realtime replication

Character lifecycle, hierarchy, authored `CFrame`, `RootPart`, Animator, assets,
and scripts remain ordinary schema state published by
`ReplicationCoordinator`. A peer must first materialize a Character through
that path and explicitly register its `ObjectId` and `StateChannelId` with the
Character manager. Realtime state cannot manufacture an Instance. Unpublish
removes the realtime channel, and hidden Characters receive no state.

Simulation movement and correction use the C++-only
`ApplyRuntimeTransform`/`ApplyRuntimeControllerFacts` path. It synchronizes the
RootPart and runtime observers but does not call `NotifyPropertyCommitted`.
Sustained prediction, authoritative motion, root motion, and correction
therefore generate zero `ChangeJournal` records and no structural property
stream, snapshot, document reconciliation, or authoring dirty state.

Application Remote Instances are also separate. The Character codec is not a
RemoteEvent payload and does not enter `RemoteManager`.

## Control identity and lifetime

A server registers at most 4,096 Characters and 1,024 generation-safe
connections. `BindControl` requires both a registered Character and a
materialization entry for that peer. The binding names:

- a generation-safe Character `ObjectId`;
- the controlling `ConnectionId` held by the server, not trusted from bytes;
- a nonzero `CharacterControlEpoch`; and
- the Character's per-peer `StateChannelId`.

The epoch advances on registration, bind/rebind, revocation, replacement,
disconnect, or another ownership assignment. Unbind is reliable and clears
pending input, actions, and prediction lineage. An old peer, old epoch, stale
ObjectId generation, or unmaterialized ObjectId cannot act on the new lifetime.
NPCs have an epoch but no controlling connection.

## Client commands and server admission

An input contains `ObjectId`, control epoch, independent nonzero
`CharacterInputSequence`, simulation tick, a finite interval in `(0, 0.25]`, a
normalized X/Z movement intent, bounded facing yaw, and the one defined Jump
request bit. It contains no CFrame, velocity, displacement, collision result,
animation playback speed, or root delta. Zero, duplicate, stale, wrong-lane,
wrong-channel, wrong-peer, wrong-Character, malformed, and old-epoch commands
fail closed.

Input is `UnreliableSequenced`: a newer semantic intent supersedes missed input,
and the server retains only one newest pending command per Character. Admission
allows at most four commands between authoritative server steps; the rate window
uses the server's tick, never the client-reported tick. The movement callback
derives a bounded `CharacterMotionRequest`, and `KinematicCharacter` derives the
accepted transform, velocity, floor, collision, slide, and step result.

An action request is `ReliableOrdered` `ReliableApplication` traffic and carries
a separate `CharacterActionSequence`, the latest input lineage it followed, and
a semantic token. It carries no arbitrary Animation or movement delta. The
server action-policy callback may reject the token or map it to a registered
`CharacterActionDefinition`.

## Authoritative actions and root motion

A definition pins a nonzero semantic token, immutable Animation `AssetId`,
`AssetContentId` revision, bounded duration, and server-owned root-motion
evaluator. Acceptance creates `CharacterActionState` with a sequence, selected
token/content, and authoritative start tick. The server evaluates interval root
motion from that known content and submits the resulting request through the
same Character/physics admission path as ordinary movement. Client animation
translation or playback speed never crosses the authority boundary.

Acceptance and rejection are published once as a reliable ordered control-state
message so a one-shot decision is not lost. The active identity is also repeated
in ordinary snapshots while active. A client exposes the latest verified
authoritative action to animation policy through `GetAuthoritativeAction`; a
missing or mismatched local content revision disables prediction and hard-resets
the local lineage. Remote clients apply the authoritative Character transform
but never evaluate action root motion into Character authority, preventing a
second application. Presentation/Animator policy may consume the exposed action
identity independently.

## Authoritative state and delivery

The minimum state contains:

- Character and control epoch;
- independent `RealtimeStateSequence`;
- newest incorporated `CharacterInputSequence` acknowledgement;
- newest resolved `CharacterActionSequence`;
- authoritative simulation tick;
- normalized finite CFrame;
- velocity, floor normal, grounded/teleport flags; and
- optional authoritative action identity/content/start/duration.

Frequent state uses `UnreliableSequenced` on the materialized Character channel.
Newest valid state wins; lost transforms need no resend. Reliable bind/unbind and
action-decision state use ordered control traffic. Scheduler and transport packet
numbers are not Character sequences. If a newer sequenced snapshot crosses an
older reliable action decision in transit, the client may merge only the pinned
action identity whose resolved action sequence matches the newer baseline; it
never rolls the Character transform or acknowledgement backward.

## Prediction and reconciliation

Only the locally controlled Character predicts. `SubmitInput` runs the same
movement-policy callback and `KinematicCharacter::AdmitMotion` path against the
client's world, then records the semantic command in a fixed 64-entry ring. A
predicted action is evaluated only from a locally registered definition whose
AssetId/revision matches the eventual server state.

Reconciliation validates materialization, lane, state sequence, and control
epoch, then:

1. applies the authoritative transform and controller facts immediately;
2. discards recorded commands through the acknowledged input;
3. restores the authoritative action baseline or safely rejects a revision
   mismatch;
4. replays the remaining commands through movement/physics in sequence; and
5. records the new authoritative state/action for presentation policy.

Collision divergence is therefore semantic, not a visual lerp: the server-only
wall test rewinds a client that predicted through clear space and replays only
still-pending intent. Rejected root-action motion disappears without residual
movement debt. A renderer/camera may add future presentation smoothing, but it
cannot change collision state.

Teleport, correction over eight metres, impossible acknowledgement, action
content mismatch, epoch transition, unmaterialization, or disconnect hard-reset
the prediction lineage. History overflow clears the ring and suspends new input
and action prediction until a fresh authoritative baseline arrives. Storage
never expands to accommodate latency or packet loss.

## Protocol version 1

The independently fuzzable codec uses little-endian `GCHR`, version 1, explicit
opcodes, and a 256-byte maximum. It uses no native struct layout, JSON,
DataModel, Luau VM, filesystem, or socket.

| Message | Encoded bytes | Delivery |
| --- | ---: | --- |
| control bind/unbind | 40 | reliable ordered control |
| semantic input | 60 | unreliable sequenced realtime |
| semantic action request | 48 | reliable ordered application |
| state without/with action | 112 / 184 | sequenced realtime; reliable once for action decisions |

The common header carries magic, version, opcode, zero reserved byte,
generation-safe ObjectId, and control epoch. CFrame uses position plus a finite
normalized quaternion. Decoding rejects empty/truncated/oversized/trailing data,
wrong magic/version/opcode, nonzero reserved fields, zero/invalid identity and
sequence fields, undefined flags, invalid finite values, over-length intent,
non-normalized rotation, invalid floor normal, and incomplete/mismatched action
state. `CharacterRealtimeProtocolFuzz` calls only this decoder.

## Bounds, memory, metrics, and allocations

Per controlled client, prediction is 64 inline command records and one optional
predicted action. Per authoritative Character, input storage is one optional
newest command, eight inline pending actions, one active action, and sequence/
controller metadata. Per peer, relevance is capped by the 4,096 Character
manager limit. Actions are capped at 4,096 definitions; action duration is at
most ten hours at 60 ticks/second. Disconnect, unpublish, Character destruction,
and manager destruction erase their maps and histories.

Counters saturate and cover received/accepted/stale/unauthorized/rate-limited
commands, accepted/rejected actions, states, stale states, corrections, hard
resets, replay, overflow, bytes, protocol rejects, and decode/admission/
movement/root/encode/scheduler/reconcile/replay CPU.

The Release allocation probe records zero allocation in command decode/admission
and prediction reconciliation/replay after setup. Each outgoing state owns one
bounded payload vector required by the general scheduler contract; Character
runtime state and root-action bookkeeping create no per-frame heap object. A
single reserve in the Character writer and allocation-free scheduler validation
avoid repeated packet-buffer growth and validation copies.

## Validation evidence

`gargantuan_character_networking_tests` runs headlessly with the real
`NetworkScheduler`, deterministic `SimulatedTransport`, and separate server and
client `WorldRoot`s. It covers materialization/bind, semantic prediction,
acknowledgement/replay, action accept/reject, reliable action identity,
server-only collision divergence, stale epoch, wrong peer/ObjectId, command
flooding, history overflow, teleport, revision mismatch, stale state, NPCs,
hidden relevance, deterministic 50 ms latency plus jitter/loss/duplication/
reorder, disconnect, destruction, ten teardown cycles, and a real custom Luau
movement/action policy. No renderer is created.

The Animation Foundation test applies a network-style correction to a Character
with a live residual CPU pose and verifies:

```text
corrected RootPart world transform
    * residual joint-model transform
    * Attachment local transform
```

for the animated Attachment, its positional Sound, and its ProximityPrompt,
with zero journal records. Animation 2C remains scoped: semantic consumers keep
the rig required, while networking neither enables global pose evaluation nor
replicates joint palettes.

The optional `gargantuan_character_real_transport_tests` carries bind, input,
action, authoritative movement, state, and reconciliation over the production
GameNetworkingSockets adapter on localhost. The FirstCompleteGame proof uses the
same deterministic Character-network executable alongside its existing packaged
ordinary movement, open/clipped root-motion NPCs, and semantic-anchor fixture;
the protocol does not add a sample-only API or runtime module.

## Windows Release benchmark

Measured on the primary Ryzen 9 7950X3D with MSVC Release, 120 iterations:

| Characters | active | admission mean us | total step mean / p95 / p99 us |
| ---: | ---: | ---: | ---: |
| 1 | 1 | 0.32 | 1.17 / 1.10 / 1.50 |
| 10 | 10 | 2.47 | 9.77 / 9.90 / 11.90 |
| 100 | 100 | 27.10 | 109.44 / 114.10 / 125.50 |
| 500 | 500 | 143.88 | 708.76 / 753.60 / 819.70 |

The benchmark reports movement, root evaluation, state encode, and scheduler
sub-timers separately in its CSV. The 500-Character mixed case registers 50
active/relevant network Characters plus 450 inactive/offscreen analog actors;
renderer cost is excluded. It measured 13.53 us admission and 83.09 / 88.00 /
125.50 us step mean/p95/p99, with 1,126,200 payload bytes and 6,050 messages
across the 120 iterations.

| Pending inputs | reconcile mean / p95 / p99 us | replayed per sample |
| ---: | ---: | ---: |
| 0 | 0.48 / 0.50 / 1.30 | 0 |
| 2 | 0.90 / 1.00 / 2.10 | 2 |
| 4 | 1.39 / 1.40 / 2.10 | 4 |
| 8 | 2.32 / 2.40 / 2.40 | 8 |
| 64 (bound) | 15.69 / 15.80 / 25.60 | 64 |

At 60 updates/second, input is 3,600 bytes/second for one controlled Character.
Uncompressed state is 6,720, 215,040, 672,000, and 3,360,000 bytes/second for
1, 32, 100, and 500 relevant Characters. Corresponding message rates are 120,
1,980, 6,060, and 30,060 per second including one input stream. One bind plus
one action request is 88 bytes before transport framing. These are audit
baselines, not production WAN targets.

## Security answers

- Local walk speed, animation translation, or playback speed changes only local
  speculation; server policy/content evaluation derives accepted motion.
- A different Character, peer, epoch, connection generation, or stale action
  sequence is rejected before authority code.
- Reconnect/rebind advances control epoch, so queued old packets cannot survive.
- Hidden/unmaterialized Characters receive and apply no realtime state.
- Non-finite or malformed floats fail in the independent decoder and never reach
  Character state.
- Input/action/history/maps are hard-bounded; flooding cannot grow memory.
- Action replay is rejected by its independent sequence and cannot repeat root
  displacement.
- Client CFrame is absent from client-to-server messages and has no server apply
  path.
- Runtime CFrame/controller writes bypass committed authoring and structural
  replication state by design and test.

## Explicit deferrals and next priorities

Foundation 3B does not add Humanoid, health/combat/ability systems, arbitrary
client transforms, distributed physics ownership, remote interpolation,
presentation smoothing, motion warping, IK, retargeting, animation graphs,
ragdoll networking, weapon rewind/lag compensation, production entity interest,
quantization/delta compression, portal/cross-server transfer, Node simulation,
matchmaking/authentication, or Studio prediction tooling.

Character / Animation Foundation 3C priorities, in evidence order, are:

1. batch/quantize/delta-compress Character state and select a measured state
   cadence, because the 500-Character raw stream is 3.36 MB/s before framing;
2. add bounded remote interpolation and local presentation smoothing over the
   already-correct semantic Character baseline;
3. expose the native manager through production server/client session bootstrap
   and narrow Luau semantic command/action bridges without moving policy native;
4. add measured spatial relevance/interest integration for large worlds while
   retaining explicit materialization; and
5. extend authoritative action presentation/Animator synchronization and content
   readiness before considering motion warping, IK, retargeting, or graph tools.

Future ownership remains an explicit server-issued control-epoch lease. It must
reuse this command/state/reconciliation seam; ownership never authorizes a
client transform. Portal work may later transfer accepted Character state only
after region identity and server handoff contracts exist.
