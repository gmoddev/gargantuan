# Attributes + Tags architecture evaluation

Status: **Approved with bounded follow-up** (2026-08-14).

This checkpoint reviewed the Attributes and Tags vertical slice from native API
entry points through authority, persistence, snapshots, journals, loopback
replication, EditorHost, the C# `StudioDocument`, and restricted Studio Luau. It
did not implement or prototype custom enums, PreRun, extensions, custom classes,
tag namespaces, or a generalized query system.

## Decision

Attributes and Tags validate the intended separation:

- the frozen runtime schema describes the fixed native APIs, while attribute
  keys, values, tag names, and memberships remain bounded dynamic state;
- writes converge on Main-domain native validation and committed journal state;
- project files persist one canonical object-side representation;
- snapshots and dedicated journal operations reconstruct the same state;
- replication receivers and Studio are isolated consumers, not authority; and
- identity-sensitive tag indexes use generation-checked `ObjectId`, never
  Instance pointers.

Gargantuan may proceed to a bounded **Custom Enum through PreRun** milestone.
That work must preserve this separation and must not treat dynamic attribute or
tag state as schema registration input.

The non-blocking follow-up is to replace Studio's whole-document clone per
non-empty journal batch before continuous high-frequency polling or
collaboration at large document sizes. Total project/snapshot envelope limits
must also be defined before those formats are accepted from an untrusted remote
transport.

## API and source-of-truth review

The apparent API difference is intentional. Attributes are per-Instance scalar
state, so `Instance:SetAttribute`, `GetAttribute`, `GetAttributes`, and the
per-name signal are coherent. Tags require world-scoped reverse lookup, so a
`Tags` service owns `Add`, `Remove`, `Has`, `GetTags`, `GetTagged`, and the
bounded explicit AND operation `GetTaggedAll`. Both use case-sensitive names,
successful no-op add/remove semantics, `ReadDataModel` for reads, and
`MutateDataModel` for writes.

Canonical engine state is:

```text
Attributes: Instance -> ordered name -> WireValue

Tags: DataModel
      |- ObjectId -> TagId
      `- TagId -> ObjectId
