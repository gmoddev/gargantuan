# EditorHost v0 protocol and Studio boundary

## Implemented boundary

Gargantuan Studio is a separately authored private application. The public
Gargantuan repository owns the engine, protocol specification, wire schemas,
validation, and authoritative DataModel state. The private application must not
include Gargantuan's private C++ headers or copy implementation from the removed
legacy Studio prototype in Git history.

EditorHost v0 is a headless document host reached through standard input and
output. It does not construct `Engine`, initialize a renderer, run scripts, step
simulation, or synchronize `FileLink` objects. Opening is therefore distinct
from executing. Protocol responses are lines prefixed with
`GARGANTUAN_EDITOR/1 `; requests are unprefixed single-line JSON documents.

Each launch requires a random token supplied with `--editor-token`. Every
request repeats that token. This is process association, not a claim of strong
authentication against a hostile local user.

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
| `OpenProject` | Canonicalizes and loads a project root without executing it. |
| `GetSchema` | Returns deterministic reflected class and property metadata. |
| `GetSnapshot` | Returns snapshot v2 and establishes the session cursor. |
| `PollChanges` | Returns scoped wire-journal v2 records after that cursor. |
| `SetProperty` | Applies a closed non-reference `WireValue` through `MutationGateway`. |

`SetProperty` accepts only live objects whose replication scope is the open
DataModel. The committed setter path remains responsible for journal emission.
Object references, enum items, create, reparent, destroy, transactions, saving,
source mounts, viewport transport, and play sessions are deliberately outside
v0.

## Licensing and repository contract

- Gargantuan and EditorHost remain public MPL-2.0 code.
- The protocol is public and may be implemented by independent tools.
- Gargantuan Studio lives in a private, separately licensed repository.
- The removed `gargantuan/studio/**` history remains MPL-covered legacy material
  and is prohibited source material for the new implementation.
- The private repository consumes a Gargantuan executable/release through this
  protocol; it does not duplicate engine implementation.

## Next interface increment

The smallest next increment after the v0 proof is renderer-owned viewport output
plus picking and camera commands. Before accepting broader mutation types, add
same-scope reference validation, transaction IDs, editor command authority, and
bounded enum/reference decoding through the mutation gateway.
