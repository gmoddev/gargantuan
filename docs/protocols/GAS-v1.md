---
status: current
owner: serialization
---

# Gargantuan Asset Serialization v1

Gargantuan Asset Serialization (GAS) defines one binary document with two
profiles. Gargantuan Asset Serialization Model (`.gasm`) carries an ordered
forest of reusable Instances. Gargantuan Asset Serialization Place (`.gasp`)
carries a single DataModel tree and place metadata. The header determines which
profile a file contains. `.gasmx` and `.gaspx` are reserved for possible textual
representations. This specification defines the clear binary codec. The
[protected export proposal](../adr/GAS-Protected-Exports.md) covers potential
encryption of the complete canonical document.

The [codec](../../src/serialization/GasDocument.cpp) produces or consumes an
inert [GAS document](../../include/gargantuan/serialization/GasDocument.hpp).
Live graph validation, restoration, and operation authorization run at a
separate semantic boundary. See the [foundation inventory](../architecture/GASFoundationInventory.md)
for the existing systems that own those operations.

## Primitive representations

- `u8`, `u16`, `u32`, and `u64` are unsigned fixed-width integers. `i32` is a
  signed fixed-width two's-complement integer. Multibyte numbers use
  little-endian order. Fields immediately follow one another without padding.
- `f32` and `f64` are IEEE 754 binary32 and binary64 bit patterns carried as
  little-endian `u32` and `u64`, respectively. All components must be finite.
  The writer encodes either signed zero as positive zero, and the reader
  rejects negative zero. Finite nonzero values, including subnormals, retain
  their exact bit patterns.
- `Text` is a `u32` byte count followed by that many shortest-form UTF-8 bytes.
  It admits Unicode scalar values other than U+0000 and preserves their bytes
  without normalization. Strings have no terminator. An initial U+FEFF is text
  data. `Identifier` is nonempty `Text` of at most 256 bytes. Other text can
  be empty and is limited to 65,536 bytes unless a field gives a different limit.
- `SchemaId` and `AssetId` each use a `u64` high word followed by a `u64` low
  word, with at least one nonzero word. Numeric ID ordering compares the high
  word first and the low word second. `ProjectId` and `AssetContentId` use their
  raw 16-byte and 32-byte arrays respectively, in the order of their canonical
  hex byte pairs. An all-zero identity or content digest is invalid in those
  fields.
- Text keys are ordered lexicographically by unsigned UTF-8 byte, with a
  shorter prefix first. This order is independent of locale and case collation.

## Header, section directory, and trailer

The fixed header occupies 20 bytes.

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | 8 bytes | `47 41 53 1a 0d 0a 02 0a` in hexadecimal |
| 8 | `u16` | Binary format profile, `1` through `4` |
| 10 | `u8` | Document kind, `1` for GASM or `2` for GASP |
| 11 | `u8` | Section count, `2` for GASM or `3` for GASP, plus one for format `4` |
| 12 | `u32` | GAS document schema version, `1` |
| 16 | `u32` | Total byte length including the digest |

The directory immediately follows the header, with one 12-byte entry per
section. Each entry comprises a `u32` section ID, `u32` absolute offset from
file start, and `u32` byte length. Section ID `1` is the graph, ID `2` is the
dependency table, and ID `3` is place metadata. Every section for the declared
kind is required. V1 has no optional sections.

Directory entries and their sections appear in ascending ID order. Graph data
starts immediately after the directory. Sections must have positive lengths
and exactly cover the region up to the digest trailer, without gaps or
overlaps. Each section reader consumes exactly its declared span.

The final 32 bytes contain the raw SHA-256 digest of all preceding bytes,
including the patched total length and directory. This digest checks file
integrity. Publisher authentication and confidentiality require an independent
mechanism. V1 contains no compression, ciphertext, external offsets, or
trailing fields.

## Graph section

The section starts with a `u32` node count followed by that many nodes in
depth-first preorder. The 1-based index assigns each node its document-local
identity, so node records carry no separate ID. Root order and sibling order
follow the serialized order.

