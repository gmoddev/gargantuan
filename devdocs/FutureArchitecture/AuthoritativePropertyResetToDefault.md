# Authoritative property reset-to-default contract

Status: **accepted architecture; ready for implementation qualification**  
Decision: **A — authoritative defaults already exist in schema metadata**  
Baseline: `feature/editorhost-property-batch` at `04e1353edfe24ffc9d23968f8791afaf245f8ee8`  
Scope: Engine/runtime schema, EditorHost discovery, and Studio Properties reset semantics. This document does not implement reset controls or a new mutation command.

## Decision

Studio reset-to-default uses the Engine-owned default already represented by native `InstanceProperty::Unmodified` and declarative schema `DefaultValue`. EditorHost's existing optional native-property `Default` field is the wire representation of that value. No second Studio default table, synthetic instance construction, separate default lookup API, or `ResetProperty` EditorHost mutation is introduced.

For a reset-eligible native property, Studio resolves the current authoritative default from the active schema definition and submits that value through the ordinary qualified property mutation path. Multi-object reset uses one `SetPropertyBatch` request containing one write per selected target. Different targets may carry different default values when their effective schema identities legitimately differ; Engine atomicity does not require all writes to carry the same value.

The existing Engine runtime method `Instance::ResetPropertyToDefault` is supporting evidence for the semantic choice, not a Studio authority boundary: it resolves the reflected property and applies `property->Unmodified` as an ordinary property mutation. Studio must not call or emulate that internal method through a new privileged path.

## Canonical default source

### Generated native properties

The class declaration is the single source. `tools/libs/classes.luau` requires `ClassProperty.unmodified`. `tools/classgen.luau` consumes that same declaration expression in both places that matter:

1. `.SetUnmodified(property.unmodified)` populates the reflected `InstanceProperty` metadata; and
2. for ordinary generated non-override properties, the backing member is initialized as `Member = property.unmodified`.

The generator therefore does not maintain a construction default and an editor default independently. For the generated prepared-safe surface, the schema default and fresh-instance initial backing value are produced from one declaration.

`EditorHost::GetSchema` encodes the reflected value. Ordinary closed wire values use `EncodeNativeWireValue(Property.Unmodified)`. Native enums translate the enum value stored in `Unmodified` to the canonical enum type/item identity. Nullable object references can expose a null default; non-nullable references do not synthesize one.

### Declarative custom-class and extension properties

`SchemaClassProperty::DefaultValue` and `SchemaExtensionProperty::DefaultValue` are already first-class frozen schema data. Registration validates exact declared type, finite numbers, UTF-8/NUL rules, a 4 KiB encoded-default limit, and aggregate custom-schema bounds. Missing per-instance overrides read the frozen declaring-schema default.

These defaults are authoritative, but the current prepared native-property batch intentionally excludes custom/extension maps. Their existence does not make them eligible for V1 multi-object reset.

### Three distinct concepts

| Concept | Contract |
| --- | --- |
| **Static property default** | The schema-owned value to which an authoritative reset assigns. Native: `InstanceProperty::Unmodified`. Declarative schema: `DefaultValue`. This is what EditorHost `Default` represents when present. |
| **Instance initial value** | The value an object has immediately after authoritative construction. Ordinary generated non-override properties use the same declaration expression as the static default. A handwritten/override implementation can perform additional construction or derived behavior and therefore must not be assumed construction-equivalent solely because `Unmodified` exists. |
| **Contextual/derived default** | A value whose meaning depends on parent, world, runtime state, another object, or other context. V1 Studio reset does not synthesize or infer such a value. Unless Engine later represents it as an authoritative reset operation with its own qualified semantics, reset is unsupported. |

A reset means **assign the static property default**, not “re-run object construction” and not “recompute whatever value a new object might currently have in this context.”

## Inheritance and definition identity

The current runtime schema flattens inheritance by retaining pointers to the declaring `InstanceProperty`. Validation rejects a subclass member that collides with an inherited property or method. A subclass therefore cannot currently override an inherited native property/default under the same name.

Consequences:

- the declaring schema owns the current native default;
- an inherited property keeps its declaring `SchemaId` and `DeclaringDefinitionVersion`;
- unrelated properties with the same display/name string are not the same semantic property;
- Studio must use declaring schema identity and exact definition version, never property name alone;
- a future architecture that permits property/default override must represent the effective declaring definition explicitly rather than silently changing a base property's meaning.

`SchemaId` is stable identity and does not include definition version. A semantic default change is schema-definition behavior. Project-defined schemas already carry explicit versions. Native schemas are fixed for the lifetime of one Engine process; Studio must still invalidate all cached schema/default data on EditorHost reconnect. If future native hot-regeneration can change a definition without process replacement, a changed default requires a changed definition version.

