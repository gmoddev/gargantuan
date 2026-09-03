---
status: current
owner: networking
last_verified: 2026-09-03
related_code:
  - include/gargantuan/network/CharacterProtocol.hpp
  - include/gargantuan/network/CharacterNetwork.hpp
  - include/gargantuan/classes/Character.hpp
  - include/gargantuan/classes/BasePart.hpp
  - src/network/CharacterProtocol.cpp
  - src/network/CharacterNetwork.cpp
  - src/classes/Character.cpp
  - src/classes/BasePart.cpp
  - src/render/RenderExtractor.cpp
  - src/render/RenderPublisher.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/CharacterNetworkingBenchmark.cpp
  - tests/fuzz/CharacterRealtimeProtocolFuzz.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3C

## Implemented boundary

Foundation 3C keeps the 3B authority, control-epoch, prediction, action, and
reconciliation model and replaces its expensive server-to-client publication
edge:

```text
60 Hz authoritative Character simulation
    -> per-peer materialized Character set
    -> 20 Hz change/absolute-refresh selection
    -> deterministic ObjectId order
    -> bounded GCHR v2 compact absolute frames
    -> NetworkScheduler / unreliable-sequenced IGameTransport
    -> immediate semantic state application
    -> bounded remote/local presentation state
    -> renderer-only RootPart projection
```

`Character` remains the permanent canonical gameplay actor. There is no
`Humanoid`, differently named Humanoid-style aggregate, native gameplay state
machine, client CFrame authority, or requirement that an NPC have a `Player`.
`KinematicCharacter : Character` remains only the current movement
specialization. Game Luau still owns gameplay and movement/action policy.

The implementation adds no schema or Luau API. The new C++ configuration,
metrics, presentation update, compact codec, and `BasePart::GetRenderCFrame`
surface are internal engine integration seams. Node, Studio, MCP, and Telemetry
are unchanged.

## Why 3C exists

The reproduced 3B state was one 112-byte state message per relevant Character
per 60 Hz update. One 60 Hz input stream is included in the message totals:

| Characters | state payload B/s | total messages/s |
| ---: | ---: | ---: |
| 1 | 6,720 | 120 |
| 32 | 215,040 | 1,980 |
| 100 | 672,000 | 6,060 |
| 500 | 3,360,000 | 30,060 |

At 500 Characters, scheduler/message pressure was the primary scaling problem,
not Character simulation CPU.

## GCHR versioning and state frames

Foundation 3E advanced server state frames to GCHR v3. It consumed the former
16-bit reserved tail as a bounded peer materialization epoch while retaining
the exact 28-byte header and compact-state sizes described below. GCHR v3 drops
delayed state from a pre-leave lifetime after the same ObjectId reenters. The v2
layout below remains the historical 3C baseline.

Foundation 3E.1 supersedes that wire revision with GCHR v4: the
materialization epoch is 64-bit, the header is 34 bytes, and reliable
per-request action results are explicit. Compact state size and the 15-state
batch ceiling remain unchanged.

GCHR v1 remains unambiguously decodable for control bind/unbind, semantic
input, action request, and legacy singular state. Server realtime publication
uses the new v2 `StateFrame` opcode. A v2 header cannot be interpreted as a v1
message, and v2 currently accepts no opcode other than `StateFrame`.

The v2 frame is:

```text
magic/version/opcode/reserved       8 bytes
server tick                         8 bytes
per-peer frame sequence             8 bytes
state count/reserved                4 bytes
compact absolute states             N * (74 or 146 bytes)
```

The protocol ceiling is 1,200 bytes and 15 states. The effective ceiling is
the smaller of 1,200, the manager configuration, and the negotiated unreliable
datagram limit. A base frame carries at most 15 states (1,138 bytes). A frame
whose states all carry active action identity carries at most eight (1,196
bytes). Configuration requires room for the largest one-state absolute frame:
174 bytes. Unreliable fragmentation is never assumed.

