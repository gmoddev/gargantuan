# Input action foundation qualification and continuation

Qualified on 2026-11-03 on the isolated `feature/input-action-foundation` branch,
based on `eb2ce26ab`. No merge into main is part of this work.

The canonical contract and Luau API are in [InputActions.md](InputActions.md).
The implementation covers persistent schema-backed definitions, construction,
keyboard chords, deterministic ownership and transitions, incremental indexed
evaluation, bounded inspection and the ActionMap/default-controller adapter.

## Exact source checkpoint

The qualified code digest is:

```text
1c954844bc007637c43071a2accf18ad579131e8b361b7616e9d9f9d972525cc
```

[SourceManifest.json](../Validation/input-action-foundation-20261103/SourceManifest.json)
records SHA-256 for all 31 changed code/schema/test/build files, all 132 generated
reflection files and the dependency revisions. Unchanged tracked source comes
from the recorded base commit. The worker verified every recorded source and
generated hash before executing the focused suite and benchmark.
Each changed source also records its normalized Git blob ID, so checkout
line-ending conversion can be distinguished from a code change.
[source-verification.json](../Validation/input-action-foundation-20261103/source-verification.json)
records that digest, verification time and the seven executable hashes. These
files bind evidence to source independently of later documentation commits.

Implementation landmarks:

- `assets/classes/Input{Context,Action,Binding}.luau` and
  `assets/services/InputService.luau` own the native schema.
- `include/gargantuan/input/CompiledInputGraph.hpp` and
  `src/input/CompiledInputGraph.cpp` own the disposable evaluator.
- `src/services/InputService.cpp` observes committed configuration, compiles at
  safe points, commits state before callbacks and builds inspection copies.
- `src/services/ActionMap.cpp` is the compatibility facade; its bindings are real
  Instances. The frozen old evaluator exists only in benchmark test sources.
- `tests/InputActionTests.cpp` covers the new contracts. Existing controller
  tests remain intact. Foundation assertions now include the persistent input
  service in the minimum authoring document.

## Build and runtime verification

Worker: `dockerbox`, Windows x64, AMD Ryzen 9 5900X (12 cores / 24 threads), approximately 32 GiB
RAM. Builds began with four jobs and finished incrementally with eight after
checking memory headroom. Test parallelism was two. No sustained native build
ran on the controlling PC.

```text
Workspace: C:\Sandbox\Codex\Workspaces\gargantuan-input
Build:     C:\Sandbox\Codex\Builds\gargantuan-input\msvc1444-release
Logs:      C:\Sandbox\Codex\Logs\gargantuan-input
CMake:     3.31.10
Compiler:  MSVC 19.44.35228.0 (toolset folder 14.44.35207), Release, C++23
Ninja:     C:\Strawberry\c\bin\ninja.exe
glslc:     C:\Sandbox\Codex\Cache\gargantuan\shaderc\glslc.exe (2026.2)
```

Configuration: `BUILD_TESTING=ON`, `GARGANTUAN_TRACY=OFF`,
`GARGANTUAN_WITH_GNS=OFF`, renderer/serialization benchmarks enabled, and
`CMAKE_DISABLE_PRECOMPILE_HEADERS=ON`. The last flag avoids an SDL PCH/path-map
failure with this older worker compiler; production source flags were retained.
Dependencies came from the repository's existing pinned submodules. Generated
reflection was produced with the pinned local Lute generator and transferred.
The dedicated build and dependency state remain available for incremental work.

| Check | Result |
| --- | --- |
| Generated reflection and complete configured default build | Passed |
| Focused input, benchmark smoke, foundation, player, platform, GUI and interaction suites | 7/7 passed, 3.84 s |
| Complete registered GNS-disabled matrix | 54/54 passed, 45.66 s |
| Full input benchmark | 18 scenarios, 20,000 measured events each; passed baseline value checks |
| Source/generated hash verification | 31 source files and 132 generated files matched |
| Diff whitespace validation | Passed |

The complete matrix includes authoring/persistence/history, packaging and
relocation, dedicated server roles, content availability/ownership/unwind,
networking contracts, scheduler, replication/relevance, character networking,
Remotes, renderer headless paths, physics, animation and GUI. Raw results:
[focused JUnit](../Validation/input-action-foundation-20261103/tests.xml),
[complete JUnit](../Validation/input-action-foundation-20261103/all-tests.xml),
[complete output](../Validation/input-action-foundation-20261103/all-tests.log).

Specific new assertions cover save/reopen, construction/parenting/destruction,
EditorHost schema/Properties/Undo/Redo, four side-specific modifiers, Ctrl+Shift+S,
repeat and consumed releases, exact chords, focus loss, enabled-state changes,
held-key takeover, active rebinding, unchanged-row preservation, stale IDs,
invalid configuration/capacity rejection and recovery, bounded observers,
replicated definitions with local-only runtime state, diagnostic accuracy,
callback reentrancy, authored-default preservation and mixed legacy signal values.

Two supporting fixes were required for normal Instance ergonomics: native enum
property writes check the source enum type before conversion, and EnumItems
compare by declared enum type/item value. Tests exercise both through the public
Luau API. Context takeover cancels lower canonical and legacy sources, and waits
for a fresh primary press before activating the new context's action.

