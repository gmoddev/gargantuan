# EditorHost v0 protocol and Studio boundary

## Implemented boundary

Gargantuan Studio is a separately authored private application. The public
Gargantuan repository owns the engine, protocol specification, wire schemas,
validation, and authoritative DataModel state. The private application must not
include Gargantuan's private C++ headers or copy implementation from the removed
legacy Studio prototype in Git history.

EditorHost v0 is a headless document host reached through standard input and
output. Opening does not construct `Engine`, initialize a runtime renderer, run
gameplay scripts, step simulation, or synchronize `FileLink` objects. Explicit
`StartPlaySession` is the sole minimal local execution path. Opening is therefore distinct
from executing a game. Project open may run only the bounded, capability-scoped
`.gargantuan/prerun.luau` schema-registration phase before constructing the
document. Protocol responses are lines prefixed with
`GARGANTUAN_EDITOR/1 `; requests are unprefixed single-line JSON documents.

Each launch requires a random token supplied with `--editor-token`. Every
request repeats that token. This is process association, not a claim of strong
authentication against a hostile local user.

Project persistence uses `SaveProject`, `SaveProjectAs`, and `GetProjectState`.
State contains `AuthoritativeRevision`, `PersistedRevision`, derived `Dirty`,
and `CurrentDestination`. Open and snapshot return it under `ProjectState`;
journal polling carries it beside the independent cursor and records. Save accepts
optional `ExpectedRevision` and no dirty input. Save As accepts `Destination` plus
optional `ExpectedRevision`, rejects a mismatch before snapshot/write, adopts its
destination only after successful atomic persistence, and preserves the instance
format. Studio supplies its observed revision for Save As; MCP exposes no Save As.

## Envelope and limits

Requests have exactly these fields:

```json
{
  "Version": 1,
  "RequestId": "client-owned bounded string",
  "SessionToken": "random per-launch token",
  "Method": "Handshake",
  "Params": {}
}
```

Responses contain the same version and request ID, an `Ok` boolean, and exactly
one conceptual result or structured error. Unknown versions, methods, fields,
invalid IDs, and malformed values fail closed. Input is drained with a 1 MiB
limit before JSON parsing; responses are limited to 8 MiB. Journal batches are
limited to 256 records. Script Source is additionally limited to 65,536 valid
UTF-8 bytes with no NUL.

## v0 methods

| Method | Contract |
| --- | --- |
| `Handshake` | Returns engine identity, protocol version, and capabilities. |
| `OpenProject` | Canonicalizes and loads a project root without executing gameplay scripts. |
| `CreateProject` | Creates, initially persists, and adopts a minimum project without accepting serialized state or revision input. |
| `GetProjectState` | Returns authoritative/persisted revisions, derived dirty state, destination, and bounded history status. |
| `SaveProject` / `SaveProjectAs` | Optionally compare `ExpectedRevision`, then atomically persist an exact authoritative revision; Save As adopts its validated destination only after success. |
| `GetSchema` | Returns class compatibility metadata plus schema-discovery v7 definitions, sparse concrete-class default overrides, native property semantics, and registry generation. |
| `GetSnapshot` | Returns snapshot v6 plus editor-property projection v1 and establishes the session cursor. |
| `PollChanges` | Returns scoped wire-journal v6 records after that cursor. |
| `SetProperty` | Applies a schema-identified closed native-property `WireValue` through `MutationGateway`; legacy Name remains compatible. |
| `SetPropertyBatch` | Applies 1–256 prepared-safe native property writes through the Engine prepared coordinator as one atomic history action; requires property-batch capability version 1, current scope and exact revision. |
| `SetTransform` | Atomically applies optional canonical `CFrame` and `Size` values to one `BasePart` through one implicit authoring transaction. |
| `SetAttribute` | Applies or removes a bounded attribute through `MutationGateway`. |
| `SetExtensionProperty` | Applies a schema-resolved extension property through `MutationGateway`. |
| `SetCustomProperty` | Applies a schema-resolved custom class property through `MutationGateway`. |
| `AddTag` / `RemoveTag` | Applies bounded tag membership through `MutationGateway`. |
| `CreateInstance` | Creates an editor-constructible active schema identity under a stable parent; optional `InitialSource` is accepted only for a `LuaSourceContainer` class and commits atomically with Name and parent publication. |
| `DestroyInstance` | Recursively destroys one generation-safe, non-protected project target. |
| `DuplicateInstance` | Engine-clones one persistent subtree beside its source with fresh identities. |
| `ReparentInstance` | Atomically moves one stable target beneath another after scope/cycle/protection validation. |
| `BeginTransaction` | Creates one engine-issued, session-owned commit-only authoring group with a bounded label. |
| `CommitTransaction` | Commits the exact owned open group, advances one revision, and releases its journal batch. |
| `Undo` / `Redo` | Traverses only the current engine history cursor entry and publishes ordinary authoritative journal state. |
| `GetScriptSource` | Reads exact bounded Source plus its conflict token for one live supported script ObjectId. |
| `SetScriptSource` | Commits bounded UTF-8 Source through MutationGateway using the exact expected SourceVersion and optional project `ExpectedRevision`. |
| `StartPlaySession` / `StopPlaySession` | Starts or destroys the one isolated local runtime from current authoritative in-memory state using an exact engine-issued session identity. |
| `GetPlaySessionState` / `PollPlayDiagnostics` | Observes bounded lifecycle and runtime diagnostics without granting mutation authority. |
| `SendPlayInput` | Sends the closed focus/key/pointer/wheel/touch/committed-text/preedit `HostEvent` subset to the exact active runtime and returns any bounded relative-pointer or text-input host state, including secure/multiline/autocorrect policy without text contents. |
| `ConfigureViewport` | Negotiates a bounded engine-owned RGB8 viewport. |
| `SetViewportCamera` | Applies a finite absolute editor-camera pose and field of view. |
| `OpenViewportTransport` | Explicitly selects shared-memory ring v1 and returns its fixed layout contract. |
| `CloseViewportTransport` | Releases the host's shared-memory mapping. |
| `CaptureViewport` | Publishes RGB8 to the selected ring, or returns the versioned Base64 fallback. |
| `PickViewport` | Resolves a viewport pixel to the nearest live BasePart `ObjectId`. |

