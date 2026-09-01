---
status: current
owner: engine
last_verified: 2026-08-16
related_code:
  - include/gargantuan/
  - src/
related_adrs: []
---

# Core architecture invariants

These invariants are the cross-subsystem constraints that Gargantuan changes
must preserve. They describe required properties, not preferred class layouts.
Tests should enforce them where practical.

## Authority and ownership

1. A DataModel is the authoritative owner of its live Instance graph, mutation
   scope, object registry, and committed change stream.
2. Studio, render snapshots, replication receivers, document caches, and future
   clients are projections. None becomes a second authority over the source
   DataModel.
3. Authoritative external changes pass type/bound validation, capability and
   authority checks, and the mutation gateway before commit.
4. Subsystems consume committed state or explicit commands. They do not gain
   mutation authority by retaining an Instance pointer.
5. Ownership of native resources is explicit. GPU, physics, filesystem, process,
   transport, and platform handles do not cross public boundaries implicitly.
6. A local Play runtime is a separately owned DataModel/VM graph captured from a
   committed authoritative state. Runtime mutation never aliases or commits into
   authoring state, revision, dirty state, or history.

## Object identity and lifecycle

1. `ObjectId` identifies a live object only within its defined scope and uses a
   generation-safe representation.
2. Zero or otherwise invalid identity never denotes a live object.
3. Reusing storage cannot cause a stale ID to resolve to a replacement object.
4. Identity does not depend on memory address, display name, or hierarchy path.
5. Destruction and scope exit invalidate identity and remove reverse-index state
   before storage or slots can be reused.
6. Serialization, snapshots, journals, replication, picking, and EditorHost use
   stable identity representations, never native pointers.
7. Authoritative create/destroy is distinct from per-receiver publish/unpublish.
   Interest changes must not falsify authoritative lifecycle history.

## Mutation and committed changes

1. Rejected mutation produces no partial state, journal record, or success signal.
2. A committed logical change is published only after its validation and state
   transition succeed.
3. No-op writes do not invent change records solely to imitate activity.
4. Ordered journal sequence describes authoritative committed history within its
   scope. It is not a universal packet, peer-replication, render, or RPC sequence.
5. Consumers may project, filter, or coalesce committed history without mutating
   the source journal or treating omitted entries as transport loss.

## Runtime schema

1. Stable `SchemaId` is deterministic and independent of address, registration
   order, and process randomness.
2. Definition version expresses persisted semantic evolution. Registry generation
   is session-local cache invalidation. They are not interchangeable.
3. Schema registration builds and validates a candidate registry. Runtime sees
   either one complete frozen registry or no replacement; partial publication is forbidden.
4. The active runtime registry is immutable. Runtime construction never proceeds
   against a partially registered schema.
5. Duplicate identity, invalid inheritance, member conflicts, and invalid access
   metadata fail validation rather than resolving by load order.
6. Schema metadata describes required domains/capabilities but never grants
   privilege based on namespace, provenance, class name, script name, path, or parentage.
7. Persistence, replication selection, reflection, Studio discovery, and generated
   typing derive from the same canonical definitions rather than parallel policy tables.
8. A class extension has its own kind-separated identity and targets a stable
   class identity. It never replaces that class, changes `IsA`, or grants runtime
   authority from its namespace, provenance, or target.
9. Extension property meaning is frozen schema; per-Instance extension values are
   bounded runtime state mutated only through authoritative capability-checked paths.
   They remain distinct from Attributes and do not mutate registry generation.
10. A project custom class is a canonical class definition with stable identity,
    a stable class base, and a native-owned construction policy. Its C++ host type
    never replaces its public schema identity, and missing/incompatible persisted
    identity must not fall back to a native base.
11. Custom class properties are frozen declarative schema with bounded sparse
    per-Instance state. They remain distinct from Attributes and extensions, and
    project registration can never supply native callbacks or behavior hooks.

## Security and execution

1. Execution domains identify context; they are not an implicit privilege hierarchy.
2. Capabilities are assigned by the native host and enforced at native/reflection
   boundaries. Trusted Core Luau remains capability-checked.
3. Copying or renaming privileged source does not copy privilege.
4. Plugin code never executes with a first-party Studio capability set, including
   when both share a VM or process.
5. Project source access remains root-confined unless a separately granted host
   capability authorizes more.
6. Untrusted client, project, plugin, IPC, persistence, and network input is
   bounded and validated before it reaches authoritative state or privileged resources.

## Dynamic Instance state

1. Attributes and tags are bounded dynamic state; they do not mutate the frozen
   class schema.