## Schema discovery contract

The minimum discovery representation is the one already present:

```json
{
  "DeclaringSchemaId": "...",
  "DeclaringDefinitionVersion": 1,
  "AtomicBatchWritable": true,
  "Default": { "Type": "...", "Value": ... }
}
```

The declaring identity/version is currently carried by the definition/property relationship rather than necessarily repeated in exactly this JSON shape; the semantic tuple is what matters.

For native property metadata:

- presence of `Default` means **HasAuthoritativeDefault = true**;
- absence of `Default` means **HasAuthoritativeDefault = false**;
- no separate boolean is needed because it would duplicate the optional-value state;
- `Default` is a closed `WireValue`, not display text;
- `Default` does not itself grant write or reset authority.

Studio must associate a cached default with all of:

- the current EditorHost connection/session lifetime;
- current `RegistryGeneration`;
- declaring `SchemaId`;
- exact declaring definition version; and
- canonical property identity.

Project replacement, Engine reconnect, registry-generation change, missing definition, version mismatch, or incompatible property metadata invalidates the cached default. Studio re-discovers; it does not carry a default across that boundary.

No separate lookup API is justified. Defaults are already part of bounded schema discovery, are reused by normal property inspection, and adding a second lookup would introduce a second cache/staleness protocol without reducing current wire cost.

## Reset eligibility

Authoritative default availability and reset eligibility are separate predicates.

A native Studio property may expose V1 reset only when all of the following are true at the current selection/gesture:

1. current schema metadata contains an authoritative `Default` that decodes as the property's declared closed wire type;
2. the property is readable and writable for the effective Studio security context and is not read-only;
3. for multi-object reset, `AtomicBatchWritable` is true for every selected target/property;
4. the value and complete request fit existing `SetPropertyBatch` value, count, envelope, preparation, journal, and history bounds;
5. the current object/scope/revision/schema identities are available; and
6. no ordinary existing admission rule (Play, open transaction, capability, lifecycle, etc.) blocks the mutation.

Current classification:

| Property family | Authoritative default | V1 multi-object reset |
| --- | --- | --- |
| Ordinary generated bool/integer/number/string | `Unmodified`; same generator source as backing initializer | **Supported when `AtomicBatchWritable` and access permit** |
| Generated Vector2/Vector3/Color3 | `Unmodified` closed wire value | **Supported when `AtomicBatchWritable` and access permit** |
| Generated UDim/UDim2/CFrame | `Unmodified` closed wire value | **Supported when `AtomicBatchWritable` and access permit** |
| Generated native enum | `Unmodified`, encoded as canonical enum type/item | **Supported when `AtomicBatchWritable` and access permit** |
| Object references | nullable references can have authoritative null; non-nullable references may have no encodable static default | **Unsupported in prepared batch V1** |
| Handwritten/override getter/setter | `Unmodified` may describe Engine's static reset target, but construction/derived behavior is not inferred from it | **Unsupported in prepared batch V1** |
| `Source` | generic default is not authority for the dedicated conflict-token source operation | **Unsupported** |
| Custom-class properties | frozen declaring-schema `DefaultValue` | **Unsupported in native prepared batch V1** |
| Extension properties | frozen extension `DefaultValue` | **Unsupported in native prepared batch V1** |
| Attributes | no class-property default contract | **Unsupported** |
| Tags | membership, not a defaulted property | **Unsupported** |
| Parent/hierarchy-derived state | structural/hierarchy semantics, not prepared property storage | **Unsupported** |
| Read-only properties/signals/derived state | default metadata may exist for reflection, but there is no eligible write | **Unsupported** |

Studio must not infer reset support from writability or datatype. It must not silently route excluded families through their dedicated setters merely to make a reset button appear. Those families require a separately designed atomic multi-object contract if reset is desired later.

## Mutation mapping

No `ResetProperty` EditorHost method is added.

The canonical multi-object operation is:

```text
current schema discovery
  -> resolve each selected target's authoritative Default
  -> preflight complete canonical selection
  -> one SetPropertyBatch at exact scope/revision
  -> ordinary PreparedPropertyCommit
  -> normal history + journal + replication
```

Each write retains the existing V1 fields:

```json
{
  "Object": { "Slot": 10, "Generation": 3 },
  "DeclaringClassSchemaId": "...",
  "DeclaringDefinitionVersion": 1,
  "Property": "Transparency",
  "Value": { "Type": "Float", "Value": 0.0 }
}
```

The fact that `Value` came from schema default metadata does not bypass or weaken preparation. Engine must perform the same schema, authority, type, enum, range/native-validator, resource, stale-object, scope, and exact-revision checks as any other batch write.