```

Only per-object attribute values and tag names are persisted. Session-local
`TagId` and the reverse index are rebuilt. Studio stores cloned object state and
derives its reverse tag lookup only after a complete candidate journal batch
validates. The retained Python client is a protocol/test harness; it is not the
canonical Studio document implementation.

## Review-driven fixes

The checkpoint found and fixed these concrete defects:

1. Scope-bound tag commands now carry an expected `DataModel` scope. A `Tags`
   service and EditorHost cannot use a valid foreign `ObjectId` to mutate
   another world.
2. Moving a subtree between DataModels clears tag membership for the complete
   subtree. Descendant membership can no longer remain stranded and reappear if
   the subtree returns to the old world.
3. Lifecycle-only bulk tag cleanup is private to `Instance`; ordinary native
   callers cannot invoke the capability-free cleanup path.
4. Attribute storage and per-name signal maps are private. Signal creation is
   capped at 64 names per Instance and allocation failure cannot leave a null
   signal entry.
5. `GetTaggedAll` accepts at most 64 input names, checked before the Luau wrapper
   reserves its vector and again at the native index boundary.
6. Attribute and tag state changes roll back if journal publication fails.
   Multi-tag lifecycle removal prebuilds and atomically splices a complete
   journal batch. Sequence numbers advance only after record insertion succeeds.
7. Replication rejects semantic no-op attribute records, duplicate `TagAdded`,
   and absent `TagRemoved` without advancing its cursor.
8. Studio enforces the engine's 1,024-distinct-tag limit and rejects semantic
   no-op attribute records transactionally.
9. Studio measures attribute JSON with unescaped UTF-8, matching the engine's
   canonical `WireValue` byte accounting instead of over-counting Unicode escape
   sequences.
10. Studio validates a 256-record candidate hierarchy once before publication,
    while retaining per-reparent cycle checks. This removed repeated full-world
    validation without weakening transactional application.

Rejected operations preserve the prior attribute map, tag indexes, Studio
cache, cursor, notifications, and schema generation. Successful identical
assignments, duplicate adds, missing removals, and absent tag removals remain
journal-free at the authoritative command surface.

## Persistence and protocol compatibility

Project JSON compatibility is explicit:

| Version | Attributes | Tags |
| --- | --- | --- |
| 0 | absent, defaults empty | absent, defaults empty |
| 1 | required and validated | absent, defaults empty |
| 2 | required and validated | required and validated |

Unknown versions fail closed. Tests now cover representative version 0 and 1
loads in addition to current version 2, malformed attribute values, and duplicate
tag membership. This version model is adequate for the current native-only
state, but custom definitions will require an explicit migration contract rather
than an indefinite series of ad hoc top-level version checks.

Snapshot and wire-journal version 4 remain coherent. Attributes use dedicated
`AttributeUpdate`; tags use `TagAdded` and `TagRemoved`. These semantics remain
separate from schema-defined `PropertyUpdate`. Stable `ObjectId`, bounded
`WireValue`, monotonic sequences, scope checks, no-op suppression, and receiver
journal suppression remain intact. No wire version changed during this review.

## Representative Debug profile

Measurements were taken from the retained profile harnesses on the current
Windows/MSVC Debug build. They are directional architecture measurements, not
release performance promises.

### Engine profile

| Scenario | Result |
| --- | ---: |
| Construct 10,000 Instances | 898 ms |
| Set 4 attributes on 1,000 Instances | 650 ms |
| Set 1,000 sparse attributes across 10,000 Instances | 113 ms |
| Add 14,000 tag memberships | 399 ms |
| `GetTagged("Enemy")`, 100 queries over 8,000 matches | 1,157 ms total |
| `Enemy + Alive`, 100 indexed intersections | 2,717 ms total |
| `Enemy + Alive + Visible`, 100 smallest-set intersections | 746 ms total |
| 10,000 add/remove tag churn pairs | 860 ms |
| Capture 10,001-object snapshot | 1,015 ms |
| Serialize snapshot | 2,854 ms |
| Parse snapshot | 3,762 ms |
| Load snapshot | 2,224 ms |
| Serialized snapshot | 1,858,120 bytes |
| 999 non-no-op attribute journal records | 193,617 bytes |
| Destroy 1,000 tagged descendants | 123 ms |

The three-tag query is faster than the two-tag query because it starts from the
1,000-object `Visible` set, confirming smallest-candidate intersection. Query
time includes generation/scope validation through `ObjectRegistry`; those live
lookups dominate dense Debug queries. There is no evidence yet that a bitset or
generalized query optimizer is warranted.

### Studio profile

| Scenario | Time | Managed allocation |
| --- | ---: | ---: |
| Load 10,001-object snapshot | 253 ms | 14.2 MB |
| `Enemy`, 100 queries | 2 ms | 6.4 MB (returned arrays) |
| `Enemy + Alive`, 100 intersections | 344 ms | 6.6 MB |
| `Enemy + Alive + Visible`, 100 intersections | 116 ms | 1.6 MB |
| Apply one mixed 256-record batch | 64 ms | 14.0 MB |
| Apply 20 separate one-record batches | 2,168 ms | 267.3 MB |

Before the review-driven validation fix, the 256-record batch took 3,452 ms and
allocated 837.4 MB. The remaining single-record result demonstrates the known
whole-cache-clone cost. It is acceptable for the present explicit editor proof,
but not for a future continuous high-frequency stream.

Native allocation counts were not instrumented. The current memory shape is
bounded but node-heavy: each stored attribute uses one ordered-map node plus a
`WireValue`; attribute signals allocate only when requested and are capped at
64. Each tag membership uses one forward and one reverse ordered-set node plus
a compact `TagId`; interned strings exist once per active tag. The 14,000-member
profile therefore uses 28,000 membership nodes, which is reasonable at the
current scale but should be remeasured before substantially raising limits.

## Security and hostile input

The focused review traced EditorHost commands, persistence decode, snapshot and
journal decode, replication apply, native services, and Studio application.
Names reject invalid UTF-8 and embedded nulls. Finite numeric checks, per-value
and aggregate attribute bytes, per-object counts, distinct tag count, query
count, stale identity, expected world scope, and explicit capabilities are
enforced at native or transactional decode boundaries rather than only UI
wrappers. EditorHost retains its request/response bounds and session token.

The Codex Security workbench could not initialize this scoped scan because it
failed while inspecting the existing `vendor/argparse` submodule. No vendor
state was changed. A focused source-backed assessment was completed manually,
so there is no generated Codex Security report for this checkpoint.

Two general hostile-input limits remain outside this slice: project JSON is read
into memory before per-attribute/tag validation, and standalone snapshot/journal
parsers do not impose a total envelope/object-count limit. EditorHost bounds its
transport and loopback replication uses trusted in-process data, so these do not
block custom enum registration. They must be resolved before remote or otherwise
untrusted state transfer.

## Thread and lifecycle assumptions

Authoritative mutation and tag cleanup are Main-domain operations. Current live
attribute/tag reads also occur on the main engine/script path; the containers do
not promise concurrent mutation/read safety. Workers must consume copied state
or commands, not live maps or indexes. Query-time generation validation is
defense in depth; destruction and scope movement perform primary index cleanup
before identity reuse.

`StudioDocument` is likewise serialized by the current Studio workflow. It is
not internally locked. A future background poller must marshal complete batches
onto one document-application domain rather than concurrently reading and
mutating the cache.

## Architecture foundation regressions

Focused and existing tests confirm that attribute/tag changes do not reopen the
schema, create dynamic `SchemaId`, or increment registry generation. Capability
checks remain independent of execution-domain names. Object generation reuse,
journal order, replication suppression, EditorHost isolation, Studio selection
cleanup, and render extraction regressions remain covered.

## Accepted limitations and bounded follow-up

- Studio clones the complete cache for every non-empty batch. Replace this with
  transactional structural sharing or a bounded undo log before high-frequency
  large-document streaming; do not weaken atomicity to optimize it.
- Dense native tag queries revalidate every result against `ObjectRegistry`.
  Profile Release builds and real editor workloads before changing containers.
- The Python harness is intentionally non-canonical and does not implement the
  full transactional C# document contract.
- Total persistence and standalone wire envelope limits remain a prerequisite
  for untrusted transport.
- No tag events, namespaces, metadata, OR/NOT language, or schema-backed
  attribute definitions were added.

These limitations do not invalidate the dynamic-state/schema boundary, so the
next architecture milestone is authorized as **Custom Enum through PreRun**.
