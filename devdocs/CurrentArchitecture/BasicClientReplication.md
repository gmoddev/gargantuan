---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/network/Replication.hpp
  - include/gargantuan/network/ReplicationProtocol.hpp
  - include/gargantuan/network/ReplicationCoordinator.hpp
  - include/gargantuan/network/ReplicationTransport.hpp
  - include/gargantuan/network/ReplicaApplier.hpp
  - include/gargantuan/network/Scheduler.hpp
  - src/network/ReplicationProtocol.cpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReplicationTransport.cpp
  - src/network/ReplicaApplier.cpp
  - src/network/Scheduler.cpp
  - tests/ReplicationTests.cpp
  - tests/ReplicationBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Basic client replication

## Implemented boundary

Networking Foundation 6 implements the first server-authoritative client replica:

```text
authoritative DataModel + ChangeJournal
    -> ReplicationCoordinator + per-peer ReplicationView
    -> ReplicationFrame semantic operations
    -> bounded Gargantuan binary codec
    -> production NetworkScheduler
    -> IGameTransport (simulator or GNS)
    -> ReplicaApplier
    -> client replica DataModel
```

The authoritative graph is never mutated by received replication bytes. The
coordinator is a server-side projection only. The applicator has no upstream
mutation API and operates on a distinct client graph under change-journal
suppression. Application remotes and Character realtime prediction use separate
implemented protocols; general physics ownership remains future work. See
`LuauRemotes.md` and `CharacterNetworkingFoundation.md`.

## Per-peer state and baselines

`ReplicationCoordinator` owns one `PeerState` per generation-safe
`ConnectionId`. Each state contains policy-desired objects, a `ReplicationView`,
an authoritative `ChangeCursor`, and a reliable replication sequence. The
coordinator maintains one journal-refreshed authoritative object catalog;
`AddPeer` projects the peer selection plus bounded dependency closure instead
of recapturing the whole DataModel per peer. A baseline always starts at
reliable sequence 1 and is identified by a nonzero `ReplicationEpoch`.

Foundation 3I makes each catalog entry an immutable revisioned per-object
structural publication template. Peer baseline/enter operations reference that
description and retain only peer-specific soft-reference nil patches until
encoding. The encoder still writes ordinary GRPL v1 `Publish` bytes; final
frames are not shared or cached. See `ReplicationFoundation3I.md`.

The receiver accepts a baseline only when its schema manifest exactly equals
the active ordered set of `(SchemaId, DefinitionVersion, definition kind)`.
Class, custom-class, custom-enum, and extension versions therefore fail closed
before object publication. Reconnect establishes a strictly newer epoch and a
fresh baseline. Old-epoch operations, replayed baselines, stale sequences, and
out-of-order reliable sequences cannot mutate the replica.

The legacy direct coordinator API still publishes the whole source scope by
default for isolated callers. Production `GameSession` supplies the server-only
selection from `ReplicationRelevance`. See
`CharacterReplicationFoundation3E.md` for spatial policy, owner requirements,
dependency closure, and transition bounds.

## Replication operations

Protocol version 1 defines distinct semantic operations for:

- `Publish`, carrying class identity/version, name, parent, native properties,
  Attributes, Tags, extension state, and custom-class state;
- native or custom-class property update;
- extension property update;
- reparent;
- Attribute set/remove;
- Tag add/remove;
- `Unpublish`, which removes peer visibility without destroying the server object;
- `Destroy`, which represents authoritative lifecycle termination.

`ChangeJournal.Sequence` never crosses this boundary. Journal records are
filtered against the peer view and projected into a separate epoch and reliable
sequence domain. A newly created object is published from its current complete
state, coalescing later records for that object in the same batch. The
`Destroyed` property journal notification is coalesced with its terminal
`ObjectDestroyedChange` into the one `Destroy` opcode.

Hard references and non-nullable references enter dependency closure and may
name only objects in the resulting materialized view. Nullable soft references
outside the view encode nil and receive deterministic fixups on target
enter/leave. Unpublishing removes the peer replica but not authoritative state;
republishing sends complete current state and never an off-interest journal
backlog.

