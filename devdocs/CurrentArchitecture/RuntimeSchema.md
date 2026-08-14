# Runtime schema

Gargantuan has one canonical native class schema owned by
`RuntimeSchemaRegistry`. Generated class definitions are registered into this
registry, and existing reflection APIs are compatibility views over the same
registered `SchemaDefinition` objects.

## Implemented now

### Stable identity

Every registered native class has a 128-bit `SchemaId`. The all-zero value is
invalid. `SchemaId::FromNativeName` deterministically derives native IDs from a
domain-separated engine algorithm plus the qualified namespace and class name;
it does not use an address, hierarchy position, startup randomness, or
registration order. The generated definition stores that ID directly.

The wire-neutral text form is 32 lowercase hexadecimal characters. Parsing
rejects malformed and zero IDs. A registry also rejects duplicate IDs, so a
hash collision is a bootstrap error rather than a load-order decision.

Native classes currently use the `Engine` namespace, definition version `1`,
and `NativeEngine` provenance. Provenance can represent native engine, Core
Luau, game, package, plugin, and tooling origins, but only native registration
is implemented in this pass. Provenance and namespace are descriptive identity
metadata; neither grants authority.

### Canonical definition

`SchemaDefinition` currently owns:

- `SchemaId`, namespace, class name, canonical name, and definition version;
- provenance and optional origin detail;
- base name and stable base `SchemaId`;
- native construction and description metadata;
- declared properties/signals and methods;
- class editor visibility;
- flattened inherited property and method compatibility views.

Properties retain their native read/write paths and reflected value type. The
same property object carries persistence, replication, editability,
main-domain authority, readable/writable domains, required capabilities,
validation, signal kind, and declaring `SchemaId`. Methods carry their
declaring `SchemaId` and enforceable invocation domain/capability metadata;
current generated methods default to the existing unrestricted method policy.
Signals remain reflected properties with signal metadata, matching current
runtime behavior.

`PersistencePolicy` is the only Instance-property persistence selector. The
former parallel `Serializable` boolean was removed. Project serialization reads
the persistence policy, while snapshots and replication read the replication
policy from the same registered property objects.

### Registry ownership and validation

`RuntimeSchemaRegistry` owns registered definitions and supports lookup by
`SchemaId`, native C++ type, qualified canonical name, and the unqualified-name
compatibility form. Enumeration is sorted by canonical name.

Registration and validation reject:

- invalid IDs, empty names/namespaces, zero versions, and non-native provenance
  passed through native registration;
- duplicate native types, `SchemaId` values, and canonical names;
- missing bases, mismatched base names/IDs, and inheritance cycles;
- properties with invalid owner/name/type/access metadata;
- replicated properties without both native read and write paths;
- editor-editable properties without an effective write path;
- methods without native calls or with invalid declaring owners; and
- property/method collisions within one definition.

Inheritance flattening is computed only after the complete candidate set
validates. Failed validation does not publish partially flattened compatibility
views. Derived members preserve the current native reflection override
semantics, and inherited members retain the declaring class ID.

### Reflection compatibility

`InstanceClassDefinition` is now an alias for `SchemaDefinition`.
`InstanceClassRegistry` delegates registration, type/name lookup, deterministic
class enumeration, and cache invalidation to `RuntimeSchemaRegistry`. Existing
`FindProperty`, `FindMethod`, `IsA`, serialization, snapshot, replication,
mutation, and EditorHost schema DTO paths therefore consume the canonical
registered definitions without a second class registry.

The existing EditorHost protocol is unchanged. Its current schema response is a
compatibility DTO backed by this registry; stable schema discovery over IPC is
deferred.

Security remains enforced at native boundaries. Reading schema metadata does
not grant a capability. Reflected property reads/writes, method invocation, and
`MutationGateway` continue to check their current domain/capability contracts.

## Deferred future architecture

This pass does **not** implement registry lifecycle states or freezing. The
registry may still accept native definitions during initialization, and it
validates before an observable lookup/enumeration.

Also deferred:

- transactional registration/freeze and registry generations;
- PreRun, Core Luau, game, package, plugin, or tooling loaders;
- custom classes, enums, extensions, components, attributes, or tags;
- migrations and saved/wire class IDs;
- EditorHost schema discovery and Studio schema-driven UI; and
- generated Luau types.

The next bounded architecture task is registration lifecycle plus a
transactional registry freeze. It should publish one fully validated registry
or none without replacing this schema representation.
