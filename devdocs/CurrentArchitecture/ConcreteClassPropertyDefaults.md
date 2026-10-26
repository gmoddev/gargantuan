# Concrete-class property default foundation

This implements the declaration and construction portion of the
[accepted design](../FutureArchitecture/ConcreteClassPropertyDefaults.md).
Reset mutation is implemented separately; see the
[reset contract and qualification status](AuthoritativePropertyResetToDefault.md).
Studio reset controls remain separate work.
The foundation is qualified on MSVC Release and Linux ASan/UBSan/LSan; see the
[validation receipt](../Validation/ConcreteClassPropertyDefaults.md).

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

Classgen also materializes declared string literals in their declared native
type (`std::string` or `std::string_view`) before storing `Unmodified`.
The old generator stored those literals as `const char*`
inside `std::any`, so closed-value discovery omitted them even though backing
members held native strings. The authored declared value is unchanged; its
native storage and discovery now match construction. This correction is
required for the no-override fallback equivalence gate.

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
The subsequent reset implementation consumes this resolver through ordinary
mutation; the foundation itself does not grant mutation authority.

## Discovery version 7

EditorHost protocol version 1 and SetPropertyBatch version 1 are unchanged.
`GetSchema` returns `SchemaDiscoveryVersion: 7` when explicitly requested with
`Params.SchemaDiscoveryVersion = 7`; omission returns compatible v6. Only v7 class definitions with
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
fallback meaning; previously omitted string defaults now encode as strings.
Older clients receive v6 without the new records. The reset integration added
explicit v7 selection because the inspected Studio reader accepts only 5/6.
New clients must request v7 and fail closed for reset when it is unavailable;
legacy discovery still supports ordinary editing.

## Bounds and persistence

Limits are 64 overrides per class, 4,096 per native registry, 4 KiB encoded value
per override, and 64 KiB aggregate encoded values plus property-name bytes.
Inheritance remains bounded by the existing 16-level class-depth limit. There
is no flattened concrete-classes-times-inherited-properties default table.

Each class adds one vector object; each actual override adds one fixed record,
name storage, and `std::any` native storage. The test prints ABI record size,
native value/name bytes, and exact added discovery bytes. Static class seeds and
the active registry retain their normal schema copies; these costs are per
schema copy, not per live Instance. The normal native-only steady state retains
three copies: generated `CLASS_DEFINITION` objects, registration seeds, and the
active registry. On MSVC, their additional fixed member/record payload totals
10,224 bytes (`3 * (16 * 120 + 62 * 24)`), excluding name allocations,
container capacity slack, and allocator overhead. This is an object-payload
account, not a process-RSS or allocator-reservation measurement. Project
registries also retain the common `SchemaDefinition` variant storage for each
additional bounded custom class/enum/extension definition; those definitions
cannot add inherited native override records. With a 24-byte vector on the
qualified ABIs, the added variant payload is bounded by 24 bytes per additional
definition, or at most `24 * (64 + 64 + 64) = 4,608` bytes per project registry
under the existing definition-count limits.
A concurrently retained candidate or old registry adds its own bounded copy.
Linux/libstdc++ measures a 72-byte record and 24-byte vector: 2,640 fixed
member/record bytes per native schema copy, or 7,920 for the usual three copies,
plus name/value allocation. Both platforms measure 62 native value bytes,
227 property-name bytes, and 672 encoded value bytes across all 16 overrides.
Separately, the declared-string correction makes 22 existing literal defaults
own native strings and two retain native string views inside their existing
`std::any` fields; allocation depends
on the standard-library small-object/string optimization. For the 62 production
classes it adds 2,493 compact JSON bytes across declared and legacy inherited
projections. That delta is derived from each newly encodable literal's wire
record and its existing inheritance fan-out; it is additional to the fixture's
measured `DefaultOverrides` delta. No new string-default table is stored.
That measured override delta is 3,695 bytes on both qualified platforms, for
a total production discovery increase of 6,188 bytes including string defaults.
See the
[qualification receipt](../Validation/ConcreteClassPropertyDefaults.md).

Native persistence still emits all readable/writable saved properties. Loading
constructs the concrete type and applies every saved value. Clone continues to
copy exact saved values through detached serialization. No persistence format
or default elision changed. Any future native default elision must compare to
the effective concrete-class default, not merely declared `Unmodified`.
