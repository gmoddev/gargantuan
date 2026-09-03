# Bounded Luau remotes

## Status

Networking Foundation 7 implements Gargantuan-owned application messaging over
the existing scheduler and `IGameTransport` boundary. Remotes and replication
share connection, scheduling, delivery, and transport contracts, but they do
not share semantic operations:

```text
Replication
    -> ReplicationCoordinator
    -> ReplicationOperation
    -> NetworkScheduler
    -> IGameTransport

Application messaging
    -> RemoteManager
    -> RemoteMessage
    -> NetworkScheduler
    -> IGameTransport

Character realtime control/state
    -> AuthoritativeCharacterNetwork / PredictedCharacterNetwork
    -> NetworkScheduler
    -> IGameTransport
```

A decoded Remote message is application data. It is never interpreted as a
DataModel mutation, schema update, capability grant, execution-domain change,
or authority transfer. Core Character input, action identity, authoritative
state, and reconciliation never use RemoteEvent/RemoteFunction messages; see
`CharacterNetworkingFoundation.md`.

## Instance classes and identity

The canonical runtime schema registers these ordinary Instance classes:

| Class | Delivery contract | Runtime state |
| --- | --- | --- |
| `RemoteEvent` | reliable ordered event | connections only |
| `UnreliableRemoteEvent` | best-effort unordered event | connections only |
| `UnreliableSequencedRemoteEvent` | best-effort newest-wins state | connections and per-session sequence state |
| `RemoteFunction` | reliable bounded request/response | handlers and pending requests |

`RemoteEventBase` and `RemoteBase` are abstract schema classes. A Remote uses
its ordinary generation-safe `ObjectId` plus a per-peer `RemotePublicationId`
as semantic wire identity and publication lifetime. No global
mutable Remote-name registry or pointer identity is used. Connection-local
compact indexes are deliberately deferred because the current bounded frame is
already sufficient and an index would add publication lifecycle state without
demonstrated value.

Remote Instances persist, load, clone, reparent, replicate, and destroy through
the ordinary Instance/schema mechanisms. Only ordinary Instance state is
persisted. Signal connections, peer associations, request IDs, request
callbacks, sequence counters, rate windows, scheduler state, and transport
state remain runtime-only. A clone receives the ordinary new `ObjectId` and
does not inherit networking state.

## RemoteManager ownership

One `RemoteManager` owns application semantics for one trusted server or client
runtime. It owns:

- peer/session association and publication state;
- Remote class validation;
- encode/decode and scheduler submission;
- per-peer/per-Remote admission;
- incoming safe-point dispatch;
- request IDs, deadlines, cancellation, and exactly-once completion;
- per-peer publication and sequence state;
- explicit reliable materialization dependencies; and
- Remote-specific metrics.

It does not own DataModel authority, replication relevance policy, transport
handles, scheduler implementation, physics, schema mutation, or player
identity. A weak lifetime token prevents an Instance or VM teardown from
dereferencing a destroyed manager.

`RemoteManager::HandleTransportEvent` only decodes and enqueues validated
semantic work. `RemoteManager::Pump` is the explicit runtime safe point that
performs Remote/visibility/rate validation and invokes gameplay handlers. No
GNS or simulator callback runs Luau directly.

## Developer-facing API

### Events

All three event classes expose the same initial surface:

```luau
Remote:FireServer(...)
Remote:FireClient(PeerSlot, PeerGeneration, ...)
Remote:FireAllClients(...)

Remote.OnServerEvent:Connect(function(Peer, ...) end)
Remote.OnClientEvent:Connect(function(...) end)
```

`FireServer` is client-runtime only. `FireClient` and `FireAllClients` are
server-runtime only. The server callback receives a read-only provisional peer
table containing `Slot`, `Generation`, and `Epoch`; this is not a `Player` and
does not claim to be one. Event handlers have no return value and create no
request tracking.

Broadcast validates every peer independently. A peer that cannot see the
Remote or an Object argument is rejected without affecting other peers. Payload
reuse is limited to cases where the peer-specific checks already succeeded.
Logical broadcasts and actual peer submissions have separate metrics. The
manager admits at most 512 peers. Before per-peer copies are made, a logical
broadcast must fit the manager's 4096-submission and 16 MiB one-second aggregate
budgets; an invocation that does not fit is rejected as a whole.

### RemoteFunction

The initial request surface is:

```luau
local Results = Function:InvokeServer(...)
local Results = Function:InvokeServerWithTimeout(Seconds, ...)
local Results = Function:InvokeClient(PeerSlot, PeerGeneration, ...)
local Results = Function:InvokeClientWithTimeout(PeerSlot, PeerGeneration, Seconds, ...)

Function:SetServerHandler(function(Peer, ...) return ... end)
Function:SetClientHandler(function(...) return ... end)
```