## Binary protocol contract

The gameplay replication codec is Gargantuan-owned and independent from JSON,
Snapshot JSON, journal JSON, nlohmann, Glaze, GNS, and native struct layout. All
integers and IEEE floating-point bit patterns are little-endian.

The fixed 36-byte header is:

| Field | Width | Contract |
| --- | ---: | --- |
| magic | 32 bits | `GRPL` |
| version | 16 bits | `1` |
| kind | 8 bits | baseline or incremental |
| reserved | 8 bits | must be zero |
| epoch | 64 bits | nonzero replication epoch |
| reliable sequence | 64 bits | nonzero, strictly ordered |
| schema count | 32 bits | at most 4,096 |
| operation count | 32 bits | at most 65,536 |
| payload bytes | 32 bits | must equal the exact remaining bytes |

Frames are capped at 8 MiB. Every operation begins with a one-byte opcode.
Strings and collections use explicit 32-bit lengths/counts and are constrained
by the existing protocol, Snapshot, Attribute, Tag, custom-class, and extension
ceilings. `ObjectId` is encoded as slot plus generation; `SchemaId` is encoded
as its two stable 64-bit halves. Optional values use a validated zero/one marker.
Schema and operation counts must also fit the remaining payload's minimum
representable size before any count-directed allocation. The writer enforces its
8 MiB budget incrementally rather than materializing an oversized payload, and
allocation failures are normalized at both codec entry points.

`WireValue` has one explicit tag and language-neutral field layout for each of
its 15 semantic alternatives. Decoding rejects unsupported tags, invalid UTF-8,
non-finite or otherwise invalid values, duplicate map keys, duplicate Tags,
duplicate schema identities, invalid IDs, bad versions, invalid optional
markers, count/byte overflow, truncation, trailing data, and unknown opcodes.
Serializer errors are normalized through Gargantuan's `SerializationError`.

The current layout favors a small auditable foundation over final compression.
It intentionally repeats property names and schema identities. Compact indexes,
property masks and compression are future versions of this structural format.
Character realtime state is implemented by the separate bounded `GCHR`
protocol and never enters a `ReplicationFrame`.

## Application and transaction semantics

`ReplicaApplier` separates framing, semantic projection, validation, and live
application. It first decodes a complete bounded frame, applies every operation
to a candidate Snapshot semantic state, runs `ValidateSnapshotSemantic`, and
performs a complete `LoadSnapshot` preflight. Only then may an incremental frame
touch the live client graph.

Incremental commit applies under journal suppression and deferred signals so
observers cannot see an operation prefix. Existing client `Instance` identities
remain stable for ordinary updates and reparenting. If an unexpected commit-time
failure occurs after preflight, the prior validated semantic state is
rematerialized before rejection. Baselines replace the replica only after a
complete successful load. A failed group, schema mismatch, malformed frame,
stale epoch, or sequence error leaves no partial candidate state published.
Candidate lookup, hierarchy closure, and live replica removal use explicit
identity/child indexes so bounded frames do not degrade into operation-count
times object-count scans.

## Scheduler and transport integration

`NetworkScheduler` is now a production implementation of `INetworkScheduler`.
It owns bounded per-connection reliable/unreliable queues, semantic traffic
precedence, per-tick byte/message budgets, unreliable sequenced supersession,
backpressure, cancellation, terminal reliable-backlog exhaustion, and scheduler
statistics. Replication frames enter it as reliable ordered
`StructuralReplication` traffic. Basic replication does not use an unreliable
lane.

The deterministic simulator exercises the production coordinator, codec,
scheduler, transport, and applicator with latency, jitter, bandwidth, injected
unreliable duplication/reordering, queue pressure, disconnect during an
in-flight baseline, stale/replayed frames, and reconnect. The optional pinned
GNS test carries a real baseline over a localhost socket through the same
scheduler and applicator; the prior GNS stall did not reproduce.

