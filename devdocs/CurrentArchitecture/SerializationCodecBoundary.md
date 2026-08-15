---
status: current
owner: runtime
last_verified: 2026-08-15
related_code:
  - include/gargantuan/serialization/SerializationError.hpp
  - include/gargantuan/runtime/WireCodec.hpp
  - include/gargantuan/runtime/Snapshot.hpp
  - include/gargantuan/runtime/WireJournal.hpp
  - include/gargantuan/assets/InstanceSerialization.hpp
  - include/gargantuan/editor/EditorHost.hpp
  - src/serialization/JsonCodec.hpp
  - src/serialization/JsonCodec.cpp
  - tests/fixtures/serialization/
  - tests/SerializationBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/future-architecture.mdx
---

# Serialization codec boundary

## Decision and implemented layering

The codec boundary is feasible and is implemented before basic client
replication, remotes, bootstrap/session protocols, and additional cross-language
surfaces expand. Gargantuan owns semantic types, format versions, validation,
limits, and compatibility. A serialization library is an implementation detail.

```text
Snapshot / WireJournal / EditorHost / Project / WireValue
    -> Gargantuan format contracts and engine validation
    -> private JSON codec
    -> nlohmann JSON
```

This is an extraction, not a serializer migration. nlohmann remains the current
JSON implementation. Glaze is neither adopted nor exposed through an engine
contract. A future JSON backend can be evaluated without changing Snapshot,
journal, EditorHost, project, replication, or Luau semantic types. A future
binary game codec can encode typed engine values directly and does not pass
through a JSON DOM.

## Previous coupling and inventory

The extraction classified the repository's nlohmann usage as follows:

| Area | Previous coupling | Current disposition |
| --- | --- | --- |
| `WireValue` | Public `WireJson = nlohmann::ordered_json`; public encode/decode accepted and returned the DOM | Public format-specific text/result entry points; DOM conversion is private |
| Protocol input | Public header included nlohmann and declared JSON-tree validation | Byte/depth policy remains public engine policy; tree validation moved into the private codec |
| Project/Instance persistence | Public `InstanceSerialization::json` alias; `Project` constructed a DOM | Callers select `InstanceFormat` and exchange semantic Instances/text; empty-project construction is typed |
| Snapshot | Semantic header was clean, but implementation built and parsed nlohmann trees directly | Semantic header remains clean; private codec owns parsing, DOM conversion, and exception normalization |
| Wire journal | Semantic header was clean, but implementation treated the JSON DOM as the record intermediary | `WireJournalRecord` remains the semantic record; JSON is private representation |
| EditorHost | Public transport was already text, while dispatch and generated payloads were nlohmann-based | Public boundary remains UTF-8 request/response text; all JSON types stay in the implementation |
| Runtime schema | Schema export was implementation-local JSON; value-size policy called the public DOM encoder | Export remains a private EditorHost JSON codec concern; size policy uses a Gargantuan byte-measure API |
| Logging/debugging | Structured log output uses nlohmann privately | Retained: it has no semantic or protocol-facing type leakage |
| Tests/fixtures | Tests use nlohmann for independent JSON mutation and semantic comparison | Retained intentionally as test-only implementation use |
| Temporary/internal code | Serializer-specific parsing, writing, and exceptions were distributed across format implementations | Centralized under `src/serialization/` or confined to the owning private codec translation unit |

No header under `include/gargantuan/` includes nlohmann or Glaze. The core target
links nlohmann privately and no longer publishes Glaze or its include directory
as a core usage requirement.

## Semantic and codec contracts

The existing Gargantuan-owned semantic models remain authoritative:

- `WireValue`, `WireObjectId`, and schema identities describe dynamic engine
  values and references.
- `Snapshot`, `SnapshotObject`, extension state, and custom-class state describe
  captured world state.
- `WireJournalRecord` and `WireJournalOperation` describe committed journal
  operations. `ChangeJournal.Sequence` remains scoped authoritative history, not
  transport framing or acknowledgement metadata.
- Instances, the runtime schema registry, mutation commands, and EditorHost
  engine operations remain typed engine state. JSON objects are not semantic
  substitutes for them.

The public standalone JSON helpers are deliberately narrow:

```cpp
SerializationResult<std::string> EncodeWireObjectIdJson(WireObjectId);
SerializationResult<WireObjectId> DecodeWireObjectIdJson(std::string_view);
SerializationResult<std::string> EncodeWireValueJson(const WireValue &);
SerializationResult<WireValue> DecodeWireValueJson(std::string_view);
```

Higher-level formats retain their format-specific entry points:

- `SerializeSnapshot` / `DeserializeSnapshot`;
- `SerializeWireJournalRecords` / `DeserializeWireJournalRecords`;
- `InstanceSerialization::Serialize` / `Deserialize` and
  `SerializeEmptyProject` with explicit `InstanceFormat` selection; and
- `EditorHost::HandleRequest`, whose transport contract is bounded UTF-8 JSON
  text rather than a JSON-library type.

The private `JsonCodec` owns JSON parsing, writing, object-ID and `WireValue`
DOM conversion, parsed-tree limits, and nlohmann exception confinement. There is
no universal Gargantuan JSON value type.

## Normalized errors and validation ownership

`SerializationErrorCode`, `SerializationError`, and `SerializationResult<T>` are
Gargantuan-owned. Errors distinguish invalid syntax, unsupported versions,
invalid types, missing/unknown fields, limit violations, invalid values,
truncation, and internal failures. A path and implementation diagnostic may be
attached without changing the public error type. nlohmann exceptions never
cross the codec boundary.

Parsing and engine validity remain separate:

```text
bounded UTF-8 text
    -> JSON syntax and structural tree parsing
    -> typed Gargantuan representation
    -> schema, identity, scope, hierarchy, value, and policy validation
    -> usable state or atomic rejection
```

The existing protocol-input ceilings remain engine-owned, including document
bytes, nesting, node and collection counts, string lengths, integer narrowing,
numeric finiteness, valid UTF-8, valid `ObjectId` and `SchemaId` values,
definition versions, reference scope, Attributes, Tags, extensions, custom
enums, custom classes, and hierarchy validity. Codec success never grants
authority and never bypasses semantic preflight.

## Current format contracts

All formats use UTF-8 JSON objects and preserve their existing PascalCase field
names and enum strings. Numbers must satisfy their declared engine type and
range; values that enter `WireValue` must also satisfy its finite-number and
shape rules. Required fields may not be replaced by `null`; nullable parent and
value fields accept `null` only where their operation explicitly permits it.

### Project/Instance JSON version 4

The root and every child encode `Name`, `ClassName`, `ClassSchemaId`,
`ClassDefinitionVersion`, `Properties`, `Attributes`, `Extensions`,
`CustomProperties`, `Tags`, and `Children`; the document root also carries
`Version`. Schema-owned extension/custom state includes its stable schema ID and
definition version. The loader accepts the repository's legacy versions 0
through 4 and applies the existing version-specific field rules. Unknown object
fields remain ignored for compatibility. `InstanceFormat::Binary` remains an
explicit but unimplemented choice.

Ordering is a deterministic human-output preference: the encoder retains its
stable field and collection order for useful project diffs, while object-key
order is not semantic during decode. Children, tags, extensions, and
custom-property groups preserve the current deterministic traversal/order
rules.

### Snapshot JSON version 6

The envelope requires only `Version`, `Cursor`, and `Objects`. A cursor contains
`Scope` and `NextSequence`. Each object requires only `Id`, `ClassSchemaId`,
`ClassDefinitionVersion`, `ClassName`, `Name`, `Parent`, `Properties`,
`Attributes`, `Extensions`, `CustomProperties`, and `Tags`. Nested object IDs
contain only `Slot` and `Generation`. Unknown fields are rejected at the
envelope, cursor, object, state, ID, and `WireValue` layers. Schema identity,
definition versions, references, class construction, and hierarchy are
validated after structural decode.

Encoder ordering is compatibility-locked and deterministic by the golden
fixtures, but consumers must not assign semantic meaning to JSON object-key
order. Array ordering remains semantic where it represents snapshot object or
tag/state order.

### Wire journal JSON version 6

The envelope requires only `Version` and `Records`. Every record requires
`Version`, `Sequence`, `Scope`, `Operation`, and `ObjectId`, plus the exact fields
for `Create`, `PropertyUpdate`, `AttributeUpdate`,
`ExtensionPropertyUpdate`, `TagAdded`, `TagRemoved`, `Reparent`, or `Destroy`.
Unknown fields and incomplete operation-specific identities are rejected.
`null` is used for attribute removal and an unparented `Reparent`; it does not
make unrelated required fields optional. Journal order and sequence are
semantic. JSON object-key order is not.

### EditorHost JSON version 1

A request envelope requires only `Version`, string `RequestId`, string
`SessionToken`, string `Method`, and object `Params`. Responses require
`Version`, `RequestId`, `Ok`, and exactly one result/error payload according to
the existing method behavior. The line transport prefix remains
`GARGANTUAN_EDITOR/1 `. Protocol version, capabilities, launch-token comparison,
request/response bounds, method-specific strict parameter checks, error codes,
Snapshot v6, journal v6, and schema identity/version semantics remain unchanged.
Unknown envelope fields and unknown method parameter fields are rejected.

