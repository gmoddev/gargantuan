---
status: current
owner: serialization
last_verified: 2026-08-15
related_code:
  - src/serialization/GlazePrototype.cpp
  - tests/GlazeSerializationTests.cpp
  - tests/SerializationBenchmark.cpp
related_adrs: []
---

# Serialization backend evaluation

## Decision

**HYBRID JSON BACKENDS**

Glaze is justified for strongly typed, high-volume JSON codecs such as Snapshot
and WireJournal. nlohmann remains the production JSON backend today and remains
appropriate for lower-volume, highly dynamic, debugging, project, and
EditorHost paths until each path has independent evidence for migration. This
evaluation does not change production codec selection or any format version.

## Evaluated dependency

- Glaze revision: `7521e2ab262acd7a0d625354e26876cd9c35a231`
  (`v8.0.0-2-g7521e2ab`).
- License: MIT, copyright Stephen Berry.
- Acquisition: the repository's existing exact git submodule at `vendor/glaze`;
  no new dependency or generated dependency tree was added.
- Size in this checkout: 665 files, 10,983,487 bytes total; headers account for
  4,459,531 bytes.
- Language/build model: header-only, C++23, linked only into the private
  development prototype target when
  `GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=ON` (default `OFF`).
- Compiler evidence: MSVC 19.50.35728 builds Debug and Release. The existing
  MSVC 19.40 Release configuration does not compile this revision of Glaze
  (errors in Glaze's `zmij.hpp` and compile-time reflection implementation).
  A future production adoption must either establish MSVC 19.50 as the supported
  compiler baseline or select and verify a compatible pinned revision.

## Prototype scope

`gargantuan_glaze_serialization_prototype` is a test/benchmark-only static
target. It implements private typed codecs for:

- Snapshot v6 envelopes, cursors, objects, extension state, custom-class state,
  Attributes, Tags, and property maps;
- WireJournal v6 envelopes and each operation-specific record shape; and
- every current `WireValue` alternative with explicit field, null, number,
  enum, schema ID, and ObjectId rules.

All durable field names are explicit Glaze metadata. The outer Snapshot and
Journal paths serialize directly between semantic state and JSON text without
an intermediate generic DOM. `glz::raw_json` is used only at genuine dynamic
seams: heterogeneous `WireValue` values and journal operation variants.
`WireValue` decode uses a small Glaze generic value locally so its dynamic shape
can be checked explicitly; it is not exposed and is not the primary document
representation. Exact nlohmann-compatible floating-point lexical output needed
explicit private writing logic.

Glaze types, metadata, and errors remain confined to
`GlazePrototype.cpp`. Public semantic headers remain serializer-independent.

## Compatibility

The prototype preserves the existing JSON contract:

- Snapshot v6 minimal and complex fixtures decode and re-encode to identical
  canonical bytes;
- the representative WireJournal v6 fixture, including custom properties,
  extension state, schema enums, and Tags, re-encodes to identical bytes;
- all 15 `WireValue` alternatives round-trip semantically and their combined
  Journal encoding matches nlohmann byte-for-byte;
- field names, field order, nulls, sparse fields, IDs, schema IDs, versions,
  escaping, and floating-point forms remain unchanged; and
- duplicate keys retain the compatibility-locked last-value-wins behavior.

There are no intentional external-format deviations and no format versions
changed. Project, EditorHost, and runtime-schema JSON were not prototyped or
modified.

## Benchmark methodology

The benchmark was run on the same checkout, process, compiler, Release flags,
machine, semantic values, and output formats. Each reported result is the mean
of five measured runs after one warm-up. Construction is outside codec timing.
Both backends perform their normal structural and semantic decode validation.

- Compiler: Microsoft C/C++ 19.50.35728, x64, Release.
- Processor identity: AMD64 Family 25 Model 97 Stepping 2.
- Snapshot workloads: 1,000, 10,000, and 20,000 objects. 20,000 is the highest
  selected representative workload below the existing 8 MiB input ceiling;
  its output is 7,126,668 bytes. The ceiling was not relaxed.
- Journal workloads: 100, 1,000, and 10,000 AttributeUpdate operations, split
  into existing bounded batches where required.

Times are milliseconds:

| Workload | Bytes | nlohmann encode | Glaze encode | nlohmann decode | Glaze decode | nlohmann total | Glaze total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Snapshot 1,000 | 351,667 | 7.613 | 1.727 | 11.567 | 6.549 | 19.181 | 8.277 |
| Snapshot 10,000 | 3,546,668 | 95.564 | 21.021 | 153.476 | 89.720 | 249.040 | 110.741 |
| Snapshot 20,000 | 7,126,668 | 209.923 | 52.648 | 370.936 | 286.321 | 580.858 | 338.969 |
| Journal 100 | 18,901 | 0.641 | 0.041 | 0.766 | 0.594 | 1.408 | 0.635 |
| Journal 1,000 | 190,714 | 5.270 | 0.329 | 5.977 | 3.964 | 11.247 | 4.293 |
| Journal 10,000 | 1,926,867 | 48.521 | 4.237 | 62.952 | 37.647 | 111.473 | 41.885 |

Output byte counts are identical for both backends. Snapshot total codec time
improved 1.71–2.32x; Journal total improved 2.22–2.66x. Glaze's largest gain is
encoding. Decode remains materially faster despite semantic validation and
explicit dynamic-value handling.

EditorHost (10,000 small rejected-handshake requests) measured 35.500 ms total
and Project (1,000 objects) measured 6.941 ms encode / 12.710 ms decode with
nlohmann. They were retained as context only; no Glaze implementation exists for
those paths, so no backend comparison is claimed.

## Build and binary impact

The experiment adds one private 36,936-byte, 465-line implementation translation
unit. A no-change incremental build of the benchmark target took 1.476 seconds.
The Release prototype static archive is 203,475,016 bytes and the dual-backend
benchmark executable is 3,841,024 bytes; MSVC debug information and template
objects make the archive size unsuitable as a runtime-size comparison. A clean
isolated nlohmann-only versus Glaze executable-size or peak-compiler-memory
comparison was not available, so none is fabricated.

Glaze headers are not added to `gargantuan_core`, public headers, or ordinary
production targets. A Release nlohmann-only benchmark build and smoke run remain
available with MSVC 19.40 when the prototype option is off. The failed MSVC
19.40 Glaze build is the largest adoption constraint discovered.

## Maintenance assessment

| Criterion | nlohmann | Glaze prototype |
| --- | --- | --- |
| Snapshot encode/decode | Established, slower DOM path | Typed outer path; materially faster |
| Journal encode/decode | Established, slower DOM path | Typed per-operation records; materially faster |
| Output compatibility | Current canonical source | Exact for fixtures and all WireValue alternatives tested |
| Dynamic WireValue | Straightforward DOM dispatch | Explicit dispatch plus local dynamic parse; more code |
| Errors | Existing normalized boundary | Glaze errors normalized to the same Gargantuan model |
| Debuggability | Familiar runtime DOM | More template diagnostics and compiler sensitivity |
| Compile impact | Existing baseline | One heavy private TU; MSVC 19.50 required for pinned revision |
| Cross-language behavior | Existing JSON | Identical JSON; no C#/Go implication |

The complexity is manageable when confined to a few high-volume typed codecs,
but it does not justify rewriting every JSON surface. In particular, exact float
lexemes and heterogeneous Journal/WireValue shapes require deliberate code that
automatic reflection cannot safely define. Golden fixtures remain mandatory for
each migrated codec.

## Validation and security

The prototype preserves Gargantuan ownership of document bytes, UTF-8, nesting,
JSON-node count, collection limits, string limits, integer narrowing, finite
numbers, ObjectIds, schema identity/version, hierarchy, Attributes, Tags,
extensions, and custom-class validation. Snapshot decode reuses the engine-owned
semantic validator; Journal decode performs the same schema-aware operation
checks. Third-party exceptions and `glz::error_ctx` do not cross the private
boundary.

Focused tests cover malformed and truncated JSON, oversized documents, invalid
UTF-8, missing required fields, unknown fields, last-value-wins duplicate fields,
invalid typed WireValues, every WireValue alternative, minimal/complex golden
fixtures, and canonical byte equality. Foundation and networking regression
tests provide the broader cross-layer check.

## Deliberately deferred

- moving Snapshot or Journal production selection to Glaze;
- Glaze prototypes for Project, EditorHost, or schema discovery;
- final binary game codec;
- binary project format;
- basic client replication;
- remotes;
- Node protocol choice; and
- EditorHost binary migration.

The next architecture milestone is:

`RESUME NETWORKING — BASIC CLIENT REPLICATION`
