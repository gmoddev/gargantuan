# Runtime schema

Gargantuan has one canonical schema owned by `RuntimeSchemaRegistry`. Native
classes and project enums are definition kinds in that registry. Existing
class-reflection APIs are class-only compatibility views over the same frozen
definitions; there is no separate custom-enum registry.

## Identity and definitions

`SchemaId` is a deterministic 128-bit identity with a 32-character lowercase
hexadecimal wire form. Zero is invalid. Native classes use
`FromNativeName(namespace, name)` and custom enums use the independently
domain-separated `FromEnumName(namespace, name)`. Neither identity depends on
addresses, registration order, randomness, hierarchy, or registry generation.
The same qualified class and enum names therefore have different candidate IDs,
although the registry separately rejects their canonical-name collision.

The canonical tagged definition model currently supports:

- `SchemaClassDefinition`: native construction, base identity, reflected
  properties/signals/methods, editor metadata, and flattened class views; and
- `SchemaEnumDefinition`: namespace, name, stable ID, nonzero definition
  version, provenance/origin, and ordered name/numeric-value items.

Native classes use namespace `Engine`, version `1`, and `NativeEngine`
provenance. PreRun enums use project-selected namespaces such as `Game`, a
project-declared version, and `Game` provenance. Provenance and namespace are
descriptive and grant no authority.

Enum items are sorted lexically by item name during candidate construction.
Names and numeric values are unique; aliases and flags semantics are not
supported. A runtime enum value is `{EnumSchemaId, DefinitionVersion,
ItemValue}`. Equality and persistence therefore include the owning enum rather
than treating an item as an unqualified integer.

## Candidate lifecycle and PreRun

The explicit lifecycle is:

```text
Bootstrap
  -> NativeRegistration
  -> CoreRegistration
  -> PreRunRegistration
  -> Validation
  -> Frozen
  -> Runtime
```

Generated native seeds are copied into a hidden mutable candidate. The native
bootstrap authority then advances to PreRun. If a project contains
`.gargantuan/prerun.luau`, the native host selects that file and executes it in
a dedicated `PreRun` domain. Filename, hierarchy, namespace, and provenance do
not grant authority. The registration callback separately requires the
`DefineSchema` capability.

PreRun exposes only sandboxed base, math, string, table, UTF-8, and the readonly
`Schema:RegisterEnum` facade. It has no DataModel, filesystem, process, OS,
debug, require, network, Studio, renderer, or registry-pointer access. Current
hard limits are:

- 256 KiB source;
- 250 ms execution time;
- 16 MiB per-VM allocation;
- 64 custom enum definitions per candidate;
- 256 items per enum;
- 100 UTF-8 bytes per namespace, definition name, or item name; and
- 64 KiB aggregate submitted definition payload.

Registration input is fully parsed and bounded before insertion. A malformed
field, collision, budget failure, runtime failure, or invalid definition aborts
the entire candidate. Previously published schema and registry generation stay
unchanged, and world construction does not begin for a failed initial project
bootstrap.

Whole-candidate validation resolves class inheritance and flattened members
before freeze. Publication happens only after validation and freeze. The active
registry is immutable, public lookup returns const definitions, and even a
caller retaining `DefineSchema` cannot register after the lifecycle reaches
Runtime. The candidate is never visible through ordinary reflection.

Each successful complete publication receives a session-local unsigned 64-bit
registry generation. Zero is invalid; failed candidates do not advance it, and
the counter fails closed rather than rolling over. Registry generation selects
an active cache set. It is independent of each definition's semantic
`DefinitionVersion` and is not part of `SchemaId`.

## Validation and compatibility

The registry rejects invalid/duplicate IDs, duplicate canonical names, invalid
UTF-8 or embedded nulls, zero versions, missing/cyclic class bases, invalid
member ownership/access, duplicate enum item names or numeric values, excessive
counts/payloads, and wrong provenance at the native/custom registration
boundaries. Observable enumeration sorts by canonical name and then definition
kind. Typed `FindClass*` and `FindEnum*` APIs fail safely on the wrong kind.

`InstanceClassDefinition` aliases `SchemaClassDefinition`, and
`InstanceClassRegistry` remains a class-only adapter over the active registry.
Native class IDs, `IsA`, property/method lookup, construction, persistence,
snapshots, journals, replication, mutation, rendering, and viewport behavior
remain unchanged.

The existing native `EnumItem` system remains a compatibility representation
for built-in engine enums. Project enums use canonical schema identity and do
not replace that system in this slice. A future bounded pass may adapt native
enums into schema definitions; there are not two custom-enum authorities.

## Wire and EditorHost

`WireValue` now has a closed `SchemaEnum` variant containing the enum
`SchemaId`, exact definition version, and signed 32-bit item value. Encoding is
deterministic. Materialization requires a frozen enum definition of the correct
kind, exact version, and known item; missing definitions, wrong kinds, version
mismatches, malformed IDs, and unknown items fail rather than coercing to a
name or integer. General migrations are deferred. Attributes continue to
reject this new variant, and Tags are unaffected.

`GetSchema` keeps the existing `Classes` compatibility DTO and adds schema
discovery version `2`, the active registry generation, and deterministic
`Definitions`. Enum DTOs contain stable identity, kind, namespace/name,
definition version, provenance, and ordered items. Studio receives immutable
metadata only; it receives neither `DefineSchema` nor the registration facade.

## Deferred

Core Luau registration, native-enum unification, enum globals/generated Luau
types, enum-valued Attributes, migrations, packages, plugins, class extensions,
custom classes, and component composition are not implemented. The next step
is a bounded review of this PreRun/enum slice before class extensions.
