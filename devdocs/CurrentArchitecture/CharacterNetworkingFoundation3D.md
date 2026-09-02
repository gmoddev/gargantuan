---
status: current
owner: networking
last_verified: 2026-09-02
related_code:
  - include/gargantuan/network/GameSession.hpp
  - include/gargantuan/network/GameSessionProtocol.hpp
  - include/gargantuan/services/CharacterControlService.hpp
  - src/network/GameSession.cpp
  - src/network/GameSessionProtocol.cpp
  - src/services/CharacterControlService.cpp
  - src/classes/RemoteBase.cpp
  - src/player/PlayerMain.cpp
  - assets/runtime/DefaultNetworkClientRuntime.luau
  - assets/runtime/DefaultNetworkServerRuntime.luau
  - tests/GameSessionTests.cpp
  - tests/GameSessionGnsTests.cpp
  - tests/PackagedGameSession.ps1
  - tests/fuzz/GameSessionProtocolFuzz.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3D

## Production session boundary

`GameSession` is the one game-session coordinator. It composes an
`IGameTransport`, `NetworkScheduler`, `ReplicationCoordinator` or
`ReplicaApplier`, `RemoteManager`, the authoritative or predicted Character
manager, and Player association. These systems retain their own semantics but
refer to one generation-safe `ConnectionId` lifetime; there are no independent
replication, Remote, Character, or Player connection identities.

Packaged entrypoints are:

```text
GargantuanPlayer --server-bind HOST:PORT [--headless]
GargantuanPlayer --connect HOST:PORT [--headless]
```

The GNS-enabled player runs `Poll -> Engine::Step -> GameSession::Step`; both
network roles pace that loop to 60 Hz, including a headless dedicated server.
Offline launch remains the default and still creates its trusted local Player
without a transport. A future listen server can compose the same server/client
roles without changing Character policy; 3D does not add listen-server UX.

## Admission and identity

The bounded `GSES` v1 handshake has three messages: `ClientHello`,
`ServerAccepted`, and `ClientReady`. A peer progresses through:

```text
transport connected
    -> protocol and downward-limit negotiation
    -> accepted game peer
    -> server-created Player
    -> structural baseline and exact Player materialization
    -> trusted LocalPlayer association
    -> client runtime attachment
    -> gameplay ready
```

No GCHR or Remote traffic is accepted before the corresponding phase. The
server creates the Player; a client does not replicate one upward or supply a
Player ObjectId. `ServerAccepted` carries the exact authoritative Player
ObjectId, replicated session-scoped PlayerId, replication epoch, session epoch,
echoed nonce, identity kind, and negotiated limits. The client accepts
`LocalPlayer` only when the exact object materializes under `Players` with the
matching PlayerId. First-player, mutable Name, hierarchy order, and tags are not
identity mechanisms.

The current identity kind is `DevelopmentLocal`, exposed internally as provider
`session-development`. It is an accepted local/development game-session
identity, not secure internet account authentication. Inspection of
`gargantuan-node` found AuthProvider/session/capability building blocks but no
stable game-join grant bound to the GNS endpoint, nonce, and Engine session.
Classification is therefore **B**: Node integration remains premature. Node is
unchanged and never enters the per-frame Character path. A future optional
ticket validator belongs between transport connection and accepted peer.

## Player and Character lifecycle

The authoritative association is native-only:

```text
ConnectionId <-> Player ObjectId
Player.Character -> optional canonical Character
```

`Player.Character` remains the sole gameplay relationship. When it changes,
the server revokes the old GCHR control lease and binds the newly materialized
Character with a fresh `CharacterControlEpoch`. Destruction and disconnect
remove manager state before Player teardown. A reconnect receives a new
connection generation, Player identity, and control lifetime. One peer cannot
command another Player's Character.

`CharacterAutoLoads` is server policy in a network session. Engine-shipped
server Luau creates ordinary `KinematicCharacter`, Part RootPart, and related
Instances in response to `CharacterSpawnRequested`; the client never
auto-spawns an authoritative actor. Setting it false permits a ready Player
without a Character until server game code calls `LoadCharacter`. NPC and
server-owned Characters remain valid without Players and still use generic
GCHR relevance/state publication.

`Character` is permanently Gargantuan's canonical gameplay actor. `Humanoid`,
`HumanoidController`, `HumanoidStateMachine`, renamed equivalents, and a giant
native gameplay state machine are prohibited by `docs/invariants/Core.md`.

## Replaceable Luau policy bridge

