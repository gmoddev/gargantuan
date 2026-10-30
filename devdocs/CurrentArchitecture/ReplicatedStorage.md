---
status: implementation-under-qualification
owner: runtime-replication
last_verified: 2026-10-29
---

# ReplicatedStorage and declarative custom services

This milestone extends the existing frozen class registry and DataModel service
owner. It adds no transport, content provider, peer-interest authority, public
streaming lifecycle or schema registry. Focused service, production GameSession,
client-runtime, persistence and EditorHost/package tests pass on the native
Windows worker. Required hosted CI remains a separate gate. This document is not
a physical/provider qualification receipt.

## Declaration and ownership

`ReplicatedStorage` retains its public native name. Loaded authoring projects
establish its canonical protected service root. A project may declare an approved
data-only custom service in its existing PreRun source:

```lua
Schema:RegisterClass({
    Namespace = "Game",
    Name = "SharedInventory",
    Version = 1,
    Base = "Engine.Folder",
    Service = true,
    Replicated = true,
    Properties = {},
})
```

`Service` and `Replicated` are strict optional booleans. Without `Service`, the
definition remains an ordinary custom class. `Replicated=true` requires
`Service=true`; omitting it for a service selects server-only visibility. Empty
properties are allowed for a service container. Existing data-only construction,
identity, aggregate limits and transactional PreRun validation still apply.
Service definitions cannot be custom subclass bases. Changing a project's
declared semantics requires the appropriate explicit definition version change.

The single frozen `SchemaServicePolicy` class field has None, ServerOnly and
Replicated values. Native classgen declares the built-in policy. DataModel derives
custom service names/identities from that same active registry, keyed by canonical
class name (for example `game:GetService("Game.SharedInventory")`). ServiceProvider
constructs the approved host through InstanceClassRegistry and reuses the exact
live, generation-safe class identity. Loaded roots use the existing protected
service lifecycle; ordinary authoring create/duplicate/delete/reparent commands
cannot manufacture or replace them. A declaration grants no script capability,
native callback, filesystem access or mutation authority.

## Visibility, mutation and source

Replicated service contents use ordinary server-owned relevance and the existing
dependency-safe structural path. Spatial distance does not hide stored contents.
The small service roots are bootstrap-required; contents still obey existing
structural budgets and materialization/convergence. Server-only service subtrees
are excluded. Hard dependencies cannot pull them into a receiver; soft references
retain the existing nullable/fixup rules. Accepted ancestry, ObjectId generation,
late baselines, retirements and epochs remain owned by the existing coordinator
and receiver. Existing planning, fairness, byte admission and scheduler limits
are not increased.

Server/Studio edits remain authoritative. Client contexts cannot mutate, parent,
destroy, set attributes/extensions/custom values, or change tags in replicated
service state. The gateway checks captured caller context, and native mutation
boundaries also enforce it; native receiver application remains a trusted Engine
operation. Client GetService/FindService cannot construct or resolve a server-only
service. Client visibility is not server mutation permission.

Only ModuleScript source in a replicated service receives a native, Engine-only
`ReplicatedSource` projection. Its getter derives from the canonical Source and
current frozen policy/ancestry. Source and ancestry changes invalidate that
projection through the existing journal. Private/non-shared modules project an
empty string. The projection is transient, non-editable and not a second saved
source. Ordinary Source remains the canonical persisted authoring resource and
keeps its existing privileged source operations. Existing project formats and
persisted class identities are unchanged. Network qualification uses matching
candidate endpoints; older receivers reject an unknown native property rather
than gaining source access or accepting a partial materialization.

Applying projected bytes invalidates compiled bytecode without incrementing the
separately received exact SourceVersion a second time. Stored Scripts do not
start through ordinary ScriptEngine startup, regardless of RunContext. A module
does not execute on replication: client/server code must explicitly require it
under its existing execution domain and capabilities. Existing VM require-cache
semantics remain unchanged; source delivery is not automatic hot reload of
previously returned module values. A server-only Script receives no source or
client execution authority merely from placement.

## Persistence and residency

Canonical module Source and ordinary authored descendants use existing project
persistence and package closure. The derived source projection is not saved.
These service subtrees remain in the existing bootstrap snapshot because 3L
partitions only eligible direct Workspace children. No new trusted demand or
second content acquisition path is introduced. Package possession is not a
claim of source-file secrecy; this policy describes live server/client
materialization and execution authority.

Studio consumes these roots through its normal schema/snapshot/Explorer workflow;
descendant authoring remains the existing EditorHost command surface. PreRun's
replication toggle is declarative source, not a mutable Instance property or
runtime client operation.

The optional handshake capabilities `ReplicatedStorage` and
`CustomServiceDeclarations` identify this extension. Existing schema discovery
and snapshot layouts are unchanged; protected service classes remain
non-constructible through ordinary Studio commands.

## Focused validation

`ReplicatedStorageTests` covers native/custom singleton identity, strict toggles,
client denial without mutation, module delivery/require, dormant stored Scripts,
codec/receiver attribution, private reparent, reentry generation and persistence.
`ReplicatedStorageSessionTests` exercises real GameSession production planning
over simulated transport: bootstrap, server updates, late join, disconnect,
reconnect and teardown. `ReplicatedStorageEditorHostTests` exercises the ordinary
Explorer/source command contract, protected roots, save/reopen and actual
package build/load. These are not native Studio visual or physical network tests.