| Order | Type | Node field |
| ---: | --- | --- |
| 1 | `u32` | Parent node index, or zero for a root |
| 2 | `SchemaId` | Class identity |
| 3 | `u32` | Nonzero class definition version |
| 4 | `Text` | Instance name |
| 5 | Property map | Native saved properties |
| 6 | Property map | Attributes |
| 7 | Property group list | Extension overrides by owner identity |
| 8 | Property group list | Custom class overrides by declaring class identity |
| 9 | `u32` then identifiers | Tag count and strictly ascending tag identifiers |

The graph must contain at least one node. A nonzero parent must be an active
ancestor in the preorder stack, which excludes self-parenting, cycles, forward
parent indices, and reopening a closed subtree. A zero parent starts a root and
resets this stack. Root depth is one. GASM permits one or more roots and
excludes the native `Engine.DataModel` class identity throughout. GASP requires
exactly one root with the native `Engine.DataModel` identity and excludes that
identity from descendants. The root identity is
[`SchemaId::FromNativeName("Engine", "DataModel")`](../../include/gargantuan/reflection/SchemaId.hpp).
Service placement, protected classes, and class-specific hierarchy restrictions
require the runtime schema and a destination validator.

A property map is a `u32` count followed by records containing an `Identifier`
key and a tagged value. Map keys must strictly increase. A native member is
identified by its canonical key in the effective property schema of the node's
class and definition version. The registry supplies its declaring class and
type. Individual native property records carry no separate numeric member ID.

A property group list is a `u32` count followed by records containing `SchemaId`
owner, nonzero `u32` definition version, and a nonempty property map. Owner IDs
must strictly increase within each list. Both the owner identity and member key
identify a grouped member. The codec rejects duplicate keys, owners within a
list, and tags. It does not interpret an attribute, tag, or member name as
authority.

## Value encodings

A `u8` tag precedes each value payload. Fields in the payload column are listed
in wire order.

| Tag | Meaning | Payload |
| ---: | --- | --- |
| 0 | Null | Empty |
| 1 | Boolean | `u8`, `0` or `1` |
| 2 | Integer | `i32` |
| 3 | Double | `f64` |
| 4 | Float | `f32` |
| 5 | String | `Text` |
| 6 | Vector2 | Two `f32` values in X, Y order |
| 7 | Vector3 | Three `f32` values in X, Y, Z order |
| 8 | Color3 | Three `f32` values in R, G, B order |
| 9 | UDim | Scale `f32`, offset `i32` |
| 10 | UDim2 | X scale `f32`, X offset `i32`, Y scale `f32`, Y offset `i32` |
| 11 | CFrame | Twelve `f32` values, position X/Y/Z followed by the three rotation matrix columns, each in row order |
| 12 | Native enum item | Enum type `Identifier`, then item `Identifier` |
| 13 | Schema enum item | Enum `SchemaId`, nonzero definition version `u32`, signed item value `i32` |
| 14 | Local Instance reference | Target node index `u32`, or zero for a null reference |
| 15 | Binary | Byte count `u32`, then raw bytes |

Only native saved-property maps admit tags 14 and 15. General null (tag 0)
and a null reference (tag 14 with target zero) are distinct wire values.
Nonzero reference targets may point forward or across roots and must be within
the graph. Reference cycles are representable where the reflected members allow
them. Importers must validate target class, scope, and nullability and resolve
targets using freshly allocated destination identities. External and live
slot/generation references have no wire representation. A semantic exporter
must reject an external edge or apply an explicit nullable-reference policy
that reports the omission. A decoder cannot search the destination by name,
path, or source identity to restore an edge.

Script source uses the `Text` value in the reflected `Source` property.
Binary values provide an opaque byte representation, including NUL bytes. A
semantic importer must reject any binary value for which it lacks an admitted
destination property representation. Decode never executes scripts, compiles
bytecode, activates PreRun, or resolves a filesystem path.

## Dependency and place sections

The dependency section starts with a `u32` count followed by that many
48-byte records. Each contains the `AssetId` and `AssetContentId` of a required
artifact. Records must be strictly ascending by `AssetId`; multiple IDs can
pin the same content digest. The section is required even when empty. It
carries no asset bytes or filenames. The semantic loader resolves these pins
through the existing asset catalog and content-addressed storage and verifies
the expected content and dependency closure before publication. Asset-valued
properties continue to use the reflected asset-reference representation.