`CharacterControlService` is the narrow high-level bridge. Client Luau calls
`SubmitMoveIntent`, `RequestAction`, and optionally `SetPredictionEnabled`.
Client code never supplies a connection, epoch, protocol sequence,
acknowledgement, state batch, prediction-history index, authoritative velocity,
transform, or root displacement. Server and prediction policy install the same
bounded command-to-motion callback with `SetMovementPolicy`; server game policy
may install action admission with `SetActionPolicy`.

The default path is:

```text
client: UserInputService -> DefaultActionMap -> DefaultNetworkCharacterRuntime
        -> CharacterControlService -> local prediction + GCHR command

server: validated GCHR command -> CharacterControlService movement policy
        -> Character motion request -> physics admission -> GCHR state
```

`DefaultNetworkLocomotion` executes on both client and server, so default speed,
gravity, and jump logic are shared while the native
`KinematicCharacter` remains the common collision primitive. Game code can
replace those policies without Engine edits. Predictable custom policy is an
opt-in contract: the game guarantees equivalent client/server behavior and
registers the same action/policy content on both sides. Otherwise it disables
prediction and accepts authoritative presentation. Client matching never grants
authority.

`RegisterAction` hashes a bounded game-defined name to an opaque token and pins
a Ready Animation `AssetId` plus immutable `ContentId`. Server policy selects
duration and bounded root translation/yaw and may reject each request. Clients
cannot supply content revision, playback speed, duration, multiplier, or root
motion. `BeaconLunge` remains sample/test content rather than a native action.

`RequestAction` reports only whether the request entered the bounded native
queue. `ActionResolved(Character, ActionName, Accepted)` later exposes the
authoritative semantic decision, and `ActionEnded(Character, ActionName)`
exposes authoritative expiry/replacement independently of local clip
completion. Gameplay never correlates control epochs, action sequences,
acknowledgements, or packet state. Rejection removes predicted action lineage;
the next authoritative state reconciles motion and presentation without root
debt or automatic retry.

Client presentation consumes the manager's bounded current-action view. A
predictable local action may start on the next session safe point. Acceptance
realigns it to the authoritative start phase; a remote or delayed active state
starts at its current authoritative phase. The bridge locates an Animator under
the Character and loads only the exact Ready `AssetId`/`ContentId`. Missing,
stale, incompatible, or not-yet-materialized content leaves world movement and
semantic action state intact and is retried only while the action remains
active. An action already past its bounded duration is never replayed from
zero. Client-created presentation tracks never enable root motion, so remote
visual playback cannot apply displacement a second time.

Script source is intentionally absent from structural replication. Before a
packaged network client constructs its Engine, `PackageBuilder` hydrates Client
Scripts and ModuleScripts from the already validated local package into exact,
unambiguous matching replicated objects. Server Scripts are never activated on
the client, and server source is not made a network payload.

## Ordering, rollback, and shutdown

Server admission creates and parents the Player, commits the native
ConnectionId association, then exposes `PlayerAdded`; shipped/game server Luau
may consequently load its Character. The baseline is produced only after that
association. Client application may receive ancestry and reference properties
in different operations, so `ReplicaApplier` establishes all published
ancestry before applying references. `LocalPlayer` is selected only after the
complete baseline resolves the exact accepted Player ObjectId and PlayerId.
The client Engine and ordinary client scripts are created after that selection;
GCHR control still requires a subsequent materialized control bind.

The explicit physical phases are `TransportConnected`, `Accepted`, and
`Ready`; the public endpoint status adds stopped/listening/connecting/failed.
Every partial failure calls the same peer teardown: Remote, Character,
replication, and scheduler state are removed before the authoritative Player.
Server shutdown stops accepting, tears down every peer/control/Player, stops
transport, then the owner destroys the Engine/VM. Client shutdown detaches the
Character bridge and presentation tracks, removes prediction/Remote state,
clears trusted `LocalPlayer`, resets the replica, and only then destroys its
Engine. A hard disconnect uses the same path and cannot leave input running.

The packaged loop is `transport Poll -> host input -> Engine::Step ->
GameSession::Step`. On the server, decoded input has already been reduced to
one latest validated command; Engine PreSimulation/game policy and physics run,
then the session samples bounded Character policy, admits motion/root action on
Main, publishes due 20 Hz state, pumps Remotes, and flushes the scheduler. On a
client, ActionMap samples once in PreSimulation and native prediction records
the command; the session then reconciles authoritative state, updates
interpolation/correction and action phase, publishes semantic results, pumps
Remotes, and flushes. No transport callback enters Luau or mutates a Character.