This is a GNS-disabled headless qualification using MSVC 19.44. It does not claim
the official MSVC 19.50/GNS-enabled Windows gate, Linux sanitizer gate, GPU output,
physical devices or private Studio UX have been qualified. No networking format,
Foundation 3L authority path or private Studio implementation was changed.

## Performance comparison

[benchmark.json](../Validation/input-action-foundation-20261103/benchmark.json)
contains every percentile, allocation count, storage estimate and compilation
measurement. The baseline is a benchmark-only renamed copy of
`eb2ce26ab:src/services/ActionMap.cpp`, frozen before the adapter rewrite.

The following rows use eight compiled context partitions. Latencies are
P50 / P95 / P99 in **microseconds**:

| Bindings | Distribution | Candidates | Old ActionMap | Compiled evaluator | Old / new allocations per event |
| --- | --- | ---: | --- | --- | --- |
| 16 | Spread, miss | 0 | 0.2 / 0.2 / 0.3 | 0.1 / 0.1 / 0.1 | 2 / 0 |
| 16 | Dense | 16 | 7.0 / 7.4 / 8.0 | 1.0 / 1.2 / 1.2 | 35 / 0 |
| 128 | Spread | 5 | 5.3 / 5.3 / 6.0 | 0.4 / 0.5 / 0.5 | 17 / 0 |
| 128 | Dense | 128 | 113.4 / 119.3 / 133.7 | 8.4 / 9.7 / 11.3 | 158 / 0 |
| 512 | Spread | 19 | 53.2 / 55.1 / 61.0 | 1.3 / 1.5 / 1.9 | 38 / 0 |
| 512 | Dense | 512 | 370.0 / 390.5 / 429.7 | 14.2 / 19.1 / 23.0 | 162 / 0 |

The matrix also includes one and 32 context partitions. Every compiled scenario
measured zero ordinary executable C++ `new/new[]` requests during evaluation.
The small spread case intentionally has no W binding and measures a lookup miss.
The other spread cases distribute bindings across letters; dense cases share W.

Cold compilation and memory are tradeoffs, not improvements. Capacity-based
storage estimates at eight contexts are:

| Bindings | Old storage, bytes | Compiled storage, bytes |
| --- | ---: | ---: |
| 16 | 3,464 | 118,936 |
| 128 | 22,104 | 135,512 |
| 512 | 63,144 | 158,552 |

Single-binding edit latency, again P50 / P95 / P99 in **microseconds**:

| Bindings | Distribution | Native compilation | Old remove + add |
| --- | --- | --- | --- |
| 16 | Spread | 64.8 / 70.3 / 115.6 | 0.8 / 1.0 / 1.3 |
| 16 | Dense | 66.3 / 72.0 / 84.7 | 0.8 / 1.0 / 2.5 |
| 128 | Spread | 164.1 / 172.7 / 239.6 | 1.8 / 2.0 / 3.9 |
| 128 | Dense | 175.6 / 194.7 / 227.4 | 1.8 / 2.2 / 3.4 |
| 512 | Spread | 374.3 / 400.9 / 502.7 | 4.1 / 5.0 / 8.0 |
| 512 | Dense | 433.4 / 494.4 / 529.0 | 4.1 / 5.1 / 7.6 |

These are native evaluator measurements. Physical input dispatch, Instance
snapshot extraction and Luau subscribers are excluded. The baseline includes
empty native signal publication; compiled transition dispatch is excluded.
Legacy ActionMap has no contexts, so the compiled partitions use compatibility
passthrough. Timings have visible 0.1-microsecond clock granularity and no timer
overhead subtraction. Storage estimates exclude allocator bookkeeping,
temporary compiler storage and the authoritative Instance/observer graph.
Aligned allocations are outside the allocation counter. These results do not
establish end-to-end InputService/Luau latency or total system memory savings.

## Continuation

Reproduce using [Build.cmd](../Validation/input-action-foundation-20261103/Build.cmd),
[Run.ps1](../Validation/input-action-foundation-20261103/Run.ps1) and
[FullRun.ps1](../Validation/input-action-foundation-20261103/FullRun.ps1).
Before running, synchronize this branch plus generated reflection and the
manifest's dependency revisions into the recorded worker workspace. Preserve
unchanged timestamps and the build cache. The worker source is an exported tree,
not a Git checkout. Execute PowerShell scripts with process-only
`-NoProfile -NonInteractive -ExecutionPolicy Bypass`; the runners suppress native
error dialogs for child tests and use SDL dummy drivers. The complete build is:

```text
cmake --build C:/Sandbox/Codex/Builds/gargantuan-input/msvc1444-release --parallel 8
```

Do not resume by restoring the old evaluator or adding another input pipeline.
Next work should extend the existing read-only snapshot into an explicitly
versioned EditorHost runtime DTO, add source-associated validation errors and a
Studio chord/runtime view, then qualify physical modifier/focus routing. Cold
snapshot/compiler allocation and fixed lookup-table storage are measured
optimization candidates. Gamepad axes, keyboard vector composites, touch gestures
and remapping UI remain later stages with their own semantics and qualification.