`SetProperty` accepts only live objects whose replication scope is the open
DataModel. The committed setter path remains responsible for journal emission.
`SetTransform` is the bounded editor-authoring compound operation used by
face-based resize. It accepts at least one and at most the canonical `CFrame`
and `Size` properties, preflights both values before mutation, enforces finite
components and the `0.01` minimum part extent, and either commits all supplied
values as one Undo entry or applies neither. Its journal may contain one record
per changed property, but all records share the one resulting project revision
and are released as one transaction batch.
The CFrame wire and project formats both encode the three rotation basis columns
contiguously as Right, Up, and Back. Decode, Undo/Redo, and save/reopen preserve
that orientation rather than transposing the matrix.
`SetAttribute` uses the same live-object and `MutateDataModel` checks. Every
mutation carries host-owned Studio authority scoped to the open
DataModel; decoded request data never grants capabilities or scope authority. Attribute
state is delivered by snapshot and dedicated `AttributeUpdate` records rather
than a second polling path. `AddTag` and `RemoveTag` use that same authority;
snapshot membership and `TagAdded`/`TagRemoved` carry committed tag state.
Native enum mutation uses canonical enum type and item identity. Object-reference
metadata carries stable class constraints and effective property access;
references are excluded from prepared batches. Source mounts and play sessions are otherwise deliberately outside
the native property contract. Script Source is intentionally excluded
from generic `SetProperty`; its dedicated token-checked operation is the only
Studio write path. Viewport methods
are a compatible capability extension with their own `ViewportVersion = 1`.
`Handshake.ViewportTransports` is authoritative: clients must negotiate rather
than assuming shared memory. The current Windows host advertises
`SharedMemoryRing` version 1 with RGB8; hosts without it advertise Base64 only.

The handshake also publishes `ScriptSecurityVersion`,
`StudioExecutionDomain`, and the exact `StudioCapabilities` grant. Version 1
uses the `Studio` domain with `ReadDataModel`, `MutateDataModel`,
`EditorCommands`, `SelectionAccess`, and `ViewportControl`. This grant is an
enforceable contract:
schema reads, snapshots, journal polling, reflected property dispatch, and the
mutation gateway check it at their native boundaries. Every viewport method
also checks `ViewportControl`. It does not grant
process, filesystem, network, or arbitrary engine-native access.

