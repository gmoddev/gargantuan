# GAS authoring and package input

[GAS v1](GAS-v1.md) carries persistent graph state. The Instance persistence
adapter captures saved reflected members, ordered children, tags, attributes,
extension state, and script source under the active frozen schema. It uses
the existing archivable traversal. An export fails when a saved reference
leaves the selection, including a reference to an omitted descendant. A
place exported from legacy JSON retains the state the legacy reader
materializes. Older files can lack references or durable Script SourceIds.
Opening and saving them in the existing project format establishes the
migrated state before repeated GASP exports.

## EditorHost

Handshake advertises the following methods in the existing EditorHost
protocol. Studio can encode and save the returned data as a GAS file and
submit bytes from a selected file. The host receives bytes and generation-safe
object IDs. The current transfer limit is 512 KiB of decoded data in either
direction. `DataBase64` is an array of Base64 strings with at most 65,536
bytes per chunk. Concatenating the chunks produces standard, padded Base64.
The normal request, response, and transaction limits still apply.

| Method | Parameters | Result |
| --- | --- | --- |
| `ExportGasModel` | `Roots`, an ordered array of 1–256 live ObjectIds from the current project. Optional `AssetIds`, an array of canonical project asset ID strings. | `DataBase64`, `DocumentKind: "Model"`, `FormatVersion: 1` |
| `ImportGasModel` | `Parent`, `ExpectedRevision`, `DataBase64` | Inserted `Root` and `ProjectState` |
| `ExportGasPlace` | Empty object | `DataBase64`, `DocumentKind: "Place"`, `FormatVersion: 1` |
| `OpenGasPlace` | `ExpectedRevision` and `DataBase64` | New DataModel `Root` and `ProjectState` |

Exports require project read and editor authority. Writes require mutation and
editor authority, an established snapshot cursor, the current authoritative
revision, and a closed authoring transaction. Play must be stopped before
writes. The host checks profile and integrity before admission.

GASM import verifies the destination catalog's asset IDs, artifact digests
and transitive dependencies. Standard reflected asset fields are resolved
automatically. Supply `AssetIds` for assets a script selects at runtime.
GASM carries pins, so the Studio client must arrange availability through
the existing asset workflow before inserting into a separate project. A
missing artifact or an asset ID with a different digest aborts insertion.
Model imports cannot insert protected service classes or a FileLink source
mount. One imported root is placed directly under `Parent`. Multiple roots
are grouped under a new `Imported Model` Folder in source order to adopt the
entire cross-referencing forest in one operation. The returned ID names that
Folder in the multi-root case.

The gateway validates and reconstructs the candidate graph in isolation,
remaps Instance and Script SourceIds, and commits one authoring history
entry and journal batch. Undo and Redo use the existing persistent subtree
history. Script source stays inert during parsing and editing. Normal Play
and runtime activation remain governed by the engine's execution policy.

GASP open uses the current project's identity, approved PreRun schema, and
existing content catalog. A different identity, PreRun source or asset
closure aborts the open. An accepted open replaces the authoring scope and
marks the place dirty for an explicit save. Studio must discard the old
cursor and object selection and fetch a fresh snapshot. The subsequent
normal save writes the existing JSON project representation.

## Command-line conversion

The standard packager accepts a GASP export plus a trusted project origin.
The origin supplies `.gargantuan/project.json`, its existing catalog and CAS,
and applicable PreRun and source-mount files. The built-in project JSON
discovery and format remain available.

```text
gargantuan-packager export-gasp --project <root> --output <file.gasp>
gargantuan-packager restore-gasp --project <root> --gasp <file.gasp>
gargantuan-packager build --project <root> --gasp <file.gasp> --output <directory> [--runtime <distribution>]
```

Restore creates the JSON scene in a same-origin workspace with no existing
scene file. To replace a scene during authoring, open the GASP in EditorHost
and use the existing save flow. The binary codec uses the header contents to
identify a place. A GASP export uses the `.gasp` extension.

GASP feeds the same immutable `GamePayload` builder as JSON. The packaged
bootstrap, content partition, CAS artifacts, manifests, integrity validation,
and role-specific runtime distribution retain their current formats. Studio
and the command line can use GASP as authoring interchange without requiring a
new runtime representation. Package source follows the existing engine
execution policy. Using a server distribution does not itself enforce
confidentiality of server source in a separately distributed player package.

The place carries persistent graph and PreRun text. Source-mount files,
asset artifacts and reimport provenance still belong to the selected project
directory. Opaque binary values are representable in the codec. Hydration
requires a reflected destination property adapter and rejects unsupported
values. The proposed [protected export envelope](../adr/GAS-Protected-Exports.md)
has a separate qualification plan.