For each peer, eligible states are traversed in `std::map<ObjectId, ...>` order,
so split boundaries use lexicographic `(Slot, Generation)` `ObjectId` order and
never pointer or unordered-map iteration. The outer unreliable order uses the
first enclosed Character's materialized channel and a monotonic per-peer frame
sequence. The payload retains every Character's independent
`RealtimeStateSequence`; a newer frame cannot make an older enclosed Character
state valid. The decoder requires strict increasing ObjectIds, so duplicates
and unordered entries reject the whole malformed frame.

Frames split as soon as the next state would cross the byte or 15-state bound.
Each frame is independently applicable. There is no batch-set completion or
atomic server-tick transaction: losing one frame affects only its enclosed
Characters, and later absolute states recover them.

## Compact absolute state

Every v2 state is self-contained. Foundation 3C deliberately does not ship
delta compression: batching, 20 Hz cadence, compact absolute encoding, and
unchanged-state suppression already produce the required reduction, while a
delta baseline would add packet-loss and materialization dependencies.
`DeltaStatesSent` and `BaselineMisses` therefore remain zero.

The 74-byte base state contains ObjectId, control epoch, per-Character state
sequence, acknowledged input sequence, resolved action sequence, position,
rotation, velocity, floor normal, and flags. The frame supplies the common
authoritative tick. Active action identity adds 72 bytes: action sequence,
semantic token, pinned `AssetId`, pinned `AssetContentId`, start tick, and
duration.

| Field | Representation | Range / resolution | Maximum encode error | Overflow behavior |
| --- | --- | --- | --- | --- |
| Position | three IEEE-754 float32 values | normal finite float32 range | none beyond the existing float32 runtime value | non-finite rejects |
| Rotation | normalized quaternion, four signed int16 values; `-32768` reserved | `[-1, 1]`, `1 / 32767` component resolution | at most `1 / 65534` per component; measured angular round trip remains below 0.001 rad | invalid norm or reserved value rejects |
| Velocity | three signed int16 values | `[-511.984375, 511.984375]`, `1 / 64` units/s | `1 / 128` units/s per component | encode rejects; never clamps or wraps |
| Floor normal | three signed int16 values; `-32768` reserved | `[-1, 1]`, `1 / 32767` | `1 / 65534` per component | encode/decode rejects |
| Grounded/teleport | one bit each in a byte | two defined bits | exact | unknown bits reject |

Position intentionally remains absolute float32. It does not impose a small
global fixed-point world and can later become `(SpatialAddress, local
transform)` without changing Character authority. Spatial regions and portals
remain unimplemented.

Quaternion encoding preserves arbitrary valid Character orientation instead of
assuming every future `Character` is upright. The current
`KinematicCharacter` authority still normally produces world-up yaw.

The decoder is independent of native struct layout, JSON, DataModel, Luau,
filesystem, sockets, and renderer. It rejects wrong magic/version/opcode,
nonzero reserved fields, zero or excessive counts, truncation, trailing bytes,
invalid ObjectIds/epochs/sequences, duplicate or unordered Characters,
non-finite position, invalid quantized values/norms, undefined flags,
out-of-range controller facts, and incomplete action identity.

## Cadence, selection, and recovery

Authoritative simulation and input remain 60 Hz. State publication defaults to
20 Hz, one publication every three simulation ticks. Animation logical time,
root-motion evaluation, collision admission, and prediction still run at their
simulation cadence. The network cadence is configuration, not a protocol
assumption, and may later vary per peer or relevance policy.

The measured continuously-moving matrix was:

| Cadence | Characters | state payload B/s | messages/s including one 60 Hz input | mean server step us |
| ---: | ---: | ---: | ---: | ---: |
| 15 Hz | 1 / 32 / 100 / 500 | 1,530 / 36,780 / 113,940 / 569,280 | 75 / 105 / 165 / 570 | 0.31 / 4.04 / 13.07 / 74.77 |
| 20 Hz | 1 / 32 / 100 / 500 | 2,040 / 49,040 / 151,920 / 759,040 | 80 / 120 / 200 / 740 | 0.28 / 4.96 / 16.15 / 94.83 |
| 30 Hz | 1 / 32 / 100 / 500 | 3,060 / 73,560 / 227,880 / 1,138,560 | 90 / 150 / 270 / 1,080 | 0.41 / 7.28 / 23.84 / 133.86 |
| 60 Hz | 1 / 32 / 100 / 500 | 6,120 / 147,120 / 455,760 / 2,277,120 | 120 / 240 / 480 / 2,100 | 0.77 / 13.89 / 43.49 / 239.90 |

Twenty hertz was selected because its 50 ms snapshot interval supplies two
samples inside the fixed 100 ms interpolation delay while retaining a 77.4%
state-byte reduction and a 97.5% total-message reduction at 500 moving
Characters. Fifteen hertz saves another 25% but increases the sample gap to
66.7 ms; 30 and 60 Hz materially increase message and byte cost without a
corresponding presentation need in the deterministic fault matrix.

Selection is per peer and begins with its already-authorized materialized
Character set. A 64-bit semantic fingerprint excludes tick/sequence but covers
transform, controller facts, acknowledgement, action resolution, and pinned
active action. Unchanged states are suppressed. Each peer receives a
self-contained absolute refresh after 60 simulation ticks (one second), and a
new materialization has no fingerprint so its next cadence publishes an
absolute state. There is no hidden ObjectId side channel and no interest or
distance logic in GCHR.

## Remote presentation

Decoded state never creates an Instance. A materialized remote Character stores
at most four `(authoritative tick, CFrame)` samples spanning at most 15
simulation ticks (250 ms). Newest sequence still determines semantic
acceptance. Reconciliation immediately writes the newest authoritative CFrame
and controller facts through the runtime-only Character path; interpolation
changes only the render projection.

`UpdatePresentation` samples six ticks (100 ms) behind presentation time and
uses `CFrame::Lerp` between bracketing authoritative samples. Oldest samples are
discarded first on count or time-window overflow. A missing future sample
increments `InterpolationBufferUnderruns` and holds the newest state. There is
no remote extrapolation and therefore no indefinitely walking remote actor.

Teleport, control epoch/lifetime change, Character replacement,
unmaterialization, disconnect, non-monotonic authoritative time, or an eight
metre sample discontinuity resets the buffer and render offset. Frames are
independent, so a dropped batch does not stall presentation for another batch.

Remote action identity and authoritative start tick remain available to
Animator policy, but the network layer never reevaluates remote root motion.
World motion comes only from interpolated authoritative GCHR transforms. If
animation content is absent, state continues and later content can begin at the
authoritative phase; missed root displacement is never replayed.

## Local correction presentation

Local reconciliation is unchanged semantically: authoritative state is applied,
acknowledged input is discarded, valid pending input is replayed, and collision
facts become correct immediately. Foundation 3C captures the previously
rendered CFrame and represents only the remaining visual error as:

```text
rendered RootPart = semantic RootPart CFrame * presentation offset
```

The visual thresholds are:

| Final visual error | Presentation behavior |
| ---: | --- |
| at most `1e-4` m | no correction state |
| above `1e-4` through 1 cm | converge over 3 ticks / 50 ms |
| above 1 cm through 1 m | converge over 6 ticks / 100 ms |
| above 1 m | immediate presentation reset |
| teleport, invalid lineage/content, epoch/replacement, or at least 8 m semantic correction | immediate presentation and prediction-lineage reset |

These thresholds never decide whether server state is accepted. The existing
64-command prediction ring remains the only input replay history.

## RootPart, renderer, camera, and semantic anchors

