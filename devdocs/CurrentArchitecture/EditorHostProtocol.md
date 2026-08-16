# EditorHost v0 protocol and Studio boundary

## Implemented boundary

Gargantuan Studio is a separately authored private application. The public
Gargantuan repository owns the engine, protocol specification, wire schemas,
validation, and authoritative DataModel state. The private application must not
include Gargantuan's private C++ headers or copy implementation from the removed
legacy Studio prototype in Git history.

EditorHost v0 is a headless document host reached through standard input and
output. It does not construct `Engine`, initialize a renderer, run gameplay scripts, step
simulation, or synchronize `FileLink` objects. Opening is therefore distinct
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
journal polling carries it beside the independent cursor and records. Save
accepts no revision or dirty input. Save As accepts only `Destination`, adopts
it after successful atomic persistence, and preserves the instance format.

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
limited to 256 records.

## v0 methods

| Method | Contract |
| --- | --- |
| `Handshake` | Returns engine identity, protocol version, and capabilities. |
| `OpenProject` | Canonicalizes and loads a project root without executing gameplay scripts. |
| `GetSchema` | Returns class compatibility metadata plus schema-discovery v4 definitions and registry generation. |
| `GetSnapshot` | Returns snapshot v6 and establishes the session cursor. |
| `PollChanges` | Returns scoped wire-journal v6 records after that cursor. |
| `SetProperty` | Applies a closed non-reference `WireValue` through `MutationGateway`. |
| `SetAttribute` | Applies or removes a bounded attribute through `MutationGateway`. |
| `SetExtensionProperty` | Applies a schema-resolved extension property through `MutationGateway`. |
| `SetCustomProperty` | Applies a schema-resolved custom class property through `MutationGateway`. |
| `AddTag` / `RemoveTag` | Applies bounded tag membership through `MutationGateway`. |
| `CreateInstance` | Creates an editor-constructible active schema identity under a stable parent; engine returns the allocated ObjectId. |
| `DestroyInstance` | Recursively destroys one generation-safe, non-protected project target. |
| `DuplicateInstance` | Engine-clones one persistent subtree beside its source with fresh identities. |
| `ReparentInstance` | Atomically moves one stable target beneath another after scope/cycle/protection validation. |
| `ConfigureViewport` | Negotiates a bounded engine-owned RGB8 viewport. |
| `SetViewportCamera` | Applies a finite absolute editor-camera pose and field of view. |
| `OpenViewportTransport` | Explicitly selects shared-memory ring v1 and returns its fixed layout contract. |
| `CloseViewportTransport` | Releases the host's shared-memory mapping. |
| `CaptureViewport` | Publishes RGB8 to the selected ring, or returns the versioned Base64 fallback. |
| `PickViewport` | Resolves a viewport pixel to the nearest live BasePart `ObjectId`. |

`SetProperty` accepts only live objects whose replication scope is the open
DataModel. The committed setter path remains responsible for journal emission.
`SetAttribute` uses the same live-object and `MutateDataModel` checks. Every
mutation carries a host-created Studio authority context scoped to the open
DataModel; decoded request data never supplies capabilities or scope. Attribute
state is delivered by snapshot and dedicated `AttributeUpdate` records rather
than a second polling path. `AddTag` and `RemoveTag` use that same authority;
snapshot membership and `TagAdded`/`TagRemoved` carry committed tag state.
Object-reference and enum-item property mutation, transactions, source mounts,
and play sessions are deliberately outside v0. Viewport methods
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

Schema discovery is read-only. Version 4 returns stable class/enum/extension
identity, definition kind and version, provenance, class-base and extension-target
IDs, class construction/subclass policy and native host identity, ordered
custom-enum items, and ordered declarative schema properties. Studio
does not receive `DefineSchema`, the PreRun facade, candidate registry access,
or mutable native metadata. The top-level EditorHost protocol remains version
1; schema discovery is independently versioned.

Replacing an open project closes and releases the prior live DataModel and
viewport snapshot before entering the next schema candidate lifecycle. If the
replacement PreRun or project load fails, no old world remains live against the
new or prior registry; a later `OpenProject` may construct a fresh document.

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
[EditorViewport.md](./EditorViewport.md). Authoritative scalar and structural
mutation, persistence, and bounded enum/value decoding are implemented. The
next authoring interface increment is explicit engine-owned transaction and
Undo/Redo semantics; project revision remains separate from that history.
