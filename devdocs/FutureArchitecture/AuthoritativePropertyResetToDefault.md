---
status: accepted
date: 2026-09-17
supersedes: null
superseded_by: devdocs/FutureArchitecture/ConcreteClassPropertyDefaults.md
owner: editorhost
---

# Authoritative property reset-to-default contract

Baseline: `feature/editorhost-property-batch` at
`04e1353edfe24ffc9d23968f8791afaf245f8ee8`.

## Amendment

The original version of this document selected:

> A — authoritative defaults already exist in schema metadata.

That conclusion is **superseded for inherited native properties** by
[Concrete-class property defaults](ConcreteClassPropertyDefaults.md).

The transport/mutation conclusion remains accepted: reset is still an ordinary
authoritative property assignment and multi-object reset still maps to one
`SetPropertyBatch`. No reset-specific mutation authority is required.

What changed is the source of the value:

```
OLD assumption:
    InstanceProperty::Unmodified
      == fresh value
      == reset value

CURRENT contract:
    declared property default
      + nearest concrete-class default override
      -> effective concrete-class default
      == fresh authored value
      == reset value
```

Constructor-only persistent authored defaults are not acceptable as a second
truth set. Transient/contextual construction state remains distinct and is not
made resettable merely because a constructor assigns it.

Reset qualification is blocked until the effective-default foundation described
by the superseding ADR is implemented and qualified.

## Authoritative reset value

For a reset-eligible native property, Studio consumes the Engine-derived
**effective concrete-class default**.

The declaring `InstanceProperty::Unmodified` remains the declared fallback.
A concrete class may own a narrow inherited-default override without redeclaring
the property. The effective value is the nearest override from the concrete
class toward the declaring class, otherwise the declaring fallback.

The declaring property retains its original:

- `SchemaId`;
- declaring definition version;
- name/canonical identity;
- type/wire type;
- access/security metadata;
- persistence/replication policy;
- validators;
- enum/reference semantics; and
- prepared-storage eligibility.

Default override is not property shadowing.

Declarative custom/extension properties keep their existing
`DefaultValue` semantics. Their current mutation families remain outside the
native prepared batch.

## Discovery

The current `GetSchema` response exposes declared native `Default` values
from `InstanceProperty::Unmodified`, but it does not encode concrete-class
inherited default overrides.

Reset-aware discovery therefore requires the concrete-default schema extension
defined in the superseding ADR.

The preferred representation keeps declared property `Default` unchanged and
adds bounded class-owned override records. A client resolves the effective value
using the active class chain. It must not:

- instantiate a local class;
- inspect another selected object;
- use current object state;
- hardcode class/property knowledge; or
- treat the declaring fallback as effective when a concrete override exists.

Resolved defaults are scoped to:

- EditorHost connection/session lifetime;
- active `RegistryGeneration`;
- concrete class schema ID/definition version;
- declaring class schema ID/definition version; and
- property identity.

Reconnect, project/registry replacement, or incompatible refreshed schema
retires the value.

A schema-discovery version bump is expected for the effective-default metadata.
The EditorHost protocol major version and property-batch mutation version do not
need to change.

## Reset eligibility

Authoritative default availability and mutation eligibility remain independent.

V1 native multi-object reset requires for every selected target:

1. an Engine-resolved effective default;
2. readable+writable effective Studio access;
3. `AtomicBatchWritable`;
4. current object/scope/revision/schema identity;
5. existing value/request/prepared resource bounds; and
6. ordinary EditorHost admission, including stopped Play and no conflicting
   authoring group.

Do not infer eligibility from datatype or writability alone.

Current prepared-V1 families remain:

- ordinary generated bool/integer/number/string;
- Vector2/Vector3/Color3;
- UDim/UDim2/CFrame; and
- native enum,

when their property is prepared-safe and access permits.

Still excluded:

- object references;
- handwritten/override setters;
- `Source`;
- custom/extension maps;
- Attributes;
- Tags;
- hierarchy;
- read-only/derived/runtime state; and
- contextual constructor state.

Concrete default override metadata does not itself make an excluded mutation
family prepared-safe.