Schema discovery is read-only. `GetSchema` accepts only the optional
`SchemaDiscoveryVersion` field (unsigned 6 or 7); omission returns 6. Version 7
must be explicitly requested and adds the existing concrete-default overrides.
Unsupported versions reject with `UnsupportedCapabilityVersion`. This preserves
ordinary editing for strict v6 consumers. See the
[reset consumption contract](AuthoritativePropertyResetToDefault.md).
Version 6 returns stable class/enum/extension
identity, definition kind and version, provenance, class-base and extension-target
IDs, class construction/subclass policy and native host identity, ordered
custom-enum items, ordered declarative schema properties, and native property
datatype/access/category/range/hint/compound/enum/reference/nullability semantics. Studio
does not receive `DefineSchema`, the PreRun facade, candidate registry access,
or mutable native metadata. The top-level EditorHost protocol remains version
1; schema discovery is independently versioned.

Replacing an open project closes and releases the prior live DataModel and
viewport snapshot before entering the next schema candidate lifecycle. If the
replacement PreRun or project load fails, no old world remains live against the
new or prior registry; a later `OpenProject` may construct a fresh document.

## Prepared native property batches, capability version 1

`Handshake.Result.Capabilities` includes `SetPropertyBatch`, and
`Handshake.Result.PropertyBatchVersion` is `1`. This version jointly covers the
method and the additive `AtomicBatchWritable` property metadata in both
`GetSchema.Result.Definitions[].Properties[]` and the inherited `Classes` adapter.
Protocol version 1 and property-batch version 1 remain unchanged. Schema
discovery version 7 adds sparse concrete-class default metadata; see the
[concrete-default contract](ConcreteClassPropertyDefaults.md). Clients
must check the capability and version before offering batch editing, then echo
`PropertyBatchVersion: 1` on each batch; no mutable handshake negotiation state
is needed. Old clients can ignore the additional capability and fields and keep
using existing single-property methods. Unknown batch versions fail closed.

The flag is derived from the coordinator's exact generated prepared-store
eligibility plus effective Studio read/write permissions and mutation capability.
It is discovery guidance, never authority or a promise that a particular value
will validate. Clients must not infer support from datatype. Ordinary generated
scalar, string, vector, color, UDim/UDim2, CFrame and native enum backing stores
can qualify. Handwritten/override setters, references, Source, custom/extension
maps, Attributes, Tags and hierarchy do not qualify. A custom class can still
inherit a supported native property such as `Instance.Name`.

The envelope remains the standard envelope above. `Params` has exactly:

```json
{
  "PropertyBatchVersion": 1,
  "Scope": { "Slot": 1, "Generation": 3 },
  "ExpectedRevision": 42,
  "Writes": [
    {
      "Object": { "Slot": 10, "Generation": 3 },
      "DeclaringClassSchemaId": "0123456789abcdef0123456789abcdef",
      "DeclaringDefinitionVersion": 1,
      "Property": "Name",
      "Value": { "Type": "String", "Value": "Renamed" }
    }
  ]
}
```

IDs in this example are illustrative. Take `Scope` from the current
`GetSnapshot.Result.Snapshot.Cursor.Scope`, object IDs from that snapshot, and
declaring schema ID/version from current discovery (following the class base
chain for inherited properties). Schema IDs are canonical 32-digit hexadecimal;
object slot/generation and positive definition versions fit uint32. Revision is
a required positive uint64, separate from cursor sequence and transaction ID.
No object names or hierarchy paths resolve identities. The launch token is
checked first; the scope is only an equality precondition against the host's
current DataModel. Thus even a delayed request with a coincidentally matching
reset revision cannot reach a replacement project. Full object generations,
declaring schema/version, active frozen registry and final revision are rechecked
by Engine. Request data cannot supply authority.

Admission requires `MutateDataModel`, an open project and established snapshot
cursor, effective reflected access, stopped Play, and no open ordinary authoring
group. `TransactionId` is not accepted. A batch does not expire or implicitly
commit an existing ordinary group, including on rejection. Other commands retain
their existing timeout/group behavior, and `SetTransform` is unchanged.

Bounds are 1–256 writes, 1 MiB for the entire encoded request before parsing,
256 UTF-8 bytes per property/identifier, and 64 KiB per string value. Existing
stricter RequestId (128 bytes) and method (64 bytes) limits, JSON depth/node,
UTF-8, NUL and finite-value validation also apply. The bounded
array count is checked before per-write reservation; IDs and versions are checked
before narrowing. The coordinator retains its conservative request accounting,
4 MiB aggregate old/new values, 2 MiB journal payload, 8 MiB history action, and
256 direct notifications/records. Its conservative estimate can reject an
envelope that fits the transport byte limit. See the
[resource accounting and proof](./PreparedPropertyCommitValidation.md).
Nothing silently splits or chunks a request. Every repeated object/property pair
is rejected, including identical assignments and conflicting schema identities.

Whole-batch success uses the ordinary `Ok: true` / `Result` envelope:

```json
{
  "StartingRevision": 42,
  "ResultingRevision": 43,
  "ChangedWriteCount": 1,
  "TransactionId": "123",
  "NotificationFailures": 0
}
```

`TransactionId` follows the existing canonical decimal-string convention. A
complete no-op returns equal revisions, zero changed writes and `null` transaction
ID; it creates no history or journal state. Mixed batches omit no-op writes from
the committed action. A nonempty changed set installs all values, one Engine
history action, normal property journal records and exactly one revision
advancement together. Prepared history Undo/Redo uses the same coordinator and
advances one revision per successful replay. Existing ordinary history semantics
are unchanged. Polling/replication consumes the normal journal; there is no
parallel publication or EditorHost-owned history. Reconcile through `PollChanges`
and authoritative project state, as for existing mutations.

The success response storage and escaping, and parsed request cleanup, are
completed before invoking the coordinator. After success, fixed numeric slots
are filled without allocation. Post-commit observer failures are counted in
`NotificationFailures` and remain success; they cannot undo a committed action
or be reported as a failed batch. Observers see the complete installed state and
may initiate later actions. `ResultingRevision` identifies this action's revision,
not any subsequent observer action. A lost transport response remains an unknown
delivery outcome: refresh authoritative state rather than blindly retrying.

Failures use `Ok: false` / `Error` with no per-write success array:

| Code | Meaning |
| --- | --- |
| `MalformedRequest` | Invalid envelope, count shape/empty array, malformed ID/version, unknown fields, or existing framing/JSON/string limits. An envelope over 1 MiB is rejected before parsing, with null RequestId and the existing byte-length diagnostic. |
| `UnsupportedCapabilityVersion` | Batch version is missing from the supported version set. A missing required field itself is malformed. |
| `Unauthorized` | Rejected launch token, missing authoring capability, or denied effective property access. |
| `ProjectRequired` / `SnapshotRequired` | No current project or no established snapshot cursor. |
| `StaleProject` | Scope identifies another project instance, even if revisions match. |
| `Conflict` | Expected authoritative revision differs. |
| `StaleObject` | Object is missing, destroyed, stale generation, or outside the current project. |
| `StaleSchema` | Unknown property or incompatible declaring schema/version. |
| `ReadOnly` | Property has no writer or its write permission is Never. |
| `UnsupportedProperty` | Reflected property lacks prepared-safe support. |
| `ValidationFailed` | Invalid WireValue or Engine type/range/enum/native-value rejection. |
| `DuplicateWrite` | Repeated object/property identity, even with equal values. |
| `ResourceLimit` | More than 256 writes or a coordinator preparation/resource budget failure. |
| `TransactionOpen` / `PlaySessionActive` | Authoring state does not admit a batch. |
| `RevisionExhausted` | Revision cannot advance. |
| `WrongExecutionDomain` / `Rejected` / `InternalError` | Engine admission or preparation failed. |

An authoritative batch failure contributes zero live writes, revision changes,
journal records or history actions. The adapter validates only transport,
identity, discovery eligibility and effective access; deep property preparation,
no-op normalization, atomic installation and replay remain exclusively in
`PreparedPropertyCommit`. General JSON allocator exhaustion before that primitive
retains the existing process-failure limitation documented as KI-008; it does
not imply partial batch installation.

This is Engine API exposure only. Studio multi-object Properties UX is a separate
task gated on the [final-source qualification receipt](./PreparedPropertyCommitValidation.md#editorhost-exposure-qualification).

## Licensing and repository contract

- Gargantuan and EditorHost remain public MPL-2.0 code.
- The protocol is public and may be implemented by independent tools.
- Gargantuan Studio lives in a private, separately licensed repository.
- The removed `gargantuan/studio/**` history remains MPL-covered legacy material
  and is prohibited source material for the new implementation.
- The private repository consumes a Gargantuan executable/release through this
  protocol; it does not duplicate engine implementation.

## Next interface increment

The bounded shared-memory viewport transport is implemented in
[EditorViewport.md](./EditorViewport.md). Authoritative scalar/structural/source
mutation, persistence, transactions, and Undo/Redo are implemented. Script
authoring uses project-v4 persistence and a journaled SourceVersion invalidation
without exposing Source through ordinary snapshots or gameplay replication.
Minimal isolated Play/Stop is implemented in [PlaySession.md](./PlaySession.md).
The next creator-loop increment may build a first playable vertical slice on that
boundary; project revision remains separate from play identity, transaction identity,
source version, and journal sequence.