For single selection, reset is also semantically an ordinary value assignment. A Studio implementation may reuse the existing single-property path when it already provides the required authoritative identity/validation/history behavior. Multi-selection must use `SetPropertyBatch`; sequential `SetProperty` calls are not an atomic fallback.

## Multi-object reset semantics

One reset gesture is whole-selection intent. Studio never silently reduces it to the eligible subset.

| Case | Required result |
| --- | --- |
| All targets expose the same compatible property and same default | Submit one write per target carrying that default in one batch. |
| Compatible property but targets legitimately have different authoritative defaults | Resolve each target independently and put each target's own default in the same batch. Equal write values are not required for atomicity. Current Studio V1 common-property projection is stricter and requires the same declaring schema/version, so this case normally does not form one row today; this rule defines Engine semantics if a future compatible grouping broadens that projection. |
| Some targets support reset and others do not | Reset is unavailable/rejected for the complete selection; send no subset. |
| One target is read-only | Reset is unavailable/rejected for the complete selection; send no subset. |
| Current values are common | Reset all targets to their authoritative defaults; common current value does not change identity semantics. |
| Current values are mixed | Reset all targets to their authoritative defaults; mixed presentation is not a value and is never submitted. |
| Some targets are already at their own defaults | Include the complete intended target set; the Engine coordinator normalizes those writes to no-ops and commits only changed writes. |
| All targets are already at their own defaults | Complete no-op: no revision, history action, transaction ID, journal record, or replication publication. |

If Studio ever observes different `Default` values for the same declaring `SchemaId`, exact definition version, registry generation, and property identity, that is inconsistent schema state. It must retire the reset intent and refresh schema rather than choosing one value.

## Prepared-property integration and failure atomicity

Reset preserves the entire existing prepared-property contract:

- 1–256 writes;
- no chunking;
- no duplicate object/property pair;
- current snapshot scope;
- exact authoritative revision;
- exact declaring schema ID/version;
- effective `AtomicBatchWritable` eligibility;
- all-or-nothing preparation/installation;
- one authoritative history action for the changed set;
- ordinary post-commit notification handling; and
- normal whole-request error categories.

Malformed default metadata, stale definition/version, decode/type failure, enum mismatch, native validator/range rejection, unsupported prepared storage, and resource overflow all fail before installation. A batch failure publishes no live write, revision, history, journal, or replication state.

An Engine schema bug can therefore make reset unavailable or make the attempted batch fail safely; Studio never repairs or substitutes the default.

## No-op, history, Undo/Redo

Existing coordinator behavior is the reset behavior:

- a target already equal to its submitted default is a no-op write;
- mixed changed/no-op batches record only changed targets;
- a complete no-op does not advance revision or create history/journal state;
- one successful reset gesture produces one history action regardless of changed target count;
- Undo restores the exact previous values for the changed targets;
- Redo reapplies the exact default values captured by that history action.

Redo does not re-query a potentially different later schema default. Project/schema replacement retires the world/history boundary rather than reinterpret an old history action under a new definition.

Studio owns no reset-specific inverse or local reset history.

## Journal and replication

A successful reset is ordinary property mutation. It emits the normal `PropertyUpdated` journal records for changed targets at the one resulting authoritative revision. Existing reconciliation and replication consume those records; no reset record type, reset replication opcode, or Studio-local optimistic patch is added.

A failed or complete-no-op reset publishes nothing.

## Bounds and protocol impact

This decision adds **zero schema-discovery bytes** and **zero new protocol fields** because native `Default` is already emitted today. The existing JSON field costs `11` bytes for `,"Default":` plus the encoded `WireValue` in metadata that already contains it; this ADR does not duplicate that payload.

Existing bounds remain authoritative:

- EditorHost request: 1 MiB;
- EditorHost response: 8 MiB;
- `SetPropertyBatch`: 1–256 writes;
- property/identifier text: 256 UTF-8 bytes where the batch protocol applies;
- individual batch string value: 64 KiB;
- prepared coordinator: conservative 4 MiB aggregate old/new values, 2 MiB journal payload, 8 MiB history action, and 256 direct notifications/records;
- custom/extension declarative defaults: 4 KiB encoded each and 64 KiB aggregate custom-schema payload.

Native generated defaults do not currently need a second per-default discovery limit because discovery is already response-bounded and this design adds no new payload. Reset eligibility additionally requires that the selected default can pass the existing mutation/value bounds. A future large-default property that fits schema discovery but cannot fit its mutation contract is authoritative metadata but reset-unsupported for that gesture; Studio must not truncate it.

A bounded lookup API would add complexity without solving an existing payload increase and is rejected for V1.

## Staleness and compatibility

### Staleness

A default is valid only inside the schema snapshot from which it was obtained. Studio invalidates it on:

- EditorHost disconnect/reconnect;
- project replacement;
- registry-generation change;
- declaring definition removal;
- declaring definition-version mismatch; or
- incompatible refreshed property metadata.

`SetPropertyBatch` independently protects execution with project scope, exact revision, live generation-bearing object IDs, declaring schema identity/version, and the current frozen registry. A delayed reset discovered against an old project cannot be applied to a replacement project merely because a revision number happens to match.

### Older Engines

Compatibility is additive:

- existing inspection and ordinary editing continue when reset support is absent;
- missing `Default` means reset unavailable; Studio does not synthesize it;
- missing/false `AtomicBatchWritable`, missing `SetPropertyBatch`, or unsupported PropertyBatchVersion means multi-object reset unavailable;
- clients that ignore `Default` continue to operate;
- no EditorHost protocol-major change is required.

If an older Engine exposes a property default but lacks the qualified multi-object batch capability, Studio may still use its existing qualified single-property assignment path for a one-object reset if all other identity/access checks are satisfied. It must never emulate multi-object atomicity with sequential calls.

## Feasibility proof

No production reset implementation or synthetic-instance default discovery is required. The source supplies a mechanical construction proof for ordinary generated properties:

`ClassProperty.unmodified -> classgen backing initializer` and the same `ClassProperty.unmodified -> InstanceProperty::Unmodified -> EditorHost Default`.

Representative prepared-safe declarations on the baseline are:

| Type | Representative declaration | Declared / fresh generated value | Schema discovery value |
| --- | --- | --- | --- |
| scalar | `BasePart.Transparency` | `0.0f` | closed Float `0` |
| string | `TextLabel.Text` | `""` | closed String `""` |
| Vector3 | `BasePart.Size` | `(1,1,1)` | Vector3 `(1,1,1)` |
| Color3 | `BasePart.Color` | `(1,1,1)` | Color3 `(1,1,1)` |
| UDim | `UIListLayout.Padding` | scale `0`, offset `0` | UDim `(0,0)` |
| UDim2 | `GuiObject.Position` | all zero | UDim2 all zero |
| CFrame | `BasePart.CFrame` | identity | identity CFrame |
| native enum | `Part.Shape` | `PartType::Block` | enum type `PartType`, item `Block` |

`Part` inherits the BasePart representatives through the flattened base property pointers without redefining their defaults, and concrete GUI classes inherit the GuiObject representatives the same way. The existing batch qualification already exercises generated string, enum, scalar, vector, color, UDim2 and CFrame writes plus no-op, atomic history, Undo/Redo, journal, replication, schema/version, and project-replacement rejection. The reset design adds only the source of each submitted `Value`.

A dedicated runtime default-equivalence test remains a useful first implementation-qualification addition, especially as regression protection for future classgen changes. It is not needed to discover defaults or to justify a new production API, so this architecture task does not add test-only duplicate encoding logic.

## Implementation sequence

1. **Engine qualification/documentation slice — no new mutation API.** Make the optional `GetSchema.Properties[].Default` semantics above normative in EditorHost/runtime-schema documentation. Add a focused regression test that, for representative generated/inherited scalar, string, vector/color, UDim/UDim2, CFrame, and native-enum properties, compares reflected `Unmodified`, a freshly constructed authoritative instance value, and the actual `GetSchema` encoded `Default`. Also prove absent/unusable defaults do not become reset eligibility. Do not alter Foundation 3L.
2. **Engine batch reset feasibility test.** Feed the discovered defaults back through one `SetPropertyBatch`, including a mixed current-value case and targets already at default. Reuse existing assertions for one revision/history action, complete no-op, Undo/Redo, journal/replication, stale schema/project, and resource rejection. This is qualification, not a `ResetProperty` implementation.
3. **Studio reset controls.** After the Engine qualification is green on the exact source Studio targets, consume only current schema `Default` values, derive whole-selection eligibility, build one batch with each target's own default, and use normal recovery/reconciliation. Do not synthesize defaults and do not sequentially fall back.
4. **Later property families only by separate design.** References, Source, custom/extension maps, attributes, tags, hierarchy, and handwritten/contextual setters remain excluded from V1 multi-reset until their own atomic semantics are qualified.

## Gate result

**READY FOR IMPLEMENTATION: YES**, with a narrow Engine qualification task first. No broad schema/instance redesign is required, no new EditorHost mutation authority is justified, multi-object all-or-nothing semantics fit the existing prepared batch, and default discovery does not add unbounded metadata.

Studio reset controls remain gated on the Engine qualification in steps 1–2. The exact next task is: **qualify `GetSchema`'s existing optional `Default` field as the authoritative reset value on `feature/editorhost-property-batch`, with representative construction/discovery equivalence and reset-through-`SetPropertyBatch` tests; make no production reset command.**
