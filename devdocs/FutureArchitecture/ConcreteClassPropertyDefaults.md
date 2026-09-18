---
status: accepted
date: 2026-09-17
supersedes: "The construction-equivalence/default-source portions of AuthoritativePropertyResetToDefault.md at 25154a9"
superseded_by: null
owner: runtime-schema
---

# Concrete-class property defaults

## Decision

**Combined B/C — concrete-class inherited default overrides are required, and
persistent constructor default drift is invalid once an authored property is
covered by this model.**

Gargantuan needs two default concepts, not three independent truth sets:

1. **declared property default** — the fallback value owned by the schema that
   declares the property; native properties currently store this as
   `InstanceProperty::Unmodified`;
2. **effective concrete-class default** — the value a fresh instance of one
   concrete class receives for that property and the value reset-to-default
   restores for that concrete class.

Construction initialization remains a separate lifecycle concept only for state
that is intentionally not an authored/resettable property default, such as
transient runtime state or context-owned objects.

For a reset-eligible persistent native property:

```
fresh authoritative construction
    ==
effective concrete-class default
    ==
reset-to-default target
```

The effective value is resolved from one declarative source. A subclass may
override only the **default value** of an inherited property. It does not
redeclare the property and cannot alter its type, access, persistence,
replication, validator, enum/reference identity, prepared-store support, or
declaring schema identity.

This decision supersedes the earlier assumption that
`InstanceProperty::Unmodified` is always construction-equivalent for inherited
generated properties.

## Source inventory

The baseline is `feature/editorhost-property-batch` at
`04e1353edfe24ffc9d23968f8791afaf245f8ee8`.

A complete pass over handwritten native class/service constructors found the
following property-value initialization that differs, or potentially differs,
from declaring reflected defaults.

### Persistent authored mismatches

| Concrete class | Inherited property | Declaring/effective-base default | Constructor value | Classification |
| --- | --- | --- | --- | --- |
| `TextLabel` | `GuiObject.BackgroundTransparency` | `0` | `1` | intentional concrete authored default |
| `ImageLabel` | `GuiObject.BackgroundTransparency` | `0` | `1` | intentional concrete authored default |
| `TextBox` | `BackgroundTransparency` | inherited `TextLabel` effective `1` | `0` | intentional concrete authored override |
| `TextBox` | `GuiObject.BackgroundColor3` | `(1,1,1)` | `(0.08,0.10,0.14)` | intentional concrete authored default |
| `TextBox` | `TextLabel.TextXAlignment` | `Center` | `Left` | intentional concrete authored default |
| `TextBox` | `GuiObject.Interactable` | `false` | `true` | intentional concrete authored default |
| `TextBox` | `GuiObject.Selectable` | `false` | `true` | intentional concrete authored default |
| `TextBox` | `GuiObject.InputSink` | `None` | `All` | intentional concrete authored default |
| `TextButton` | `BackgroundTransparency` | inherited `TextLabel` effective `1` | `0` | intentional concrete authored override |
| `TextButton` | `GuiObject.BackgroundColor3` | `(1,1,1)` | `(0.18,0.22,0.30)` | intentional concrete authored default |
| `TextButton` | `GuiObject.Interactable` | `false` | `true` | intentional concrete authored default |
| `TextButton` | `GuiObject.Selectable` | `false` | `true` | intentional concrete authored default |
| `TextButton` | `GuiObject.InputSink` | `None` | `Activate` | intentional concrete authored default |
| `ScrollingFrame` | `GuiObject.Interactable` | `false` | `true` | intentional concrete authored default |
| `ScrollingFrame` | `GuiObject.Selectable` | `false` | `true` | intentional concrete authored default |
| `ScrollingFrame` | `GuiObject.InputSink` | `None` | `All` | intentional concrete authored default |

`ScrollingFrame.ClipsDescendants = true` repeats the declaring default and is
not a mismatch.

The pattern is therefore **not isolated to TextLabel**. It is concentrated in
the GUI control hierarchy and is semantically coherent: label/image classes
choose transparent backgrounds, while editable/interactive controls choose
different interaction and presentation defaults.

### Construction initialization that is not an authored reset default

`TextBox`, `TextButton`, and `ScrollingFrame` assign
`GuiState = Idle` while the declaring default is `NonInteractable`.
`GuiState` is transient and read-only. This is runtime interaction state, not
an authored resettable default, and it must not force a general reset-default
override concept.

`Workspace` constructs `CurrentCamera` although the reflected property
fallback is null. `CurrentCamera` is read-only/transient and owns a runtime
object. It is contextual construction state, not a resettable property default.

