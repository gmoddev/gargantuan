---
status: current
owner: networking
last_verified: 2026-09-03
related_code:
  - include/gargantuan/network/GameSession.hpp
  - include/gargantuan/network/CharacterNetwork.hpp
  - include/gargantuan/network/CharacterProtocol.hpp
  - include/gargantuan/services/CharacterControlService.hpp
  - src/network/GameSession.cpp
  - src/network/CharacterNetwork.cpp
  - src/network/CharacterProtocol.cpp
  - src/network/RemoteManager.cpp
  - src/network/Transport.cpp
  - src/services/CharacterControlService.cpp
  - tests/GameSessionTests.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/RemoteTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3E.1

Foundation 3E.1 hardens the production session and Character paths established
by 3D and 3E. It does not replace relevance, add adaptive cadence, or begin
3F. The governing rule is that state becomes live only after its owning
subsystem and the next fallible boundary have accepted it. If rollback is not
safe after reliable structural state advances, the affected peer becomes
terminal before any further gameplay work.

## Session state and ownership

`GameSession` is one-shot. Its exact states are `Created`, `Starting`,
`Listening`, `Connecting`, `Accepted`, `Ready`, `Closing`, `Closed`, and
`Failed`. `Start` is valid only from `Created`. `Stop` is idempotent and ends in
`Closed`; neither `Closed` nor `Failed` may restart. Reconnect constructs a new
session and receives a new transport `ConnectionId`, session epoch, replication
epoch, Player association, control epoch, and materialization state.

```text
Created -> Starting -> Listening/Connecting -> Accepted -> Ready
               |              |                   |         |
               +--------------+-------------------+---------+-> Closing
                                                                  |   |
                                                               Closed Failed
```

`Accepted` is a client bootstrap state: the trusted Player and replica root
exist, but runtime callbacks are not attached. `Ready` means `ClientReady` was
serialized and admitted by the scheduler after all client managers, graph
state, and the scoped control callback lease were acquired. Status is never
used as a proxy for resource ownership. In particular, `GetClientDataModel`
uses an explicit `Accepted || Ready` predicate, and `Failed` owns no exposed
replica or runtime attachment.

Server peers use the smaller internal lifecycle `TransportConnected ->
Accepted -> Ready`. Peer teardown removes the peer instead of retaining a
terminal phase. A scheduler, protocol, replication, Remote, GCHR, or
materialization failure attributable to one server connection calls the
peer-terminal path. A fatal transport endpoint or shared authoritative
subsystem failure calls the session-terminal path. On the client, its one
server peer is the session, so peer-terminal failures become session-terminal.

### Resource ownership table

| Resource | Acquired by | Commit point | Released by | Failure behavior |
| --- | --- | --- | --- | --- |
| transport endpoint | `GameSession::Start` | `IGameTransport::Start` succeeds | `Stop` / `FailSession` | session-terminal |
| scheduler peer | `OnConnected` / server acceptance | `NetworkScheduler::RegisterConnection` succeeds | `TearDownPeer` | peer-terminal |
| replication peer | `AcceptServerPeer` | `ReplicationCoordinator::AddPeer` succeeds | `TearDownPeer` | peer-terminal |
| relevance peer | `AcceptServerPeer` | `ReplicationRelevance::AddPeer` succeeds | `TearDownPeer` | peer-terminal |
| Remote peer | client bootstrap / server ready | `RemoteManager::AddPeer` succeeds | `TearDownPeer` | peer-terminal |
| Character network peer | client bootstrap / server ready | predicted/authoritative `AddPeer` succeeds | `TearDownPeer` | peer-terminal |
| authoritative `Player` | `Players::CreateSessionPlayer` | native replication/relevance association and bootstrap scheduling succeed | `TearDownPeer` / Player policy | exact rollback emits coherent removal |
| control bind | `SynchronizeServerGraph` | `AuthoritativeCharacterNetwork::BindControl` queues the reliable bind | replacement / peer teardown | old bind is revoked before replacement; failure terminates peer |
| CharacterControl attachment | `GameSession` start/bootstrap | `CharacterControlService::AttachRuntime` returns a scoped lease | lease RAII / session teardown | cannot detach another generation |
| client runtime attachment | `AttachClientRuntime` | `ClientReady` scheduler admission succeeds | `Stop` / `FailSession` | all candidate state rolls back before `Failed` |
| LocalPlayer binding | trusted client bootstrap | exact accepted ObjectId and PlayerId validate and `SetTrustedLocalPlayer` succeeds | client teardown | never inferred from replica order |
| materialization view | `ReplicationCoordinator` | reliable structural scheduler admission succeeds, or peer immediately terminates | `TearDownPeer` | no live peer survives an undelivered advanced view |

