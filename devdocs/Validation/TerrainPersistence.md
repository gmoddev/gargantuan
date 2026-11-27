# Native terrain persistence qualification

Continuation baseline `78c773342`. The native persistence owner captures immutable authority, builds a content-addressed terrain closure, and restores that closure into a detached `Field`. It uses the Engine's `AssetContentId::Hash` SHA-256 and the existing GTCH/GTMF codecs. The Project/GAS owner provides the confined artifact reader and commits the root. [Native API](../../include/gargantuan/terrain/Persistence.hpp), [chunk and manifest formats](TerrainChunkCodec.md).

## Authority and identity

- `Field::CaptureMaterialized` freezes sorted materialized pages and the exact committed revision on Main. The capture shares existing immutable pages and excludes version-only tombstones. A pending worker can own prepared terrain pages without those pages becoming part of the save. A second encoding overload consumes only the frozen capture. Save callers can use it away from Main after acquiring the exact project/field snapshot.
- `GetTerrainDatasetId` maps the Engine's nonzero 16 project identity bytes to separate typed High and Low values. This is the stable identity for the single `Workspace.Terrain` dataset. Save As under a new project ID changes the manifest binding. Unchanged chunk artifacts remain byte-identical because their bindings use their own signed keys and lattice spacing.
- A capture receipt keeps the live field's revision and the incremental index keeps the live field's lifetime and per-chunk geometry/material versions. GTMF v1 represents authoritative content and preserves the same digest for the same terrain and project binding. A reopened field acquires a fresh lifetime and local revisions through its detached commits. Former runtime tickets cannot resolve in that lifetime. Synchronizing durable gameplay or edit-log sequence numbers would require a separately versioned contract.
- `GetMaterializedChunkKeys(Maximum)` supplies bounded ascending keys for restore derivation. The enumeration excludes tombstones and retains positive-only scalar data if it differs from saturated air.

## Integrity, budgets and incremental reuse

The artifact purpose and SHA-256 digest construct fixed root-relative `.gargantuan/terrain/chunks/<digest>.gtch` and `.gargantuan/terrain/manifests/<digest>.gtmf` paths. Only the typed digest and a known purpose contribute path elements. File confinement, installation, directory creation, and root durability belong to the Project/package owner. The native return contains sorted immutable byte buffers. It never opens a file or publishes a field.

Capture enforces caller limits within a hard 65,536-chunk and 256 MiB complete encoded closure budget. The aggregate includes unchanged reused leaf bytes so a partial artifact result cannot bypass admission. `Snapshot::ReadBrick` resolves known air and uniform bricks directly, expanding only mixed bricks into dense temporary samples. The byte output stays canonical. Each artifact uses the Engine SHA-256 helper. Optional prior-index reuse requires the same project, dataset, spacing, field lifetime and exact chunk version. Use it only after that complete previous closure has been verified durable at the same destination. For Save As or a new package destination, request a full closure or independently prove that the destination owns the referenced bytes. The prior index is trusted process-local state.

Load verifies the exact root digest and manifest binding, checks the aggregate declared bytes before reading any leaf, and requests each leaf under its exact encoded size cap. It computes SHA-256 over returned owned bytes and checks data length, codec version, key and grid. It applies each decoded chunk through the validated Field preparation and commit path in a detached field. Signed density and stable material IDs survive exactly. Fresh detached air lets this importer omit only exact default writes after decode. A missing, invalid, canceled, or overbudget operation discards the detached field and all intermediate results. The previous live authority stays outside the loader.

Encoded byte caps bound returned artifact size. They do not measure allocator or OS working-set peaks. A full in-memory capture can retain the whole compressed closure and a frozen set of page references while later edits create new pages. A load retains the manifest directory, a verifying SHA-256 input copy, one bounded chunk decode and its preparation intermediates, and the growing detached field under its Field limits. Project-level concurrent saves, file and package copies, old generation pins, and garbage collection need separate complete-path resource and lifecycle accounting.

## Release evidence

[TerrainPersistenceTests.cpp](../../tests/terrain/codec/TerrainPersistenceTests.cpp) passed x64 MSVC 19.50.35728 with `/O2 /DNDEBUG /W4 /WX`. On the Ryzen 9 7950X3D host, an in-memory capability reader served the actual SHA-256-verified buffers. The fixture compared 98,304 authoritative owned samples across negative uniform rock, an excavated cave, and a disconnected island. It preserved half-unit spacing, material-only changes, non-default air densities, exact saturated air and sparse layout. A worker prepared an edit while Main froze the prior committed capture, then Main published the edit. The old capture and old manifest remained independently restorable.

Denials covered wrong project/dataset, mutated manifest and leaf hashes, malformed leaf length, a digest-valid leaf with the wrong signed key, a digest-valid unsupported codec version, absent leaf payload, aggregate admission before leaf reads, detached field byte limits, moved-from capture use, and cancellation. The chunk codec golden mode/size and replacement tests also passed after the immutable brick-read change.

The unpainted closure occupied 33,752 bytes. The painted closure occupied 34,844 bytes. Incremental capture reused two leaf references and supplied the 34,620 bytes for the changed cave leaf and its manifest. The palette change increased that cave leaf's encoded bytes. The three-page fixture's small uniform and sparse leaves provide little favorable CPU contrast between full and incremental capture. Larger untouched mixed sets require their own measurements.

| Native operation, 31 matched samples | p50 | p95 | Worst |
| --- | ---: | ---: | ---: |
| Full capture | 313.2 µs | 342.9 µs | 365.5 µs |
| Incremental capture after paint | 313.7 µs | 334.2 µs | 352.0 µs |
| Verified detached restore | 3,641.1 µs | 3,922.8 µs | 4,154.6 µs |

The timing scope includes exact leaf encoding/hash and immutable buffer construction, or manifest/leaf verification and authoritative detached reconstruction. It excludes storage flush, original shape generation, physical file replacement, main-game frame work, GPU and physics reconstruction, and packaging. Actual GAS save/reopen, persisted artifact retirement and provider-ready reopen require their own acceptance receipts.

Run the native test from the production worktree with an x64 Developer Command Prompt and the Engine's checked-out GLM headers.

```bat
set "TerrainPersistenceRoot=%CD%"
mkdir "%TEMP%\GargantuanTerrainPersistence"
pushd "%TEMP%\GargantuanTerrainPersistence"
cl /nologo /std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX /I"%TerrainPersistenceRoot%\include" /external:I"%TerrainPersistenceRoot%\vendor\glm" /external:W0 "%TerrainPersistenceRoot%\tests\terrain\codec\TerrainPersistenceTests.cpp" "%TerrainPersistenceRoot%\src\terrain\Persistence.cpp" "%TerrainPersistenceRoot%\src\terrain\ChunkCodec.cpp" "%TerrainPersistenceRoot%\src\terrain\Manifest.cpp" "%TerrainPersistenceRoot%\src\terrain\Field.cpp" "%TerrainPersistenceRoot%\src\assets\AssetTypes.cpp" "%TerrainPersistenceRoot%\src\filesystem\ProjectIdentity.cpp" /Fe:TerrainPersistenceTests.exe
TerrainPersistenceTests.exe
popd
```