Custom-class construction also has a related existing behavior:
`BindRuntimeSchemaClass` assigns inherited `Instance.Name` to the custom
class canonical name after constructing the native host. That is concrete-class
initialization outside `Unmodified`. The effective-default model must be
compatible with representing such inherited defaults later; reset support must
not infer them from construction.

EditorHost creation separately initializes `Archivable = true` for new project
objects. That is operation-specific authoring initialization, not a class
default.

## Constructor side effects

The GUI constructor assignments above write generated backing members directly.
They do not call the generated setters and therefore do not execute:

- `AssertCanMutate`;
- `ValidatePropertyMutation`;
- `NotifyPropertyCommitted`;
- ChangeJournal publication;
- authoritative revision advancement; or
- property-changed notifications.

They occur before normal object publication. The replacement declarative
construction mechanism must preserve that property: applying an effective
default during construction is initialization, not an authoritative mutation
action and must not create history/journal/revision state.

## Default model

### Declared property default

The declaring schema owns the property and its fallback default.

For native properties this remains `InstanceProperty::Unmodified`.
For declarative custom/extension properties it remains their existing
`DefaultValue`.

A declared default is **not automatically the effective default of every derived
concrete class**.

### Effective concrete-class default

For a concrete class C and property P:

1. preserve P's original declaring property identity;
2. walk the class chain from C toward P's declaring class;
3. use the nearest valid class-owned default override for P;
4. if none exists, use P's declared default.

Overrides inherit normally. A more-derived override wins.

Thus:

- `GuiObject.BackgroundTransparency` declared default = `0`;
- `TextLabel` effective default = `1`;
- `TextBox` effective default = `0`;
- `TextButton` effective default = `0`;
- a future subclass of `TextLabel` with no override inherits `1`.

No fake property is created. `FindProperty` and `AllProperties` still point
to the one declaring `InstanceProperty`.

### Construction initialization

A constructor may still establish state that is not an authored default when
that state is transient, read-only, contextual, runtime-owned, or otherwise not
eligible for reset semantics.

Persistent authored property values that are intended to define what a new
concrete class looks/behaves like must not remain handwritten constructor-only
truth after this architecture is implemented. They must be represented by the
effective-default declaration.

### Reset default

For reset-eligible properties, **reset default equals effective concrete-class
default**. Gargantuan does not introduce an independent reset-default table.

Candidate D is rejected: the repository demonstrates a need to distinguish
authored defaults from runtime construction state, but no case demonstrates a
need for a reset target different from the authored concrete-class default.

## Inheritance representation

The minimum native schema addition is class-owned inherited-default override
metadata conceptually equivalent to:

```
ConcreteClassSchemaId           // implicit from containing class definition
ConcreteDefinitionVersion       // implicit from containing class definition
DeclaringClassSchemaId
DeclaringDefinitionVersion
Property
DefaultValue
```

The concrete class owns only the override. The declaring schema continues to
own the property.

A class declaration should gain a narrow field such as:

```luau
defaultOverrides = {
    BackgroundTransparency = "1.0f",
}
```

The exact spelling is implementation detail. It must not permit declaring a new
property, changing property metadata, or shadowing an inherited member.

Runtime-schema validation must reject an override when:

- the named member is absent;
- it is not inherited by the concrete class;
- the value does not match the declaring property's native type;
- enum identity/value is invalid;
- the value violates a range/native validator applicable to a default;
- the property is a signal; or
- the default cannot be represented by the closed schema/wire default contract
  when discovery is required.

General inherited-property shadowing remains prohibited.

Changing an override is a semantic change to the concrete class definition.
Within one Engine process the frozen registry and `RegistryGeneration` provide
the cache boundary. Current native definitions remain version 1 under the
existing exact-version/migration policy; this ADR does not invent a native
schema migration system. Reconnect/re-discovery remains mandatory across Engine
process replacement. Project/custom definitions retain their explicit
definition versions.

## Construction implementation constraint

One declarative override value must feed both construction and schema/reset
discovery. Do not retain:

```
constructor assignment = X
schema override = X
```

as two author-maintained truths.

For native generated classes, classgen is the preferred compile-time boundary.
It should emit both:

1. the class-definition override metadata; and
2. automatic raw backing-store initialization for the concrete class from the
   same declaration expression.

The generated application must happen for direct native C++ construction as
well as `InstanceClassRegistry::Construct`; therefore applying defaults only by
a runtime registry lookup after generic construction is insufficient.

A classgen-generated in-class initializer/constructor preamble or equivalent
compile-time mechanism is appropriate. It should apply after base construction
and before handwritten constructor logic, so derived defaults naturally
override base defaults without calling setters or publishing mutations.