## Authority, validation, and limits

Server mutations still originate in the authoritative runtime and its mutation
gateway. Replication bytes confer no `MutationAuthorityContext`, capability, or
server-object access. The receiver validates schema identity/version, object
identity, reference visibility, hierarchy, property type and replication
policy, Attributes, Tags, custom enums/classes/extensions, UTF-8, numeric
finiteness, and all collection limits before publication.

Every full publish validates references in native properties, Attributes,
extension state, and custom-class state against the peer's materialized view.
Objects published together may reference one another because the receiver
constructs the complete group before applying values; a relevance transition
cannot expose an identity hidden from that peer or advance its stream with an
unusable frame.

Resource ownership is explicit: the coordinator owns per-peer views and journal
cursors; the scheduler owns bounded outgoing queues; the transport owns delivery
mechanics; the applicator owns the replica graph. Metrics remain separate:
coordinator counters cover projected objects/operations/bytes/backlog and
invalid references, scheduler counters cover admission/queues/backpressure, and
applicator counters cover applied/rejected frames.

## Performance baseline

`gargantuan_replication_benchmark` measures baseline generation, encode, decode,
scheduler/transport submission, application, and output bytes for 1,000 objects;
`--full` adds 10,000 objects. On the verified Windows x64 MSVC Release build, one
run measured:

| Objects | Bytes | Generate ms | Encode ms | Decode ms | Scheduler/transport ms | Apply ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 81,574 | 1.6888 | 0.2575 | 0.5155 | 0.2035 | 2.8839 |
| 10,000 | 819,574 | 24.1386 | 2.6697 | 5.3960 | 2.4490 | 36.6966 |

This is a repeatable engineering baseline rather than a pass/fail latency
budget. It must not be compared to the future realtime or compact-codec design
without equivalent workloads.

## Deliberately deferred

The following are not part of basic client replication: bounded Luau remotes,
Character realtime state/prediction/reconciliation, general transform or physics
replication, ownership leases, interpolation, spatial interest management,
authentication tickets, negotiated schema-index tables, compression,
production Node integration, and Studio play-session orchestration. The first
two are now implemented as sibling protocols; their existence does not expand
this structural layer's authority.

Foundation 3D composes this unchanged one-way structural layer into
`GameSession`. The server publishes the exact accepted Player identity and
`Player.Character`; the client applies the baseline before trusted
`LocalPlayer` selection and GCHR control activation. Runtime Character motion
still bypasses structural commits, and engine runtime-module source is excluded
from the replicated graph. This composition does not make client replica
mutation an upstream replication path.

Foundation 3E.1 preserves this structural protocol and corrects its production
commit interpretation. `GameSession` checks both serialization and the inner
scheduler admission result. If a reliable frame is rejected after the
coordinator prepared its cursor/KnownObjects state, that peer is immediately
terminal and its entire view is removed before subsequent work. Remote and
GCHR materialization update only after accepted structural admission. No new
application acknowledgement or parallel streaming protocol was added.

Foundation 3F does not alter this structural path. Boolean relevance and
Player/Character-shell visibility remain structural 3E decisions; GCHR
importance exists only after Character materialization. Runtime transforms,
tier history, and presentation samples remain transient and cannot enter a
replication frame or authoring journal. No structural serialization cache was
added because current frames contain peer-view and materialization-specific
state.

Foundation 3H likewise leaves this protocol unchanged. Its derived
`SpatialAddress` and bounded region index provide candidates to 3E before
dependency closure. A region crossing is not an `Unpublish`; only a changed 3E
desired set can create structural enter/leave work. Spatial addresses, region
membership, and candidate lists are neither serialized nor persisted.

Foundation 3I resolves the previously deferred safe sharing boundary. Only
peer-independent authoritative object state is shared. Relevance, known-object
membership, frame/materialization epochs, LocalPlayer, soft-reference patches,
and scheduler admission remain peer-owned. The prepared and owning publication
paths are byte-equivalent and require no protocol-version change.
