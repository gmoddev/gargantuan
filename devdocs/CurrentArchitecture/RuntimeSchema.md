# Runtime schema

Gargantuan has one canonical schema owned by `RuntimeSchemaRegistry`. Native
and project classes, project enums, and project class extensions are tagged definition
kinds in that registry. Class-reflection APIs are compatibility views over the
same frozen definitions; there is no custom-enum or extension side registry.

## Identity and definitions

`SchemaId` is a deterministic 128-bit identity with a 32-character lowercase
hexadecimal wire form. Zero is invalid. Native classes, custom classes, enums,
and extensions use the independently domain-separated `FromNativeName`,
`FromCustomClassName`, `FromEnumName`, and `FromExtensionName` derivations.
Identity never includes an address,
registration order, randomness, target memory representation, definition
version, or registry generation. The registry also rejects duplicate IDs and
canonical names, so a calculated collision fails instead of becoming
load-order-dependent.

The canonical definition model supports:

- `SchemaClassDefinition`: native or data-only custom construction, stable base
  and native-host identity, reflected/declarative properties, editor metadata,
  subclass policy, and flattened class views;
- `SchemaEnumDefinition`: identity/version/provenance and lexically ordered
  unique name/signed-32-bit-value items; and
- `SchemaExtensionDefinition`: its own identity/version/provenance, a resolved
  target class `SchemaId`, and lexically ordered declarative properties.

An extension does not copy, replace, or change its target class identity. It
applies to the target and derived classes through registry inheritance lookup;
members are not copied into derived class definitions. Property canonical
identity is `ExtensionCanonicalName.PropertyName`. The initial closed type
domain is Boolean, signed 32-bit Integer, finite Number, and bounded UTF-8
String. Defaults are validated during registration and frozen as immutable
metadata. Enum-valued and object-reference extension properties are deferred.

Native property, signal, and method names on the target's flattened surface are
protected. Extensions expose no callbacks, functions, lifecycle hooks, native
pointers, getters/setters, constructors, or inheritance changes.

A project custom class is not a parallel definition kind or registry. It uses
the canonical class definition with `Game` provenance and `CustomData`
construction. Its public schema identity is distinct from the native C++ host.
The initial engine-owned policy permits only `Engine.Folder` as a native
data-only host. Custom-on-custom inheritance is resolved across the complete
candidate, including child-before-parent order, while cycles, depth overflow,
forbidden bases, and inherited native/custom member collisions fail validation.
Project code supplies no constructor, native method, callback, lifecycle hook,
or component composition.

## Candidate lifecycle and PreRun

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
bootstrap authority advances the phases. A project-local canonicalized
`.gargantuan/prerun.luau` executes before world construction in the `PreRun`
domain. Filename, hierarchy, namespace, provenance, target class, and domain do
not grant authority: each registration callback independently requires the
native `DefineSchema` capability and the PreRun registration phase.

PreRun exposes sandboxed base, math, string, table, UTF-8, and a readonly
`Schema` facade with only `RegisterEnum`, `RegisterExtension`, and
`RegisterClass`. It has no
DataModel, filesystem, process, OS, debug, require, network, Studio, renderer,
native pointer, or registry access. Extension registration uses this shape:

```lua
Schema:RegisterExtension({
    Namespace = "Game.Combat",
    Name = "CombatProperties",
    Version = 1,
    Target = "Engine.BasePart",
    Properties = {
        Damage = { Type = "Integer", Default = 0 },
        Team = { Type = "String", Default = "" },
    },
})

Schema:RegisterClass({
    Namespace = "Game",
    Name = "CombatFolder",
    Version = 1,
    Base = "Engine.Folder",
    Properties = {
        Health = { Type = "Integer", Default = 100 },
    },
})
```

Current bounds are 256 KiB source, 250 ms execution, 16 MiB VM allocation, 64
enums, 256 items per enum, 64 extensions, 64 custom classes, 64 properties per
extension or custom class, 16 inheritance levels, 4 KiB per encoded default,
100 UTF-8 bytes per identity/item/property name,
and 64 KiB shared custom-schema payload. The canonical registry independently
enforces definition, property, default, and aggregate limits rather than
trusting the Luau facade.