The current GUI constructors should retain only genuine runtime construction
logic after migration. Persistent authored assignments move to declarations.
Transient `GuiState` initialization may remain constructor logic.

## Schema discovery

Current schema discovery has two views:

- `Definitions[].Properties[]` contains **declared** native properties only;
- the legacy `Classes[].Properties[]` adapter flattens applicable inherited
  properties, but its current `Default` still comes directly from the
  declaring `InstanceProperty::Unmodified`.

Neither currently represents concrete-class effective defaults.

The next schema version should preserve declared-property `Default` as the
declared fallback and add bounded per-class default-override metadata. A
reset-aware client resolves the effective default using the class chain and
override records; it never constructs instances or guesses from current values.

A schema-discovery version bump is preferred because this changes the meaning
available to clients materially. The top-level EditorHost protocol and
`SetPropertyBatch` version do not need to change.

The cache identity for one resolved default is conceptually:

```
EditorHost connection/session
RegistryGeneration
ConcreteClassSchemaId
ConcreteDefinitionVersion
DeclaringClassSchemaId
DeclaringDefinitionVersion
Property identity
```

Project replacement, reconnect, registry replacement, or incompatible refreshed
metadata retires the resolved default.

The representation is bounded. Native override count is compile-time finite.
Future custom-class override registration must count against the existing
bounded custom-schema definition/property/payload envelope or another explicit
bounded extension; it may not become an unbounded map.

## ResetPropertyToDefault

Current `Instance::ResetPropertyToDefault` resolves `FindProperty` and writes
`property->Unmodified`.

That behavior is correct only when the concrete class has no effective override.
Under this decision it is **underspecified/incorrect for overridden inherited
defaults** and must eventually resolve the effective default from the instance's
actual runtime schema class before performing the ordinary property mutation.

Do not change it in this architecture task.

For `TextLabel.BackgroundTransparency` after implementation:

| Step | Value |
| --- | ---: |
| fresh `TextLabel` | `1.0` |
| set to `0.5` | `0.5` |
| Reset | `1.0` |
| Undo | `0.5` |
| Redo | `1.0` |

For `TextBox.BackgroundTransparency`, fresh/reset is `0.0`, because the
TextBox override is nearer than TextLabel's `1.0`.

Studio's schema view must therefore resolve `1.0` for a TextLabel target and
`0.0` for a TextBox target without changing the declaring identity
`Engine.GuiObject.BackgroundTransparency`.

## Prepared-property batching

No prepared-property architecture change is required.

Once Studio has an Engine-derived effective default, reset remains an ordinary
prepared write. A multi-object batch may contain different values for different
concrete classes while retaining the same declaring property identity.

All existing rules remain:

- exact scope/revision;
- exact declaring schema/version;
- `AtomicBatchWritable`;
- whole-selection admission;
- no chunking;
- normal validation;
- one history action;
- no-op normalization;
- ordinary journal/replication;
- atomic Undo/Redo.

The source of the submitted value does not bypass preparation.

## Serialization

Current native persistence serializes **all** readable+writable
`Persistence::Saved` properties. It does not omit native values merely because
they equal `Unmodified`.

Deserialization constructs the concrete class and then applies every saved
property present in the document.

Therefore the current constructor/default mismatch does not lose
`TextLabel.BackgroundTransparency` or the other persistent GUI values across
save/reopen: the saved value overwrites the freshly constructed value.

This decision does not require a persistence-format change.

If native default elision is introduced later, the comparison must use the
**effective concrete-class default**, and deserialization must reconstruct that
same effective value before omitted fields are interpreted. Declaring
`Unmodified` alone would be insufficient.

Custom/extension sparse storage remains unchanged: those systems already use
their frozen declaring-schema `DefaultValue` to omit redundant overrides.

## Clone and duplicate

`Instance::Clone` serializes and detached-deserializes saved state, then copies
writable transient state and remaps references. EditorHost duplication uses the
same authoritative clone/transaction machinery.

A source TextLabel with `BackgroundTransparency = 0.4` therefore duplicates as
`0.4` regardless of its construction/effective default.

The new default model does not change clone value preservation.

## History / Undo / Redo

Reset remains ordinary authoritative mutation through the existing prepared
coordinator.

A successful reset records the previous value and the resolved effective default
used by the action. Undo restores the previous value; Redo reapplies the captured
effective default. Redo does not re-resolve a later schema generation.

No default-specific history format is needed.

## Custom classes and extensions

Custom scalar properties already have declarative `DefaultValue`; no change is
required for their declared properties.

Inherited native properties on custom classes must use the same effective-default
resolution model if custom classes are ever allowed to override inherited
defaults. Such overrides must be declarative, bounded, versioned with the custom
class definition, and must not grant behavior/property shadowing.