Schema discovery is an EditorHost result, not a second serializer-owned schema.
It projects the frozen runtime registry's stable IDs, definition versions,
provenance, custom enums, extensions, and custom classes. The registry continues
to own generation and validation semantics.

## Duplicate fields and canonical ordering

nlohmann's existing parser keeps the last value for a duplicate object key.
Foundation 1 deliberately preserves and tests this behavior so the extraction
does not become an unversioned protocol-hardening change. It is not endorsed as
a future canonical rule: changing a hostile protocol to reject duplicate keys
requires an explicit compatibility/security decision and tests before a backend
switch. A future codec must match the retained behavior unless that separate
hardening decision changes the contract.

All current encoders emit compact JSON with stable insertion order. Golden byte
comparison locks externally useful deterministic output. Decoders remain
insensitive to object-key ordering; only arrays and explicitly ordered semantic
collections carry order.

## Golden compatibility fixtures

The fixtures under `tests/fixtures/serialization/` are small, current-version
compatibility locks:

- `project_v4_minimal.json` and `project_v4_complex.json` cover basic Instances,
  Attributes, Tags, a class extension, custom-class sparse state, stable schema
  identities, and deterministic child output. The complex re-encode reapplies
  project v4's existing non-persisted root `Archivable` emission precondition;
  the extraction does not change that legacy runtime behavior;
- `snapshot_v6_minimal.json` and `snapshot_v6_complex.json` cover object IDs,
  parent references, cursor state, Attributes, Tags, extensions, and custom
  classes;
- `journal_v6_representative.json` covers native/custom property updates, an
  extension update, a schema-enum `WireValue`, sequence/scope, and a tag; and
- `editorhost_v1_request.json` and `editorhost_v1_response.json` lock the request
  envelope, request ID, authorization failure, and response envelope.

Foundation tests prove fixture decode, semantic state, re-encode, and exact
canonical bytes where the current encoder owns canonical output.

## Benchmark baseline

`gargantuan_serialization_benchmark` is a repeatable nlohmann baseline harness.
The default CTest smoke measures Snapshot 1,000 objects, journal 100 operations,
EditorHost 100 small requests, and project 100 objects. `--full` adds Snapshot
10,000/50,000 and journal 1,000/10,000 workloads. It reports semantic
construction, encode, decode/parse, total time, and output bytes separately where
the current API exposes those phases. EditorHost reports only total request
handling because its public API intentionally combines decode, dispatch, and
encode. Unavailable phases are `N/A`; the harness does not fabricate allocation
or build-cost data.

A representative Windows x64 Release smoke on 2026-08-15 measured:

| Workload | Items | Bytes | Construct ms | Encode ms | Decode ms | Total ms | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Snapshot | 1,000 | 351,667 | 0.690 | 9.956 | 15.223 | 25.870 | Ok |
| Snapshot | 10,000 | 3,546,668 | 5.298 | 100.622 | 157.815 | 263.736 | Ok |
| Snapshot | 50,000 | 17,866,668 | 25.032 | 469.483 | 0.166 | 494.680 | Expected 8 MiB limit |
| Journal | 100 | 18,901 | 0.025 | 0.476 | 0.498 | 0.999 | Ok |
| Journal | 1,000 | 190,714 | 0.127 | 3.964 | 4.829 | 8.921 | Ok |
| Journal | 10,000 | 1,926,867 | 0.673 | 44.312 | 58.943 | 103.928 | Ok |
| EditorHost | 10,000 | 1,190,000 | N/A | N/A | N/A | 37.827 | Ok |
| Project | 1,000 | 213,912 | 2.576 | 7.224 | 11.967 | 21.809 | Ok |

The 50,000-object Snapshot encode is measured, while decode correctly rejects
its 17.9 MB document at Gargantuan's existing 8 MiB hostile-input ceiling. These
are baseline observations, not stable performance requirements and not a Glaze
comparison.

## GNS regression check

The pinned real GameNetworkingSockets target was rebuilt in Debug and Release
and its localhost lifecycle, reliable/unreliable delivery, limit, bounded
stress, listener failure, close/identity reuse, queue exhaustion, failed-connect,
and destruction-cleanup tests pass. The previously reported runtime stall did
not reproduce. Windows test staging now copies the dynamic protobuf runtime in
addition to SDL so CTest reaches the transport smoke instead of failing during
process startup.

## Deferred work

Glaze adoption, a final binary game codec, binary Instance/project persistence,
basic client replication, Luau remotes, the Node protocol choice, and an
EditorHost binary migration remain deferred. The next serialization milestone
may benchmark a Glaze implementation against this boundary; it must not change
the semantic or format contracts merely to make the candidate library easier to
use.