2. Attribute and tag writes use authoritative mutation and capability paths.
3. Attribute types, names, per-object counts, encoded values, and aggregate bytes
   remain bounded by explicit policy.
4. Tag names, per-object membership, and active distinct-name counts remain bounded.
5. Destroy and scope exit remove all tag reverse-index entries before identity reuse.
6. Queries return deterministic live identity order and never retain or return dead IDs.

## Rendering

1. Renderer and pass code consume immutable extracted render state and do not
   traverse the DataModel or read mutable Instances.
2. A published render snapshot contains no Instance, Camera, WorldRoot, callback,
   or runtime-owned container pointer.
3. Runtime mutation or object destruction cannot invalidate an already published frame.
4. Picking uses stable identity from the displayed snapshot and validates it
   against the live registry before authoritative use.
5. GPU resources are renderer/backend-owned and never stored in or exposed by Instances.

## Physics

1. Engine systems depend on Gargantuan physics semantics; backend-native types,
   handles, callbacks, and ownership primitives remain inside the backend adapter.
2. Physics body and constraint IDs are generation-safe engine values independent
   of `ObjectId`, native pointers, and backend handles.
3. Committed authoritative Part changes become physics intents and are applied on
   Main at an explicit non-stepping safe point.
4. Backend events are copied into engine-owned values and validated against live
   physics and object identities before they reach Instances.
5. Backend-internal body or shape reconstruction does not unexpectedly change
   Gargantuan physics identity or leave dangling constraint identity.

## Character and gameplay actors

1. `Character` is Gargantuan's permanent canonical gameplay-actor type.
   `KinematicCharacter : Character` is the current native movement
   specialization, `Player.Character` is an optional `Character` reference,
   and an NPC never requires a fake `Player`.
2. Gargantuan will not introduce `Humanoid`, `HumanoidController`,
   `HumanoidStateMachine`, or an equivalent health/traversal/combat/locomotion
   aggregate under another name. Character behavior remains composed from
   narrow native authority primitives and replaceable game Luau policy.
3. Native Character code may own identity, validation, control lifetimes,
   movement admission, physics/controller facts, bounded prediction and
   reconciliation, and protocol bookkeeping. Walk/run/jump/attack/vault state,
   action selection, animation selection, health, traversal, combat, and other
   game semantics remain Luau or game-owned composition.
4. A client may request bounded semantic movement or actions, but never supplies
   an authoritative Character transform, velocity, collision result, or
   root-motion displacement. The server derives and admits those values.
5. Runtime Character movement and network correction are transient simulation
   state. They must not create authoring journal records, structural replication
   property streams, save dirtying, or document reconciliation work.

## EditorHost and Studio

1. EditorHost is the versioned public process boundary between Studio and the engine.
2. Studio receives DTOs, stable IDs, schema, journals, commands, and viewport
   pixels; it never receives raw engine pointers, private headers, or GPU handles.
3. Studio's document model is a non-authoritative snapshot/journal projection.
4. Studio mutations return through validated EditorHost commands and the engine
   mutation path.
5. Opening an editor document does not implicitly execute project scripts or
   grant filesystem, process, network, or raw-engine authority.

## Persistence, replication, and networking

1. External formats are explicitly versioned, bounded, schema-aware, and fail
   safely on malformed or unsupported input.
2. Object references validate identity generation, scope, visibility, and
   authority as appropriate to the boundary.
3. The debug/EditorHost snapshot and journal representation is not automatically
   the optimized game-network representation.
4. Authoritative history, receiver-specific replication intent, and wire delivery
   remain distinct layers.
5. Engine replication and application remotes may share scheduling and transport
   primitives but never share authority or semantics.
6. Reliable queues, decode queues, pending requests, and replication backlogs are
   bounded. Congestion cannot cause unbounded memory growth.
7. Runtime schema is frozen before gameplay replication begins, and clients never
   receive runtime schema-definition authority.

## Changes requiring architecture review

Architecture review and usually an ADR are required before:

- changing identity width, scope, generation, or reuse semantics;
- moving authoritative ownership outside DataModel or bypassing the mutation gateway;
- introducing another runtime schema or policy registry;
- allowing post-freeze schema mutation;
- changing privilege assignment or capability enforcement boundaries;
- allowing renderer access to mutable runtime objects or storing GPU resources in Instances;
- exposing raw engine state across EditorHost;
- changing snapshot, journal, persistence, IPC, or network compatibility semantics;
- coupling authoritative journal sequence to a receiver or transport sequence;
- adding a process, VM, thread, transport, or host-authority boundary; or
- replacing bounded failure/backpressure with unbounded buffering or indefinite waits.