An invocation is legal only from a yieldable Luau coroutine. The engine Main
coroutine is rejected rather than blocked. Success returns the handler values.
A non-success terminal result resumes with:

```luau
nil, StatusName, SanitizedMessage
```

Nested RemoteFunction calls are supported: a handler may suspend on another
RemoteFunction without blocking unrelated handlers. Each nested request keeps
its own ID and deadline. Arbitrary application-level cyclic dependencies still
have no distributed deadlock detection; finite deadlines terminate them.
The exact `ScriptSecurityContext` at scheduling or suspension is restored around
every `task.spawn`, `task.defer`, `task.delay`, `task.wait`, RemoteFunction, and
signal-wait continuation, so yielding cannot upgrade a game coroutine to the
engine thread's ambient authority. `task.wait` is rejected inside an incoming
RemoteFunction handler because that boundary supports only nested
RemoteFunction suspension; a rejected handler cannot remain scheduled and run
after its terminal error. The shared script task queue is capped at 65,536
entries.

The handler API uses methods rather than Roblox callback properties. Peer
slot/generation arguments and handler-setter spelling are intentionally
provisional until the complete `Players` service exists. This is not a claim of
full Roblox API parity.

## Event semantics

Reliable events use `ReliableOrdered` delivery and `ReliableApplication`
traffic. Ordering is the scheduler/transport order for the declared Remote
logical stream while the connection remains healthy. Terminal disconnect ends
the delivery promise. Reliable admission failure is observable to the local
caller; it does not create an unbounded fallback queue.

Unreliable events use `UnreliableUnordered` and `EphemeralApplication`. They may
drop, duplicate, or reorder. `RemoteManager` never retries them. The encoded
frame must fit both the 256 KiB Remote frame ceiling and the smaller negotiated
unreliable datagram limit. The scheduler bounds unreliable work to the current
tick and preferentially drops it under congestion.

An `UnreliableSequencedRemoteEvent` is one state channel per Remote, peer,
connection session, and publication lifetime. It uses its own
`RemoteEventSequence` and `RemotePublicationId` domains and the
scheduler's `RemoteEventOrder`; it never reuses replication sequences,
`ChangeJournal.Sequence`, or packet numbers. A newer queued value may supersede
an older unsent value. Duplicate or reordered values at or below the latest
accepted sequence are discarded. Disconnect, unpublication, and republish
clear the relevant sequence state. A republished Remote receives a newer
publication ID, so an old high sequence cannot supersede or poison the new
publication's low sequence.

## Request lifecycle

Each peer owns a monotonic nonzero `RemoteRequestId` sequence. A request record
contains the local connection identity, Remote identity, ID, monotonic deadline,
and completion callback. The default deadline is 10 seconds and the maximum is
30 seconds. Zero, negative, non-finite Luau values, and longer deadlines are
rejected.

Every admitted request reaches exactly one terminal state:

| Terminal state | Cause |
| --- | --- |
| success | correlated validated response |
| timeout | local monotonic deadline expires |
| cancelled | explicit native cancellation |
| disconnected | peer/session closes |
| remote error | bounded structured handler rejection |
| protocol rejected | Remote lifecycle or protocol validation invalidates it |
| resource rejected | scheduler or bounded resource policy rejects it |

Completion removes tracking before invoking the callback. Duplicate,
nonexistent, mismatched, late, old-connection, and post-timeout responses cannot
resume a coroutine. Cancellation sends a best-effort reliable cancellation
control message after completing locally. Any unsent materialization-dependent
request frame is removed atomically at every terminal state, so cancellation or
timeout cannot release the original request later. Disconnect terminates outgoing
requests, removes incoming requests, drops queued and deferred work, clears
sequence/rate state, and removes the peer.

A terminal scheduler result is preserved in `RemoteSendResult`, drained into the
same peer teardown path, and exposed to the host through a terminal callback.
Reliable queue exhaustion therefore resumes older pending requests with
`disconnected` exactly once instead of leaving them until their deadlines.

Cancellation or a peer-selected short deadline revokes reply eligibility but
does not immediately release the handler-work admission slot. The slot remains
charged until the handler acknowledges termination or the fixed 30-second work
lease expires. This prevents start/cancel and short-deadline churn from evading
the per-peer concurrent-handler ceiling. Native handlers that launch work
outside the runtime must make that work cooperative with their own cancellation
policy.

