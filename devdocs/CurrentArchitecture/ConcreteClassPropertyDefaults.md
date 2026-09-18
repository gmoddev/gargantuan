# Concrete-class property default foundation

This implements the declaration and construction portion of the
[accepted design](../FutureArchitecture/ConcreteClassPropertyDefaults.md).
Reset mutation and Studio reset controls are separate, unqualified work.

## One declaration

Native class declarations accept a sparse ordered list:

```luau
defaultOverrides = {
    { Property = "BackgroundTransparency", Value = 0 },
    { Property = "BackgroundColor3", Value = { 0.08, 0.10, 0.14 } },
    { Property = "TextXAlignment", Value = "Left" },
}
```

The list preserves duplicate names for rejection. `Value` is a closed literal,
not C++ source. The first implementation supports the four literal families
needed by the audited GUI defaults: Boolean, Float, Color3 component triples,
and native enum item identifiers. Other override literal types reject explicitly;
ordinary declared defaults of other types continue to resolve as fallbacks.

Classgen resolves the inherited declaration and rejects missing/own properties,
duplicates, malformed records/components, wrong literal types, nonfinite/out-of-
range floats, metadata fields, and nonpersistent/read-only/override-setter
properties. C++ compilation validates the exact native enum item/type. Frozen
schema validation repeats identity/type checks and checks native validators,
ranges, enum values, closed wire encoding, and resource bounds. There are no new
properties, setter callbacks, or changes to prepared eligibility.

## Construction order

For each override, classgen emits one typed static value function. Both the
class-definition metadata and raw inherited-member initialization call it.
The generated `const bool <Class>DefaultOverridesInitialized` default member
initializer applies the raw stores and returns true. C++ initializes the base
first, then this member before the handwritten constructor body. Each derived
class therefore applies its nearer overrides after the base's overrides.
Direct C++ construction and registry construction use the same mechanism.

No setter, registry lookup, notification, mutation transaction, revision, or
journal operation runs in this generated initializer. The marker costs one bool
subobject per override-owning class in an instance's base chain, plus ABI padding;
it is not a per-instance default table. Classgen freshness includes base class
declarations and generator inputs to prevent stale inherited output.

The 16 migrated overrides are TextLabel (1), ImageLabel (1), TextBox (6),
TextButton (5), and ScrollingFrame (3). Their persistent constructor assignments
were removed. Transient `GuiState = Idle`, contextual `Workspace.CurrentCamera`,
operation-specific EditorHost `Archivable`, and custom-class Name binding retain
their existing behavior. The redundant ScrollingFrame `ClipsDescendants = true`
assignment also remains unchanged.

## Frozen schema and resolution

`SchemaClassDefinition::DefaultOverrides` owns a sparse vector of
`SchemaPropertyDefaultOverride`: declaring class ID/version, property name,
and exact native `std::any` value. `AllProperties` still points to the single
declaring `InstanceProperty`; `Unmodified` remains its declared fallback.

`RuntimeSchemaRegistry::ResolveEffectivePropertyDefault` accepts concrete ID/
version and declaring ID/version/property name. It checks both identities, walks
the frozen concrete-to-declaring base chain, and returns the nearest override or
the declaring fallback. It returns null for invalid/stale identity, signals,
custom properties, or absent fallback storage. Its borrowed native value is
valid only while that registry remains alive; cache consumers must retain the
registry generation and session boundary. The resolver describes values and
does not grant write/reset permission. Transient properties can have fallback
metadata without being construction-equivalent or reset eligible.

Native definitions remain version 1 under existing native migration policy.
Custom inherited override registration is rejected; custom Name construction
must not be inferred as reset metadata. No third reset-default table exists.
`Instance::ResetPropertyToDefault` is intentionally unchanged in this slice.

## Discovery version 7

EditorHost protocol version 1 and SetPropertyBatch version 1 are unchanged.
`GetSchema` now returns `SchemaDiscoveryVersion: 7`. Only class definitions with
authored overrides add `DefaultOverrides`:

```json
{
  "DeclaringClassSchemaId": "<32 lowercase hex digits>",
  "DeclaringDefinitionVersion": 1,
  "Property": "BackgroundTransparency",
  "CanonicalName": "Engine.GuiObject.BackgroundTransparency",
  "Default": { "Type": "Float", "Value": 1.0 }
}
```

Concrete identity/version come from the containing class definition. Clients
walk existing `BaseSchemaId` links and fall back to the declaring property's
`Default`. Empty override lists are absent. Both default paths share production
closed-value/native-enum encoding. Invalid override metadata fails registry
publication, rather than causing discovery to invent or omit a value.

`Definitions[].Properties[].Default` and legacy `Classes` keep their declared-
fallback meaning. Older clients consuming the legacy projection can ignore the
new records. Exact-version schema readers must fail closed on version 7 until
updated; the inspected Studio reader accepts only 5/6 and therefore needs a
separate compatibility update. This Engine change does not claim that existing
Studio schema readers already support concrete defaults.

## Bounds and persistence

Limits are 64 overrides per class, 4,096 per native registry, 4 KiB encoded value
per override, and 64 KiB aggregate encoded values plus property-name bytes.
Inheritance remains bounded by the existing 16-level class-depth limit. There
is no flattened concrete-classes-times-inherited-properties default table.

Each class adds one vector object; each actual override adds one fixed record,
name storage, and `std::any` native storage. The test prints ABI record size,
native value/name bytes, and exact added discovery bytes. Static class seeds and
the active registry retain their normal schema copies; these costs are per
schema copy, not per live Instance. See the
[qualification receipt](../Validation/ConcreteClassPropertyDefaults.md).

Native persistence still emits all readable/writable saved properties. Loading
constructs the concrete type and applies every saved value. Clone continues to
copy exact saved values through detached serialization. No persistence format
or default elision changed. Any future native default elision must compare to
the effective concrete-class default, not merely declared `Unmodified`.