External DataModel signals are stored `SignalConnection` leases. The session
disconnects them before destroying managers or Players. `CharacterControlService`
owns one explicit runtime attachment slot. `RuntimeAttachment` is move-only and
contains a weak owner plus a 64-bit attachment generation; its destructor
clears callbacks only when the generation still matches. Overlapping owners
are rejected. Registered game action definitions remain service-owned and are
replayed into a newly attached manager; releasing a runtime lease does not
erase the action catalog or policy.

Duplicate semantics are deliberate: `AddPeer`, `RegisterCharacter`, and
`RegisterRemote` reject duplicate ownership; materialization marking is
idempotent only where the owning manager documents it; `BindControl` creates a
fresh control epoch; and runtime attachment rejects overlap rather than
silently replacing an owner.

## Failure atomicity

`FailPeer` rejects new work, removes the peer from session lookup, revokes
Character control and removes GCHR/prediction state, terminates Remote state,
removes relevance and replication state, cancels the scheduler connection,
removes the authoritative Player, and finally closes the transport connection.
It does not fail unrelated server peers. `FailSession` first enters `Closing`,
releases every callback, peer, manager, replica, presentation track, prediction
history, pending action, LocalPlayer binding, and raw runtime pointer, stops the
transport, retains only the first bounded diagnostic (512 bytes), and finally
publishes `Failed`.

Terminal callbacks from `RemoteManager` and both Character managers enqueue a
bounded failure record for the next session safe point. No network thread calls
game Luau or tears down mutable world state. Default gameplay action rejection
is a semantic result, not a peer failure.

Client bootstrap is an acquisition transaction:

```text
trusted structural replica and LocalPlayer
    -> RemoteManager + peer
    -> PredictedCharacterNetwork + peer
    -> current materialization graph
    -> scoped CharacterControl callbacks
    -> ClientReady encoding
    -> scheduler admission
    -> publish Ready
```

Any failure runs the same rollback. A source-private one-shot test seam injects
failure after transport start, scheduler registration, replication creation,
LocalPlayer resolution, both gameplay peer registrations, callback attachment,
graph synchronization, ClientReady serialization, and ClientReady admission.
It is not installed as a public header, runtime option, protocol field, or Luau
surface.

Server admission registers the scheduler, creates a candidate Player, and uses
that exact identity to acquire relevance and replication ownership in one
dependency-closed baseline. Only accepted reliable baseline and GSES
acceptance submissions commit the peer to `Accepted`. A failure at any later
acquisition removes the candidate through the ordinary coherent
`PlayerRemoving` path; `PlayerAdded` is therefore never an orphan
notification. The single-baseline order preserves the bounded 3E admission
shape instead of preparing a second full structural transition per peer.

`GameSession::Poll` processes at most 128 transport events per call. This is a
session work bound, not packet loss: unconsumed transport events remain queued
for the next simulation turn. The server flushes an already queued baseline at
most once for that peer in a step, then may admit the peer's current relevance
and journal catch-up behind it for the next flush. This prevents an admission
burst from running thousands of Player mutations ahead of an early peer's
committed journal cursor. A 192-peer burst regression test is deliberately
larger than the old journal-expiration threshold and reaches readiness without
rebaseline, peer loss, or cursor mismatch.

## Reliable commit rules

`QueueReplicationFrame` has two fallible layers: serialization returns an
outer `expected`, while scheduler admission is the inner
`SchedulerSubmitResult`. Both must succeed. The coordinator currently advances
its journal cursor and Known/Relevant candidate view while preparing a frame;
there is no application-level acknowledgement. If local reliable admission
then fails, the affected peer is immediately terminal and its entire view is
destroyed. No live peer can produce a later delta from an undelivered cursor.

Remote argument visibility and publication advance only after the structural
submission was accepted. Reliable Remote scheduler exhaustion terminates the
peer and completes pending RemoteFunctions through existing bounded terminal
outcomes. Reliable GCHR submission uses the same rule: the required reliable
state flag clears only after accepted publication (or when no peer has the
Character materialized), and rejection invokes the Character terminal handler.

Unreliable state remains newest-wins and lossy. For local Character input, the
client validates capacity and intent, asks the scheduler to admit the command,
and only then mutates prediction, input sequence, or history. A dropped input
therefore produces no predicted movement or replay entry. Terminal submission
also stops the owning session through the same failure callback.

## Actions and root motion

GCHR protocol version 4 adds the reliable `ActionResult` message. Every admitted
action request has its own `(Character, ControlEpoch, ActionSequence,
RequestedActionToken)` result, accepted flag, and optional authoritative action
state. The client matches and removes exactly that pending request; multiple
same-tick results cannot overwrite one another. Rejection of request B does not
clear already active action A. Control revoke, replacement, unmaterialization,
disconnect, or manager teardown clears all pending request and presentation
state, so an old result cannot attach to a new control lifetime.

An action registered with duration `D` applies intervals `[StartTick,
StartTick + D)`. Both authority and prediction clamp evaluation to the end tick,
apply the final interval, publish the final authoritative state, and then clear
the active action. Duration 1 therefore applies one interval, not zero. Addition
overflow is rejected before action activation.