The place section starts with a `ProjectId`, then a `u8` presence flag, `0` or
`1`. When present, the optional PreRun source follows as `Text` with a
262,144-byte limit. A present empty source differs from an absent source.
Project title is carried by the DataModel name. The GAS schema version in the
header is separate from each runtime class, group, and enum definition version.
Place metadata contains no runtime state or authority credentials.

## Native BlockGrid section, format 4

Writers select format 4 when at least one saved BlockGrid field exceeds 262,144 bytes.
GASM uses section IDs 1, 2 and 4. GASP uses 1, 2, 3 and 4. Its place section
begins with one byte selecting the ordinary place-metadata profile. Profile 1
has ordinary metadata, profile 2 includes a Workspace terrain digest and profile
3 includes domain terrain bindings. Older compatible readers continue to handle
files that use their supported profiles.

Only a saved native `Engine.BlockGrid` node can reference section 4. Its
`NativeBlockCellSegmentsV2` binary record contains the four ASCII bytes `BGSM`,
version byte 1, a little-endian `u32` canonical field length, a 32-byte SHA-256
of that entire field and the ordered 32-byte SHA-256 digests of its segments.
The number of digests is the length divided by 262,144 and rounded up. The range
is two through 64. The final segment can be shorter, and all earlier segments
have exactly 262,144 bytes. One node can carry one of the inline or segmented
cell representations.

Section 4 starts with a `u32` count. Each entry contains its 32-byte digest, a
`u32` length and the raw bytes. Unique entries appear in ascending digest order.
The required hashes and lengths come from the manifests in the graph. Readers
check the exact closure, each digest and the ordered full-field digest before
returning the document. Semantic loading then validates canonical native cells,
definitions, channels and references using the ordinary field decoder.

## Limits and rejection

| Resource | Maximum |
| --- | ---: |
| Ordinary document, or all format 4 bytes outside section 4 including the digest | 8,388,608 bytes |
| Section 4 logical native bytes across all owners, counting repeated references | 67,108,864 bytes |
| Section 4 distinct segment entries | 512 |
| Section 4 physical byte ceiling | 4 + 512 × 36 + 67,108,864 bytes |
| Nodes | 65,536 |
| Hierarchy depth including root | 64 |
| Native saved properties per node | 1,024 |
| Attributes per node | 64 |
| Extension groups per node | 256 |
| Custom class groups per node | 256 |
| Properties per group | 1,024 |
| Tags per node | 64 |
| Values across all property maps | 1,048,576 |
| Reference records, including null references | 1,048,576 |
| Asset dependencies | 16,384 |
| Identifier, ordinary text, PreRun source | 256, 65,536, and 262,144 bytes, respectively |
| Binary value | 1,048,576 bytes |

The decoder validates bounds and contiguous spans, verifies the digest, and
checks record counts against each section's remaining bytes before allocating
or traversing variable-size data. Graph depth uses a bounded ancestor stack.
Each node, group, and dependency is visited once for admission. Every property
value consumes the global value budget. There is no decompression step.
The codec returns a complete inert document or a structured serialization
error. A successful parse grants no authorization to publish. Apply reflected
value validation, destination authority, lifecycle rules, and transactional
rollback at the existing project or mutation boundary before use.

## Canonical bytes and compatibility

Canonical encoding preserves graph preorder and uses ascending order for
map keys, tag names, property group owner IDs, and dependency AssetIds.
Document-local node IDs derive from preorder. Changing durable identifiers,
root order, sibling order, text bytes, or definition versions changes the
logical input. Reordering a tag, group, or dependency collection and changing
the sign of zero leave canonical output identical. The writer adds no
timestamps or allocation-order-dependent identities.

Readers must reject unsupported format profiles and GAS schema versions,
unknown value tags, and missing, unexpected, duplicated, or unordered
sections. A syntactically valid but unrecognized class or member identity can
exist in the inert document because inspection and re-encoding require no live
registry. Semantic import must reject an unknown or incompatible class or
member unless an explicit migration handles it. A deprecated member can be
read when the selected definition still supports it as a saved member. A
removed member requires explicit migration. General schema migration does not
occur in the codec. Unsupported values must produce a clear export or import
error.
Existing JSON project versions continue through the
[existing project persistence contract](../../devdocs/CurrentArchitecture/ProjectPersistenceV5.md).