## Bounds, policy failure, and memory

The server retains one newest continuous input per controlled Character and
consumes it once per authoritative tick. Jump remains a one-shot flag for a new
input sequence. After six server ticks (100 ms at the default 60 Hz) without a
fresh arrival, movement intent and jump become neutral; gravity/server policy
may continue. There is no task or callback queue per input packet. Policy is a
single bounded, non-yielding pcall per active Character at the simulation safe
point. A policy error increments `MovementPolicyErrors`, yields no motion for
that sample, and does not change authority or disconnect unrelated peers.

Session capacity is 512 peers, aligned with RemoteManager. GCHR retains its
1,024-peer internal ceiling for non-session/future compositions. GSES is 256
bytes, the handshake default is 600 ticks, Characters are bounded at 4,096,
registered action names at 256 entries/64 UTF-8 bytes, pending actions at eight
per control, prediction history/action results at 64, and state frames at 1,200
bytes/15 base states. Peer/session maps, replication views, scheduler state,
latest input, action state, and Player association are removed together. Player
adds only session identity; Character adds no unbounded gameplay-policy state.

## Surface audit

The new Luau-visible surface is one `CharacterControlService`:

| Member | Domain and purpose |
| --- | --- |
| `SubmitMoveIntent` | client game/runtime; normalized move/facing/jump intent for the trusted active control |
| `RequestAction` | client game/runtime; bounded registered semantic name only |
| `SetMovementPolicy` | client or server game/runtime; replaceable command-to-`Character:Move` policy |
| `SetActionPolicy` | server game/runtime only; authorize a registered name for the associated Player/Character |
| `RegisterAction` | client registers local presentation/prediction only; server independently registers authoritative pinned content and root parameters |
| `SetPredictionEnabled` | client game/runtime; clears replay state when selecting server-authoritative presentation |
| `ActionResolved` | client semantic accepted/rejected result; no protocol diagnostics |
| `ActionEnded` | client authoritative lifetime end/replacement; independent of visual clip completion |

All methods preserve the caller's server/client security context; PreRun,
offline misuse, and the wrong direction fail. Protocol identifiers remain
native-only. New native surfaces are `RuntimeMode`, `GameSessionProtocol`,
`GameSession`, the internal Player identity/lifecycle methods, the
`CharacterControlService` attachment callbacks, and the Character manager's
presentation/result drains. They own identity, staged lifetime, codecs,
authorization, bounded replay, and safe VM invocation—responsibilities that
cannot be trusted to replicated Luau. `RemoteBase` also accepts an internal
authoritative network ObjectId so a client-local replica can use the same Remote
wire identity; this is not exposed to Luau.

## Security answers

- A client cannot choose `LocalPlayer`: `LocalPlayer` is read-only and exact
  GSES ObjectId plus replicated PlayerId must match a direct child of Players.
- A client cannot create its server Player or assign server `Player.Character`:
  structural replication is server-to-client only and has no upstream apply
  path.
- Client movement and actions always use the native active bind. A local replica
  write cannot retarget it, so another Player's Character is not commandable.
- The client supplies intent, not WalkSpeed, velocity, CFrame, Animation,
  content revision, duration, or root displacement. Server Luau and pinned
  server registration choose all authoritative results.
- Reconnect is a new ConnectionId generation/session/Player association and a
  fresh CharacterControlEpoch. Old epochs, object generations, sequences, and
  packets fail before gameplay policy.
- Continuous floods replace one latest slot and remain under native per-tick
  admission; no `task.spawn` amplification exists. Stale intent neutralizes
  after six ticks.
- A malformed, spoofed-Player, premature-GCHR, timed-out, or partially failed
  handshake invokes atomic teardown and leaves no active Player.
- A Player with no Character is ready and can use Remotes; it has no GCHR
  control. NPC Characters remain Player-independent.
- Default policy is entirely replaceable from game Luau. Custom policy opts
  into matching prediction explicitly or disables it and uses authoritative
  presentation; equivalent client code never grants authority.

## Validation and measured results