Registration resolves the target canonical name to a class `SchemaId` while
the candidate is building. Missing, enum, and extension targets fail. A
malformed field, collision, limit failure, runtime failure, or validation
failure aborts the complete candidate. The previous active registry and
generation remain unchanged, and failed initial bootstrap constructs no world.
Validation and freeze precede one atomic publication; active definitions are
const and registration after freeze fails even with retained capability.

Custom class base names resolve to stable class IDs during whole-candidate
validation rather than registration order. Native host policy, construction
availability, inheritance, declarative defaults, and the protected member
surface are revalidated before freeze.

Each successful publication receives a nonzero session-local registry
generation. It selects a complete cache set, is not semantic definition
compatibility, and is not part of `SchemaId`. Failed candidates do not advance
it.

## Runtime extension state

Extension values are sparse per-Instance overrides keyed by extension
`SchemaId` and property name. Missing storage reads the frozen default. A write
resolves extension identity/version, target applicability, property identity,
and exact type against the active frozen registry. Setting the default removes
the physical override. Per-Instance state is bounded to 128 overrides and a 32
KiB aggregate encoded payload.

Luau uses `GetExtensionProperty(extension, property)` and
`SetExtensionProperty(extension, property, value)`. Reads require
`ReadDataModel`; writes require `MutateDataModel` and route through
`MutationGateway`. `DefineSchema` grants neither permission. Destroyed
Instances release extension state normally. Attributes remain dynamic
untyped-name state, Tags remain dynamic indexed state, and neither is used as
extension storage.

Project persistence version 4 stores extension `SchemaId`, exact definition
version, property name, and `WireValue`. Snapshot and journal version 6 carry
sparse initial state and dedicated `ExtensionPropertyUpdate` records. Load and
replication require a present Extension definition, exact version, applicable
target, known property, and exact value type; malformed state does not
partially apply. Migrations are deferred.

## Runtime custom class state

Construction resolves a canonical name to a frozen class definition. Native
classes call their native constructor; a custom class constructs its approved
native host and binds the actual runtime class `SchemaId` before publication.
`ClassName` reports the custom canonical name and `IsA` follows stable class IDs
through custom and native ancestors. The host implementation name is never a
semantic fallback.

Custom class values are sparse per-Instance overrides keyed by declaring custom
class `SchemaId` and property name. Missing storage reads the immutable default;
writing that default removes storage. Values are bounded to 128 overrides and
32 KiB per Instance and remain separate from Attributes and extension state.
Ordinary reflected mutation resolves owner/version/type/applicability and still
requires `MutateDataModel`; `DefineSchema` grants no runtime construction or
write authority.

Persistence version 4 and snapshot/journal version 6 carry exact runtime class
identity/version plus declaring class identity/version for sparse values. Load,
replication, EditorHost, and Studio fail closed on missing/wrong-kind classes,
version mismatch, forbidden construction, target mismatch, unknown property,
or wrong scalar type. There is no migration or base-class fallback.

## Wire and EditorHost

Schema discovery version 4 retains the `Classes` compatibility DTO and emits
deterministic immutable definitions plus registry generation. Extension DTOs
carry target class ID, ordered property identities, types, defaults, and the
standard read/write/editability metadata. Studio caches immutable definitions
by `SchemaId + DefinitionVersion`, selects the active set by registry
generation, and validates an entire replacement before swapping it. Studio
receives no `DefineSchema`, candidate handles, callbacks, or mutable native
metadata.

Class DTOs additionally carry stable base and native-host IDs, construction and
subclass policy, constructibility, and ordered custom properties. Studio keeps
schema meaning in the immutable cache while per-object custom identity and
sparse values live transactionally in `StudioDocument`.

The existing native `EnumItem` system remains a compatibility representation.
Project custom enum values use `{EnumSchemaId, DefinitionVersion, ItemValue}`;
extension scalar values never become unqualified native enum values.

## Deferred

Additional custom native hosts, custom behavior callbacks/hooks, enum-valued
and object-reference schema properties, Core/package/plugin registration,
native-enum migration, generated Luau enum types, migrations, per-definition
ACLs, and component composition are not implemented.