Handler exceptions become the remote error code `handler_error` with the
sanitized text `Remote handler failed`. Native stacks, filesystem paths,
pointer values, backend diagnostics, and arbitrary exception text are never
sent. Richer detail may remain in local `[Networking:Remote]` logs.

## Argument model

Remote arguments reuse the bounded Gargantuan-owned `WireValue` semantic model
and shared primitive binary helpers. The Luau bridge accepts:

- nil, boolean, signed 32-bit integer, and finite number;
- valid UTF-8 strings;
- `Vector2`, `Vector3`, `Color3`, `UDim`, `UDim2`, and `CFrame`;
- registered `EnumItem` values; and
- visible, live Instance references.

The native codec also carries `WireFloat` and frozen-schema enum values.
Schema-enum values are not yet exposed as a distinct Luau value and are rejected
before a Luau gameplay handler runs. An unsupported response value resumes its
caller with `protocol_rejected` rather than stranding the coroutine. Tables,
containers, functions, threads, callbacks, arbitrary userdata, and cyclic data
are unsupported. The bridge never serializes execution context, capabilities,
or trust metadata.

## Binary protocol version 2

The Remote protocol is Gargantuan-owned, language-neutral little-endian binary.
It is independent of GNS, Box3D, nlohmann, Glaze, compiler ABI, and C++ struct
layout. The fixed 52-byte header is:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `GRMT` |
| 4 | 2 | protocol version (`1`) |
| 6 | 1 | message kind |
| 7 | 1 | zero reserved |
| 8 | 8 | Remote `ObjectId` slot/generation |
| 16 | 8 | peer-local `RemotePublicationId` |
| 24 | 8 | `RemoteRequestId`, or zero |
| 32 | 8 | `RemoteEventSequence`, or zero |
| 40 | 4 | request deadline milliseconds, or zero |
| 44 | 2 | argument count |
| 46 | 2 | zero reserved |
| 48 | 4 | payload bytes |

Version 2 opcode assignments are `0 ReliableEvent`, `1 UnreliableEvent`,
`2 SequencedEvent`, `3 Request`, `4 Response`, `5 RequestError`, and
`6 Cancellation`. Unknown versions, opcodes, nonzero reserved fields, invalid
field combinations, count/length mismatches, invalid tags, invalid UTF-8,
non-finite values, invalid IDs, and trailing bytes fail closed.

`DecodeRemoteMessage(span<byte>)` is independent of sockets, GNS, a DataModel,
and a Luau VM. `tests/fuzz/RemoteProtocolFuzz.cpp` is a dependency-free
libFuzzer-ready adapter. Deterministic corpus tests cover empty/truncated input,
all kinds, maximum and oversized values, invalid version/opcode/count/tag/UTF-8,
invalid references/IDs/publications, malformed request/sequence fields,
oversized errors, and trailing data. Version 1 frames are incompatible and fail
closed rather than inheriting publication state implicitly.

## Visibility and replication dependency

Remote invocation requires all of these checks:

1. the generation-safe Remote `ObjectId` exists in the manager;
2. the Remote class matches the wire message kind;
3. the peer's current session has published and materialized the Remote and the
   message carries that publication's current nonzero `RemotePublicationId`;
4. `ReplicationView`-supplied visibility authorizes the Remote; and
5. every Object argument resolves to a live current-generation Object visible
   to that peer.

Guessing a hidden ObjectId therefore does not reveal whether an authoritative
object exists and cannot reach gameplay code.

For a reliable outgoing message, a visible live Object that is not yet
materialized creates an explicit bounded dependency. The message stays in the
per-peer deferred reliable budget until `MarkMaterialized`, invalidation, or the
30-second hard ceiling. An unreliable message is rejected instead of waiting.
If an Object is unpublished or destroyed before dispatch, validation fails and
the Remote cannot resurrect it.

This dependency guarantees only that a referenced Object is resolved before
the handler sees it. Gargantuan promises no general causal ordering between
Remote messages and replication on unrelated channels. A property write
followed by `FireClient` does not imply the property update is observed first.

## Execution domains and authority

Trusted runtime context, not script name, ancestry, source path, or payload,
controls direction:

| Runtime | Allowed developer direction |
| --- | --- |
| client | `FireServer`, `InvokeServer*`, `SetClientHandler` |
| server | `FireClient`, `FireAllClients`, `InvokeClient*`, `SetServerHandler` |
| PreRun | none |
| Studio/editor | none through the game Remote path |
| Core | none through the developer-facing direction methods |