The deterministic suite covers malformed GSES magic/version/opcode/reserved/
identity/truncation/trailing/oversize input, premature GCHR, spoofed ready
Player identity, silent timeout, 100 accepted connect/disconnect generations,
ten complete Player/Character/action/movement/disconnect sessions, default and
custom semantic actions (accept and reject), action phase/end presentation,
an ordinary reliable RemoteEvent, no-Character then spawn, replacement, stale
input, prediction-disable behavior, and two-client LocalPlayer/control
isolation. The decoder also has a socket/filesystem-free libFuzzer adapter.
Existing Character tests retain the 48-case latency/jitter/loss/reorder matrix,
delayed materialization/control, stale generation/epoch, action replay/content
mismatch, collision reconciliation, and NPC state.

The optional real-GNS gate now loads the sample's actual Animation catalog,
hydrates trusted client code, carries a real `W` platform event through the
default ActionMap into authoritative Character movement, resolves a generic
Luau-authorized action with pinned root motion, then performs a transport
disconnect and verifies control revocation, Player removal, default Character
destruction, failed client status, and cleared `LocalPlayer`.

The packaged gate makes a temporary copy of `FirstCompleteGame` and adds only
role-specific proof Scripts to that copy; the saved sample stays unchanged. It
builds one canonical package and launches separate GNS server/client processes
for both graphical and headless clients. Both variants require trusted
`LocalPlayer`/Character materialization, default ActionMap movement observed on
the server, generic server Luau action authorization, authoritative action
resolution and expiry, 0.87 m pinned root motion, presentation start/stop,
both sample NPC Characters, and server-observed disconnect teardown. The
graphical variant additionally requires the shipped camera to be Scriptable.

One requested-host MSVC Release admission run, including Player creation,
baseline registration, default server Character assembly, ClientReady,
Remote/GCHR peer registration, and control binding, measured:

| Ready peers | Total ms | ms/peer | allocator calls | control binds |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.439 | 0.439 | 2,020 | 1 |
| 32 | 29.005 | 0.906 | 404,364 | 32 |
| 100 | 306.422 | 3.064 | 3,613,481 | 100 |
| 500 | 13,077.4 | 26.155 | 43,649,262 | 500 |

The 500-peer admission figure intentionally includes the current all-peer
structural baseline, so it exposes the expected quadratic pressure that future
3E relevance must remove rather than hiding it as socket latency. Admission is
bounded lifecycle work, not steady state. After warmup, the Luau command bridge
measured 0.369/0.374/0.365/0.389 microseconds per call for 1/10/100/500
Characters across 120 ticks and recorded zero general allocator calls for all
60,000 calls at 500 Characters. This is one bounded policy read per active
Character, not 30,000 scheduled Luau tasks per second.

The unchanged 500-moving-Character 3C Release result remains 762,640 B/s and
740 messages/s including input (759,040 B/s and 680 state frames/s), equal to
the established baseline rather than regressing to 3B's 3,363,600 B/s and
30,060 messages/s. Runtime CFrame updates still create zero structural CFrame
operations and zero authoring journal records. Offline mode allocates no
GameSession, transport, replication, Remote, or GCHR manager and retains the
existing FirstCompleteGame/ten-Play-Stop suite.

## Failure behavior, limits, and observability

Malformed, wrong-version, out-of-order, duplicate, reserved-field, trailing,
wrong-nonce/epoch/Player, premature-gameplay, timeout, capacity, scheduler, and
materialization failures disconnect or fail the session without partial Player
authority. Limits negotiate downward. GSES frames are at most 256 bytes, a
session has at most 512 peers, the default handshake timeout is 600 simulation
ticks, and existing GCHR/Remote/replication/scheduler bounds remain unchanged.

Saturating `GameSessionMetrics` separate transport connections, accepted and
ready peers, handshake/pre-acceptance/protocol rejects, timeouts, Player
creation/removal, and Character control binds/revocations. Existing subsystem
metrics remain the source for bytes, messages, queue pressure, prediction,
state batching, and reconciliation cost.

## Explicit deferrals

External account authentication/tickets, discovery, matchmaking, server
registration, migration, persistence, spatial interest/streaming, adaptive
cadence, listen-server UX, and distributed game-code synchronization are not
implemented. `gargantuan-node`, `gargantuan-studio`, `gargantuan-mcp`, and
`gargantuan-telemetry` required no 3D change. Realtime movement remains entirely
inside the game server.

Foundation 3E should first introduce peer-specific spatial relevance for
Characters and global structural baselines, then measure relevance churn and
adaptive publication cadence without delta dependency chains. Region/portal
handoff, distributed ownership, matchmaking, and account tickets remain later
layers and must reuse this accepted-peer/Player/control lifetime rather than
invent another connection identity.