`Character` owns a transient optional presentation offset and transfers it only
to its current RootPart. `BasePart::GetCFrame` remains semantic. Physics,
queries, Interaction, static Attachment resolution, positional Sound,
ProximityPrompt, scripts, and authoring observe that semantic CFrame.
`GetRenderCFrame` composes the optional offset and is consumed only by render
extraction/publication. Offset changes mark the transient render transform dirty
without firing a property commit or entering `ChangeJournal`.

The shipped Luau camera continues to follow responsive predicted
`Character.Position`. It intentionally remains semantic in 3C because exposing
the private correction offset to Luau would widen the public surface; large
resets cannot leave it on stale coordinates. A future native camera projection
seam may share the renderer offset without changing authority.

The resulting semantic contract is:

```text
Attachment / Sound / Prompt world state
    = corrected/replayed semantic Character transform
    * residual animated joint pose
    * Attachment local transform
```

Visual root animation remains residual pose. Neither local nor remote
presentation writes the offset back to Character CFrame, RootPart CFrame,
physics, action admission, collision, or journal state.

## Bounds, allocation behavior, and metrics

Server batch storage is a fixed 15-state frame. Per peer, publication metadata
is one bounded fingerprint/tick record per materialized Character and one frame
sequence. Per remote Character, presentation storage is four inline CFrame
samples plus one inline local-correction CFrame/counters. All maps remain under
the existing 4,096 Character and 1,024 peer ceilings. Disconnect,
unmaterialization, destruction, replacement, and manager teardown erase the
state.

No per-Character heap object is created during a network tick. Each emitted
frame owns the one payload vector required by `NetworkMessageIntent`, so
allocation pressure scales with batch count. At 20 Hz moving load, measured
allocations per publication tick were 1.075, 3.9, 9.7, and 47.45 for 1, 32,
100, and 500 Characters; the 500 case emitted 34 frames per publication rather
than 500 messages. The maps and inline presentation buffers do not grow in
steady state.

Saturating metrics cover states considered/suppressed, absolute/delta states,
semantic/compact bytes, frames, states per frame, splits, scheduler submissions,
baseline misses, stale drops, interpolation underruns/resets, local smooth and
hard presentation corrections, malformed frames, and separately timed change
detection, assembly, encode, scheduler, reconciliation, replay, and
interpolation work.

On the Ryzen 9 7950X3D MSVC Release run, moving 20 Hz server results were:

| Characters | mean / p95 / p99 step us | change / assembly / encode / scheduler total us over 120 ticks |
| ---: | ---: | ---: |
| 1 | 0.28 / 0.80 / 1.00 | 6.9 / 22.4 / 11.5 / 1.6 |
| 32 | 4.96 / 14.3 / 17.9 | 197.3 / 346.7 / 210.0 / 5.5 |
| 100 | 16.15 / 46.6 / 56.5 | 665.0 / 1,110.7 / 643.6 / 13.1 |
| 500 | 94.83 / 272.9 / 298.4 | 3,835.0 / 6,515.9 / 3,212.0 / 95.1 |

Prediction replay remained 0.53, 0.98, 1.42, 2.43, and 15.96 us mean for 0,
2, 4, 8, and 64 pending commands in a representative Release run.

## Apples-to-apples 3B/3C result

The selected 20 Hz continuously-moving comparison includes one 60 Hz input
stream in byte/message totals:

| Characters | 3B total B/s | 3C total B/s | reduction | 3B msg/s | 3C msg/s | reduction |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10,320 | 5,640 | 45.3% | 120 | 80 | 33.3% |
| 32 | 218,640 | 52,640 | 75.9% | 1,980 | 120 | 93.9% |
| 100 | 675,600 | 155,520 | 77.0% | 6,060 | 200 | 96.7% |
| 500 | 3,363,600 | 762,640 | 77.3% | 30,060 | 740 | 97.5% |

The 20 Hz movement mixes were:

| Workload | 1 / 32 / 100 / 500 state B/s | 1 / 32 / 100 / 500 msg/s including input |
| --- | ---: | ---: |
| stationary, one-second absolute refresh | 102 / 2,452 / 7,596 / 37,952 | 61 / 63 / 67 / 94 |
| all moving | 2,040 / 49,040 / 151,920 / 759,040 | 80 / 120 / 200 / 740 |
| 20% moving | 2,040 / 12,826 / 36,780 / 182,276 | 80 / 82 / 105 / 227 |
| active root action identity and motion | 3,574 / 98,688 / 308,680 / 1,542,280 | 80.5 / 156 / 370 / 1,570 |

The one-Character mixed row rounds its moving population to one. The root-action
row includes the one-time reliable action decision over the two-second sample;
active compact states are 146 bytes and fit eight per full frame. With 50
moving relevant Characters and 450 registered but visually inactive actors,
the selected cadence produced 76,240 state B/s, 140 total messages/s, 1,000
states/s, a 38.15 us mean server step, and 5.375 allocations per publication
tick. Relevance selection, not animation scheduling, controls the publication
set.

## Validation evidence

The headless Character suite now covers every v1/v2 truncation boundary,
15-state maximum frames, low 250-byte splitting, duplicate ObjectIds, bad
counts/masks/flags/quantized values, overflow, trailing data, wrong
version/opcode, position/rotation/velocity error, and random malformed input.
The independent libFuzzer entry continues to call only the decoder.

Manager tests prove 32 states become `15/15/2` in three scheduler submissions,
unchanged suppression and one-second refresh, one-state change publication,
per-Character stale protection inside a newer frame, independent batch loss,
materialization gating, NPC operation, four-snapshot interpolation, bounded
hold without extrapolation, and immediate semantic state.

The deterministic simulator exercises all 48 combinations of 0/50/100/200 ms
latency, 0/modest/stress jitter/reorder, and 0/1/5/10% loss, with duplication in
the stress tier. Remote NPC semantic and presented state converge after the
fault window, the presentation error remains below the eight-metre reset bound,
and no `Player` is constructed for the NPC.

The local matrix covers exact, 1 cm, 5 cm, 25 cm collision divergence, 1 m,
8 m, and teleport corrections. It proves immediate semantic correction,
eligible-only smoothing, 100 ms convergence, semantic RootPart and Attachment
anchors, renderer-only offset, and zero journal records.

Existing tests retain server collision clipping, accepted/rejected root motion,
pinned content mismatch, 64-entry replay overflow, stale epochs/ObjectIds,
reconnect, destruction, ten lifecycle cycles, custom Luau policy,
FirstCompleteGame BeaconLunge identity, optional localhost GNS, package closure,
and structural-replication isolation.

## Explicit deferrals and Foundation 3D result

Foundation 3C did not itself implement delta compression, production spatial
interest, regions/portals, production session bootstrap, default Luau network
bridges, distributed ownership, server migration, adaptive network-quality
estimation, remote gameplay prediction, combat rollback, ragdoll networking,
motion warping, IK, retargeting, animation graphs, Node realtime routing, Studio
network tuning UI, or a final long-term compatibility promise.

Foundation 3D completed priority 1 through the packaged `GameSession`, GSES
admission protocol, trusted `LocalPlayer` association, server-owned Player and
Character lifecycle, and `CharacterControlService`; see
`CharacterNetworkingFoundation3D.md`. Remaining priorities are:

1. add measured peer-specific spatial relevance upstream of the existing
   materialized-Character selection seam;
2. complete Animator action-phase/content-readiness presentation without
   replicating pose or applying root motion remotely;
3. evaluate per-peer cadence/priority only from observed bandwidth and quality
   pressure; and
4. reconsider explicit-baseline, periodically absolute delta states only if
   future measurements show enough benefit to justify their loss behavior.

Future ownership remains a server-issued control-epoch lease. It never grants
client transform authority.
