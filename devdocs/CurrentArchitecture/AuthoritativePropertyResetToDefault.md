---
status: current
owner: runtime-authoring
last_verified: 2026-09-18
related_code:
  - src/classes/Instance.cpp
  - src/editor/EditorHost.cpp
  - tests/ConcretePropertyDefaultTests.cpp
---

# Authoritative property reset

Reset applies an ordinary value from the existing
[concrete-default resolver](ConcreteClassPropertyDefaults.md). The
[accepted reset design](../FutureArchitecture/AuthoritativePropertyResetToDefault.md)
remains in force. Qualification is recorded in the
[reset receipt](../Validation/AuthoritativePropertyResetToDefault.md).
The native V1 reset surface and per-target prepared batch consumption model are
qualified on MSVC Release and Linux ASan/UBSan/LSan and ready for Studio consumption.

## Native operation and eligibility

`Instance::ResetPropertyToDefault` requires the frozen active registry, resolves
the actual concrete class and original declaring property, and calls
`ResolveEffectivePropertyDefault` with both schema IDs and exact versions.
The nearest class-owned override wins; otherwise `Unmodified` is the fallback.
The declaring property and its identity remain unchanged.

This slice supports native concrete classes and generated editable properties
accepted by the existing `PreparedPropertyCommit::SupportsProperty` query, when
they are saved properties or the ordinary persisted Instance `Name` identity.
Using that eligibility query does not execute the prepared coordinator. The
resolved value must encode through `EncodePropertyDefault`; normal wire mutation
then validates live object, execution domain, permissions, capability, native
type, enum, ranges and validators. Failure rejects before a setter is invoked.

The value goes through `ApplyPropertyWireMutation`, then the ordinary reflection
writer / `ApplyPropertyMutation`. This reuses native-enum normalization and its
no-op check. There are no raw stores, reset-specific records, defaults table,
transaction type or authority token. Already-equal scalar and enum resets follow
ordinary no-op semantics: no revision, journal or notification.

Runtime/native mutations remain outside Studio history, exactly like ordinary
runtime assignments. An editor single reset sends the resolved value through
existing semantic `SetProperty`, with exact concrete and declaring versions and
`ExpectedRevision`. It receives one ordinary implicit authoring action. Undo
restores Before; Redo reuses the captured After value, without recomputing a
default. Normal native publication/observer failure behavior is unchanged; the
stronger prepared-batch failure proof is not attributed to ordinary setters.

Excluded are transient/contextual and read-only state (including `GuiState` and
`Workspace.CurrentCamera`), hierarchy and derived aliases, object references,
Source, handwritten/override setters, custom concrete classes, custom properties,
extension maps, Attributes and Tags. Custom construction's Name binding is not
an authoritative inherited default declaration. Even native inherited properties
on custom objects stay outside this reset slice. Ordinary editing remains
available under its existing rules. These exclusions do not change the
effective-default value resolver or prepared-safe eligibility.

## Discovery and compatibility

`GetSchema` accepts an optional `SchemaDiscoveryVersion` of 6 or 7. Omission
returns version 6, as does explicit 6, preserving the inspected Studio reader
that accepts only 5/6. Version 6 omits class-owned overrides and retains declared
property defaults and ordinary editing metadata. Version 7 must be requested:

```json
{ "SchemaDiscoveryVersion": 7 }
```

Its existing `Definitions[].DefaultOverrides`, concrete/base IDs and versions,
declaring properties and registry generation are sufficient. No additional
resolved table, reset RPC or capability is needed. Invalid/unknown version
requests reject with `UnsupportedCapabilityVersion`; unknown fields reject with
`MalformedRequest`. Protocol major 1 and SetPropertyBatch V1 are unchanged.

This explicit selection corrects the foundation's parameterless-v7 response:
inspection of Studio `StudioSchemaCache.Refresh` at
`d3b1c6b77e73105a0331b4925bd5cb347c3e6e28` (`foundation/studio-authoring`) proved that an
unsolicited v7 response would disable ordinary schema-backed editing. A new
consumer can first request v7; older Engines reject the parameter, allowing
ordinary discovery fallback while reset stays disabled. Successful v6 discovery
never authorizes concrete-default reset. No Studio files are changed here.

A future client must validate the complete v7 graph and each override's canonical
declaring identity/version before resolving. Cache keys include connection,
registry generation, concrete ID/version and declaring ID/version/property.
Use the nearest override along the base chain, then the declared default. Do not
guess from class names, constructors, neighboring objects or current values.

## One complete selection, one batch

Preflight every selected target: native concrete class, persistent supported
property (or native Instance Name), effective read/write access,
`AtomicBatchWritable`, resolvable effective default, current schema identities,
live target and scope, exact project revision and all existing resource bounds.
An unsupported target disables the complete gesture, never a selected subset.

Send full selection intent in one existing `SetPropertyBatch`. A TextLabel and
TextBox sharing `Engine.GuiObject.BackgroundTransparency` legitimately carry
different values, 1 and 0. They keep the same declaring-property identity.
Include already-equal targets; the coordinator validates all writes before
normalizing no-ops. One changed set produces one revision/action and normal
property journal records. Complete no-op produces none. Undo/Redo prepare and
atomically install captured values for the entire changed set.

The existing bounds remain 1–256 writes, 1 MiB request, 4 MiB prepared values,
2 MiB journal accounting and 8 MiB semantic history action. A 257-target gesture
rejects without chunking, even when some targets might prove to be no-ops.
Default resolution adds no retained table or mutation protocol bytes.

Within an open project the registry is frozen and object class identity cannot
change. Declaring version, generation-safe object, scope and exact revision are
revalidated by the existing batch admission/coordinator. Project replacement
retires the world before registry replacement; even a delayed batch with a
coincidentally equal revision rejects its old scope. Connection/registry changes
must retire client defaults before dispatch. An ordinary batch carries actual
values, not an assertion that they were defaults; Engine does not reinterpret
arbitrary value assignment as a reset request.

Journal, notifications and replication use ordinary `PropertyUpdated` values.
Persistence/Clone still preserve explicit saved state and do not elide defaults.
No Foundation 3L, prepared atomicity, history representation, or Studio UX change
is part of this implementation.

The next Studio task is v7 opt-in and validated cache resolution, followed by
single/multi-object Properties reset controls using these existing mutation
operations, whole-selection admission, and authoritative history/reconciliation.