The current custom-class `Name` binding demonstrates why construction state
must not be inferred as reset metadata. Until that inherited initialization is
represented through the effective-default model, reset support for such a case
must not be synthesized from a constructed object.

Extensions do not participate in class inheritance and keep their existing
declared defaults.

## Candidate disposition

### A — declaring-property default wins

Rejected for authored persistent constructor defaults.

It would make a fresh `TextLabel` transparent but Reset make it opaque, and it
would reset TextButton/TextBox presentation and interaction properties away from
the values intentionally chosen by their concrete classes. That makes
"Default" mean a base implementation fallback rather than the concrete class's
authoritative authored default.

A remains the fallback rule only when no concrete override exists.

### B — concrete-class inherited default override

Required.

It directly models the existing GUI semantics while preserving one property
identity and the existing no-shadowing rule.

### C — constructor/schema drift is invalid

Required in combination with B for persistent authored defaults.

B without C would leave constructor assignment and schema override as duplicate
truth sets. The current declarations cannot express inherited defaults, so C
alone reduces to the narrow B mechanism.

### D — separate reset default

Rejected.

No current production class requires reset to differ from its authoritative
authored concrete default. Transient/contextual constructor state is handled by
reset ineligibility, not by a third reset-default table.

### E — broader model required

Rejected.

A narrow class-owned default-override layer plus migration of persistent
constructor assignments is sufficient. General inherited-property shadowing is
not required.

## Feasibility matrix

| Concrete class | Property | Declared default | Current constructor | Proposed effective default |
| --- | --- | --- | --- | --- |
| TextLabel | BackgroundTransparency | 0 | 1 | 1 |
| ImageLabel | BackgroundTransparency | 0 | 1 | 1 |
| TextBox | BackgroundTransparency | 0 / inherited TextLabel 1 | 0 | 0 |
| TextBox | BackgroundColor3 | white | dark | dark |
| TextBox | TextXAlignment | Center | Left | Left |
| TextBox | Interactable | false | true | true |
| TextButton | BackgroundColor3 | white | button dark | button dark |
| TextButton | InputSink | None | Activate | Activate |
| ScrollingFrame | Interactable | false | true | true |
| ScrollingFrame | InputSink | None | All | All |
| TextBox/TextButton/ScrollingFrame | GuiState | NonInteractable | Idle | **not an authored reset default** |
| Workspace | CurrentCamera | null | Camera instance | **contextual construction state** |

The exact implementation test should generate this matrix mechanically from
schema/default resolution and fresh construction rather than preserving a second
handwritten expected-value table.

## Implementation sequence

1. Extend native class declarations and classgen with a bounded inherited
   default-override concept. Generate schema metadata and automatic raw
   construction initialization from the same declaration.
2. Add canonical runtime-schema effective-default resolution. Validate overrides
   after inheritance flattening without modifying `AllProperties` identity or
   permitting shadowing.
3. Migrate the persistent GUI constructor assignments listed above into
   declarative overrides. Keep transient/runtime initialization such as
   `GuiState` separate.
4. Expose class-owned override metadata through a new schema-discovery version.
   Preserve declared `Default` as the declaring fallback.
5. Update `Instance::ResetPropertyToDefault` to use effective-default
   resolution, still applying an ordinary property mutation.
6. Add fresh-construction/effective-schema equivalence tests across the full
   migrated matrix plus ordinary non-overridden scalar/string/vector/color/
   UDim/UDim2/CFrame/native-enum representatives.
7. Only after those tests pass, resume the existing reset-through-
   `SetPropertyBatch` qualification and then Studio reset controls.

No Foundation 3L, Studio, prepared coordinator, journal, history, or persistence
format change belongs in this architecture task.

## Gate

Implementation status: the native declaration, generated construction, sparse
runtime resolver and schema-discovery v7 foundation are implemented and
qualified on `feature/concrete-property-defaults`; evidence is recorded in the
[foundation receipt](../Validation/ConcreteClassPropertyDefaults.md). The
[implemented contract](../CurrentArchitecture/ConcreteClassPropertyDefaults.md)
defines the exact literal syntax and bounds. Production reset integration and
reset-through-batch qualification remain separate subsequent work.

**Concrete-default foundation implementation and qualification: COMPLETE.**

The constructor/schema prerequisite gate is satisfied. Production reset
behavior is not qualified by this foundation slice.

The exact next Engine task is:

> Integrate the canonical effective-default resolver into ordinary
> `Instance::ResetPropertyToDefault` while preserving the existing mutation
> path, then resume effective-default reset-through-SetPropertyBatch
> qualification. Studio schema-v7 consumption and reset controls remain a
> subsequent task after that Engine gate succeeds.