## Mutation mapping

No `ResetProperty` EditorHost command is introduced.

Canonical reset remains:

```
current schema + concrete class
    -> resolve effective authoritative default per target
    -> preflight complete selection
    -> one SetPropertyBatch at exact scope/revision
    -> PreparedPropertyCommit
    -> ordinary history/journal/replication
```

Each write keeps the existing V1 identity:

```json
{
  "Object": { "Slot": 10, "Generation": 3 },
  "DeclaringClassSchemaId": "...",
  "DeclaringDefinitionVersion": 1,
  "Property": "BackgroundTransparency",
  "Value": { "Type": "Float", "Value": 1.0 }
}
```

The default's concrete-class ownership is discovery/cache context. It does not
replace the canonical declaring-property identity used by mutation.

## Multi-object semantics

One reset gesture is whole-selection intent. Studio must never silently reset an
eligible subset.

| Case | Result |
| --- | --- |
| same property / same effective default | one write per target with that value |
| same declaring property / different concrete defaults | each target carries its own Engine-resolved effective value in the same batch |
| any target lacks effective default | reset unavailable for the complete selection |
| any target read-only/unsupported | reset unavailable for the complete selection |
| current values common | reset all targets to their own effective defaults |
| current values mixed | reset all targets to their own effective defaults |
| some targets already at default | include full intent; coordinator normalizes no-op writes |
| all targets already at default | complete no-op: no revision/history/journal |

Different values do not violate atomicity. `SetPropertyBatch` already supports
different values per write.

## Prepared-property integration

Reset does not bypass preparation because its value came from schema metadata.

All existing V1 guarantees remain:

- 1–256 writes;
- no chunking;
- no duplicate object/property pair;
- current scope;
- exact authoritative revision;
- exact declaring schema/version;
- whole-batch validation and installation;
- one history action for the changed set;
- no-op normalization;
- ordinary notification handling; and
- whole-request failure with no mutation.

Malformed/stale effective-default metadata must fail before dispatch or through
normal Engine validation. Studio never substitutes another value.

## No-op / history / Undo / Redo

Existing prepared semantics are the reset semantics.

- already-equal targets are no-op writes;
- mixed no-op/change batches record only changed targets;
- complete no-op creates no revision/history/journal state;
- one successful gesture creates one authoritative history action;
- Undo restores previous changed values;
- Redo reapplies the exact effective defaults captured in the action.

Redo does not re-resolve a newer schema default.

No Studio-local reset history exists.

## Journal / replication

Successful reset publishes ordinary `PropertyUpdated` state for changed
targets. Existing journal reconciliation and replication consume it.

There is no reset-specific journal record or replication opcode.

Failed or complete-no-op reset publishes nothing.

## Persistence and cloning

Current native persistence serializes every readable+writable saved property; it
does not omit values equal to `Unmodified`.

Therefore the discovered constructor/default mismatch is not presently a
save/reopen fidelity defect.

Clone/duplicate serializes and reconstructs saved state and then copies writable
transient state. Exact source values remain authoritative.

If native default elision is added later, it must compare against the **effective
concrete-class default**, not merely the declaring `Unmodified`.

## Compatibility

Older Engines that expose only declaring `Default` metadata cannot truthfully
support reset for classes whose effective default may differ.

Compatibility direction:

- inspection and existing editing continue;
- single/multi reset controls remain unavailable without the effective-default
  discovery version;
- Studio never treats legacy declaring defaults as concrete defaults by guess;
- property-batch V1 remains usable for ordinary multi-editing;
- no EditorHost protocol-major change is required.

## Gate

**RESET ARCHITECTURE TRANSPORT/MUTATION: ACCEPTED.**

**AUTHORITATIVE DEFAULT SOURCE: SUPERSEDED by
`ConcreteClassPropertyDefaults.md`.**

**RESET QUALIFICATION: BLOCKED.**

The exact next Engine task is to implement and qualify the narrow
concrete-class inherited-default foundation first. Only then should the
reset-through-`SetPropertyBatch` qualification resume.

Studio reset controls remain out of scope until that gate passes.
