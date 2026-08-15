---
status: current
owner: engine
last_verified: 2026-08-15
related_code:
  - include/gargantuan/runtime/ProtocolInput.hpp
  - src/runtime/ProtocolInput.cpp
  - src/runtime/WireCodec.cpp
  - src/runtime/Snapshot.cpp
  - src/runtime/WireJournal.cpp
  - src/runtime/InProcessReplicationSession.cpp
  - src/runtime/MutationGateway.cpp
  - src/assets/InstanceSerialization.cpp
  - src/editor/EditorHost.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Protocol input hardening

## Implemented boundary

Gargantuan's reusable protocol-shaped inputs now share native safety ceilings
and fail-closed validation before they can publish or incrementally mutate a
world. This is the hardened substrate for a future networking protocol; it is
not itself a game-network protocol.

```text
bounded bytes / structured DTO
  -> structural validation
  -> identity, schema, scope, and semantic preflight
  -> coherent candidate or batch commit on Main
  -> observable notifications
```

The boundary covers JSON persistence, snapshot and wire-journal decoding,
EditorHost requests, structured `WireValue` use, snapshot materialization, and
in-process replication application. Serialized documents are byte- and
nesting-checked before JSON parsing, then node-, field-, string-, shape-, count-,
integer-, and value-checked after parsing. All unsigned-to-32-bit and signed
integer conversions validate range before narrowing. Floating-point protocol
values must be finite.

The hard native ceilings are safety limits rather than future session budgets:

| Resource | Current ceiling |
| --- | ---: |
| Snapshot, journal, or persistence document | 8 MiB |
| EditorHost request | 1 MiB |
| JSON container nesting | 64 |
| Parsed JSON nodes | 1,048,576 |
| General protocol string | 64 KiB |
| Protocol identifier | 256 bytes |
| Snapshot or persistence objects | 65,536 |
| Snapshot/persistence properties per object | 1,024 |
| Wire journal records per application batch | 4,096 |
| Wire journal hierarchy-validation steps per batch | 262,144 |
| Pending mutation commands | 4,096 |

Attributes, Tags, custom enums, class extensions, and custom classes retain
their narrower existing name, count, definition, inheritance, sparse-state,
and aggregate-byte limits. Reference-valued Attributes, extension properties,
and custom-class properties remain unsupported; hardening does not expand their
type domains.

## Identity and same-scope references

`ObjectId` and `WireObjectId` remain explicit 32-bit slot/generation pairs and
never contain a pointer or address. Lookup requires the live weak object and an
exact generation match. When a slot reaches maximum generation it is retired
instead of wrapping, so no older identity can eventually resolve to a newer
object in the same slot.

Snapshot references resolve only through the candidate snapshot's source-ID
map. Loopback references resolve only through the receiving session's tracked
or candidate map. Missing, zero, stale, destroyed, same-slot/new-generation,
and outside-scope IDs are rejected; there is no global-registry fallback.
Reference-property preflight also requires the referenced candidate class to be
the reflected target class or one of its derived classes, so a class mismatch
cannot first appear after an earlier record in the batch has committed.
Authoritative reflected reference writes also require the referenced live
object to have the target object's DataModel replication scope before commit.

This is native DataModel scope correctness only. Per-peer visibility and
interest are deliberately deferred.

## Authority and command origin

`MutationAuthorityContext` keeps decoded command data separate from host-owned
authority metadata. It carries the existing `ScriptSecurityContext`, a native
origin (`LocalInternal`, `Studio`, or `AuthenticatedPeer`), and an optional
DataModel scope. Studio contexts require the Studio domain; peer contexts
require the Client domain and a valid scope. Scoped commands reject targets,
parents, or creates outside that assigned scope.

Capabilities still come from a native-created `ScriptSecurityContext`; packet
fields, script names, namespaces, class names, and hierarchy never manufacture
them. Main execution-domain enforcement and existing Core, Studio, and PreRun
capability behavior remain unchanged. `AuthenticatedPeer` is an authority slot
for a future host-authenticated session. No authentication, ticket, player, or
permission model is implemented here.

## Transaction and notification safe points

Snapshot loading performs complete structural and semantic preflight—including
class/schema identity, references, hierarchy cycles, properties, Attributes,
Tags, extensions, and custom-class sparse state—before constructing the
candidate graph. Failure never returns a root or partially populated object map.
The parent graph is cycle-checked iteratively with shared visit state, keeping
validation linear in the number of objects and avoiding recursion on deep flat
hierarchies.
Persistence builds an unexposed candidate hierarchy and publishes it only after
the complete tree and deferred tag-index reconstruction succeed. Runtime schema
publication already uses its candidate/validate/freeze/replace lifecycle.

Loopback journal batches simulate the complete batch before mutation. Preflight
covers creates, destroys, native/custom/extension properties, references,
Attributes, Tags, reparenting, cycles, no-ops, and aggregate limits. A semantic
error after a valid prefix returns zero applied records and leaves the receiver
cursor and state unchanged. Signals raised by a valid batch are queued until
all records commit, then delivered synchronously with the complete batch visible.
No internal journal, registry, or tag-index lock is held while those callbacks
run. Ordinary one-mutation native signals remain synchronous.

## Tested security guarantees

The deterministic hostile corpus exercises truncated compound values, unknown
fields, zero/malformed/overflowing IDs, integer narrowing overflow, non-finite
numbers, oversized strings and documents, excessive JSON depth, journal count
overflow, malformed persistence state, stale and cross-scope references,
out-of-order and duplicate records, wrong schema versions, custom-state prefix
rejection, hierarchy cycles, scoped authority, bounded mutation queues, atomic
batch rejection, and post-commit notification visibility.

The repository has no existing libFuzzer/AFL integration, so this milestone adds
fuzz-ready deterministic decoder/applicator entry coverage rather than a new
fuzzing dependency. Byte-oriented snapshot, journal, persistence, and EditorHost
entrypoints can be wrapped by a future fuzz runner without changing production
semantics.

## Deferred network-specific policy

Still unimplemented: packet framing, negotiated per-session budgets, peer
authentication and tickets, durable hosting identity, schema negotiation,
visibility/interest management, transport-specific message limits, congestion
control, binary game codecs, populated replication views, networking threads,
sockets, and runtime disconnect policy. Pure value/interface contracts now live
in `NetworkingContracts.md`; they do not consume this boundary yet.
`ChangeJournal.Sequence` remains authoritative scoped history and is not a packet
or per-peer replication sequence.