Ordinary server/client game scripts do not need a gratuitous privileged
capability token to use Remotes. Incoming handlers run with a host-constructed
server/client context containing only network send/receive capabilities. A
payload cannot select that context, and coroutine continuation restores the
same host context instead of inheriting `CoreTrusted`. EditorHost remains a
separate protocol.

The engine validates protocol and transport correctness. Gameplay code must
still authorize meaning: price, proximity, ownership, inventory membership,
cooldowns, and similar rules remain application responsibilities. Successful
decode does not make a client's claim trustworthy.

## Resource policy

The initial hard ceilings are intentionally small in number:

| Resource | Ceiling |
| --- | ---: |
| encoded Remote frame | 256 KiB |
| arguments | 32 |
| UTF-8 string | 16 KiB |
| error code / message | 128 B / 1024 B |
| default / maximum request deadline | 10 s / 30 s |
| calls per peer per Remote per second | 256 |
| total calls per peer per second | 1024 |
| concurrent incoming handlers per peer | 64 and negotiated request ceiling |
| manager peers | 512 |
| manager calls per second | 8192 |
| aggregate concurrent incoming handlers | 4096 |
| aggregate in-flight outgoing requests | 8192 |
| aggregate queued dispatch | 8192 messages / 32 MiB |
| aggregate broadcast per second | 4096 submissions / 16 MiB |
| generated reliable terminal messages per second | 4096 / 32 MiB |
| shared Luau task queue | 65,536 tasks |

Negotiated `NetworkLimits` additionally bound reliable/unreliable message size,
reliable queue bytes, in-flight outgoing requests per peer, decoded bytes per
peer tick, messages per peer tick, send/receive bytes per tick, and unreliable
datagram size. Deferred reliable bytes are accounted per peer. Incoming decoded
queues are accounted per peer and against the manager-wide ceiling. The runtime
safe point processes at most the native per-tick message ceiling and divides a
pump pass fairly across peers with queued work. Every Request frame, including
an unknown or invalid request, consumes admission before a reliable rejection
can be generated; over-rate requests are dropped and terminate at the caller's
finite deadline.

Individual Luau instruction preemption is not introduced by this milestone;
the existing Luau runtime has no handler-specific instruction-budget facility.
Remote-created handler count, dispatch count, request lifetime, and nested
request trees are nevertheless bounded. A future general script watchdog must
remain a scripting-runtime policy rather than Remote protocol state.

## Statistics

`RemoteMetrics` is separate from replication, scheduler, and transport
statistics. Saturating counters cover accepted/rejected reliable events,
accepted/dropped unreliable events, accepted/superseded/stale sequenced events,
requests started/completed/timed out/cancelled, handler errors, rate/visibility/
protocol/resource rejections, and logical/per-peer broadcasts. Gauges expose
queued dispatch bytes/messages, deferred reliable bytes/messages, and in-flight
requests.

## Verification boundaries

The deterministic simulator tests latency/order delivery, forced loss,
sequence replay/reorder rejection, materialization dependency, hidden Object
references, queue/count amplification, rate limiting, request success, nested
requests, sanitized handler failure, timeout, cancellation, disconnect,
unpublication/destruction, broadcast isolation, and reconnect/epoch rejection.

The optional real GNS test proves localhost reliable/unreliable/sequenced
messages in both directions, client and server requests, a 200-event bounded
reliable workload, disconnect with a pending request, reconnect, new connection
generation, stale old-session rejection, and mixed baseline/publication plus
Remote event/request routing through one production scheduler and session. It
uses no Internet service.

Foundation 3D's `GameSession` is now that production session owner. A Remote
peer is registered only after GSES acceptance, exact Player materialization,
trusted `LocalPlayer` selection on the client, and `ClientReady`; disconnect
removes Remote, GCHR, replication, scheduler, and Player association state from
the same `ConnectionId` lifetime. Remote payloads still cannot select a Player,
Character, or script security context.

Foundation 3E.1 installs the Remote manager terminal callback into that session
owner. Every nonaccepted reliable scheduler result, including backlog
exhaustion, fails only the affected server peer (or the client's one session),
removes its Remote peer state, and completes pending RemoteFunctions through
bounded disconnect/protocol/resource outcomes. Remote
materialization is committed only after the corresponding reliable structural
frame was admitted. Teardown never exposes scheduler implementation details to
ordinary Luau.

## Deliberately deferred

This Remote milestone does not implement matchmaking, Node integration, Studio
multiplayer play orchestration, general transform/physics replication, network
ownership, rollback, streaming/interest management,
voice, HTTP/service networking, or backend messaging. Character realtime
prediction/reconciliation and production session composition are sibling
systems and deliberately do not expand Remote authority.