## Materialization generation lifetime

GCHR v4 widens `CharacterMaterializationEpoch` on the wire from 16 to 64 bits.
The modern state-frame header is 34 bytes. The peer-wide epoch begins at one and
advances without wrap for every accepted Character materialize or
unmaterialize operation, in deterministic ObjectId order. Server and client
perform the same committed transition sequence. Exhaustion returns failure;
it never wraps to a value that could validate an old packet. On a server that
failure terminates only the affected peer; on a client it terminates the one
session. Stale reliable or unreliable state with a different epoch is consumed
as stale and cannot restore transform, interpolation, prediction, or actions.

Per-transition increments are retained instead of introducing a redundant
generation domain. At 64 bits this has practical live-session lifetime even
under pathological churn. Future atomic batch generation remains an optional
optimization only if measurements justify changing this wire contract.

`RegisterServerObject` inserts a Character or Remote into the session registry
only after `RegisterCharacter` or `BindRemoteManager` succeeds. Character
capacity is 4,096. A 4,097th Character aborts session acquisition without
poisoning a later exact-capacity session. Remote binding first registers with
the candidate manager, then changes the Instance manager/lifetime token, so a
failed bind cannot precommit session membership.

## DevelopmentLocal security

`DevelopmentLocal` remains a label, not external account authentication.
GameSession configuration accepts it only for parsed IPv4 loopback addresses
in `127.0.0.0/8` or exact IPv6 `::1`. Prefixes, suffixed DNS names, and LAN or
public addresses fail. The Player CLI requires
`--allow-insecure-development-network` to override that restriction and emits
a bounded `[Network:Security]` warning. The override changes endpoint policy;
it does not authenticate the peer or grant Node identity. Short-lived join
tickets, server identity, and account authentication remain future control
plane work.

## Memory bounds

The session adds one flush-state bit per peer and a peer-failure map bounded by
the existing 512-peer session ceiling. The callback owner is one shared
attachment state plus one 64-bit generation, independent of Character count.
The materialization epoch is now a 64-bit value per peer and per materialized
Character record; it adds six wire bytes per GCHR state frame but no new
unbounded collection. Per-request action results reuse the existing maximum of
eight pending actions per Character and the 64-entry prediction/resolution
history bound. Replication continues to use the documented terminal-peer
commit rule, so 3E.1 adds no retained full-peer prepared-state copy or pending
replication transaction queue.

## Validation and remaining work

The deterministic tests cover scoped callback acquisition and overlap,
unstarted/running/failed destruction, one-shot Start/Stop, ten client bootstrap
failure points, partial server registration, bounded 192-peer burst admission,
terminal peer teardown after an injected reliable structural scheduler rejection,
100 accepted connection churn,
100 complete packaged-policy session lifecycles, two-client identity/control
and failure isolation, Character replacement, no-Character readiness, Remote
delivery, accepted/rejected action results, scheduler rejection before
prediction, duration 1/2/4 root motion, stale materialization frames, 64-bit
generation encoding/exhaustion, relevance leave/reentry, custom policy, NPC,
offline, and malformed/truncated protocol input.

The Clang 19 `-O3` release benchmark on the remote worker measured:

| Admission peers | 3E.1 time | 3E.1 allocations | Change from 3E time | Change from 3E allocations |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 2.575 ms | 1,494 | +531% (cold-start dominated) | -35.7% |
| 32 | 17.244 ms | 206,452 | -11.7% | -27.2% |
| 100 | 150.620 ms | 2,101,189 | -11.4% | -20.9% |
| 500 | 5,486.37 ms | 60,944,284 | +30.9% | -11.5% |

The 500-peer run spans nine bounded poll/step turns rather than draining the
whole burst in one call. The timing increase is therefore explained work
scheduling, while allocator pressure falls rather than returning to the pre-3E
pathology. Terminal teardown measured 0.394 ms/70 allocations for one peer and
25.774 ms/349,372 allocations for 32 peers failed independently over time.
The representative 500-peer relevance update measured 1.856 ms and 14,113
allocations.

At 20 Hz, 500 moving and relevant Characters produced 763,120 state bytes/s in
680 state frames/s, or 766,720 bytes/s and 740 messages/s including input. The
4,080-byte/s increase from GCHR v3 is exactly six additional materialization
epoch bytes across 680 state frames/s; cadence and batching are unchanged. With
50 of 500 Characters relevant, the result was 76,720 state bytes/s in 80 state
frames/s, or 80,320 bytes/s and 140 messages/s including input. This is the
same 480-byte/s wire-width delta from the 3E sparse result.

3E relevance policy, bounded closure, hysteresis, structural materialization,
and the 20 Hz selected-state cadence are unchanged. Foundation 3F should first
reduce remaining global Player/Character-shell baseline allocations, then add
importance/cadence tiers for already relevant objects. It must not weaken the
3E boolean materialization boundary or expose arbitrary client pins.
