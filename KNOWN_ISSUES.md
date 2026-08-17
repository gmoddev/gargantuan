# Known issues

This file tracks verified current defects and engineering gaps that are useful
to contributors but do not belong in the feature roadmap. An entry remains
open until its resolution criteria are implemented and verified.

## KI-002: Box3D diagnostics are not integrated with engine logging

- Status: Open
- Priority: Low
- Area: Physics observability
- Upstream reference: [teamfireworks/gargantuan commit `7a38dd9`](https://github.com/teamfireworks/gargantuan/commit/7a38dd9e188d25f784264ff0000e69dd4c7e63b1)
- Relevant code:
  - `include/gargantuan/Log.hpp`
  - `src/Log.cpp`
  - `src/Main.cpp`

Box3D warnings currently bypass Gargantuan's structured logging categories.
This makes physics failures and performance warnings harder to diagnose.

Resolution requires forwarding `b3SetLogFcn` through a clearly named
Physics/Box3D logging category, preserving pretty and JSON output behavior, and
verifying that representative Box3D warnings are emitted once at the intended
severity. The upstream abbreviated `B3D` category should be adapted to the
repository's subsystem-oriented logging convention rather than copied verbatim.

## KI-003: FileLink silently skips nested Instance JSON files

- Status: Open
- Priority: Medium
- Area: Projects and source synchronization
- Relevant code:
  - `src/classes/FileLink.cpp`
  - `src/assets/InstanceSerialization.cpp`

`InstanceFromPath` recognizes filenames ending in `.instance.json`, but the
deserialization block is disabled under a `FIXME: self recursion` comment and
the function returns `nullptr`. A recursive `FileLink` import therefore omits
nested model files without creating an Instance or returning a structured
failure to the caller.

Resolution requires a bounded, root-confined nested-model import path that
cannot recursively import itself, reports malformed models with actionable
paths, and has coverage for successful imports, malformed files, cycles or
self-reference, and depth/count limits.

## KI-004: Lazy built-in services are inconsistent through direct DataModel access

- Status: Open
- Priority: Medium
- Area: Luau service access
- Relevant code:
  - `src/classes/Instance.cpp`
  - `src/classes/ServiceProvider.cpp`
  - `src/classes/DataModel.cpp`

Direct DataModel service access resolves only an already-instantiated service,
whereas `GetService` constructs a registered lazy service. Consequently, a new
runtime can observe `game.Tags == nil`; after `game:GetService("Tags")`, direct
access resolves the same canonical service and `game.Tags == Tags` is true.

This makes stable built-in service access depend on construction order. It does
not block a vertical slice because `GetService` is available and returns the
correct scoped service.

Resolution requires an explicit service-property contract. Either direct access
must lazily construct registered canonical services, or the distinction from
`GetService` must be intentionally documented and retained with regression
coverage for both paths.

## KI-005: Instance has no pre-removal DescendantRemoving lifecycle signal

- Status: Open
- Priority: Low
- Area: Luau Instance lifecycle API
- Relevant code:
  - `assets/classes/Instance.luau`
  - `include/gargantuan/classes/generated/Instance.hpp`
  - `src/classes/Instance.cpp`

The current hierarchy API exposes `DescendantRemoved` after removal, but no
`DescendantRemoving` signal for observers that need to inspect the prior
hierarchy state. This is an API-completeness gap, not a correctness defect: the
engine does not currently promise the missing signal.

Resolution requires deciding whether both pre- and post-removal descendant
signals belong in the supported lifecycle surface. If adopted, define exact
ordering, parent/hierarchy visibility during the callback, reparent versus
Destroy behavior, subtree ordering, and reentrancy rules before adding the
schema declaration, native signal, and tests.

## KI-006: Play-mode relative pointer input is derived from absolute cursor position

- Status: Open
- Priority: High
- Area: Studio Play input and host pointer semantics
- Relevant code:
  - `gargantuan-studio/src/GargantuanStudio/MainWindow.cs`
  - `include/gargantuan/runtime/HostEvent.hpp`
  - `src/editor/PlaySession.cpp`

During Studio Play, pointer movement is synthesized from successive absolute
viewport positions. This works only while the operating-system cursor can move
inside the viewport/window. At a cursor boundary, one or both derived deltas
become zero despite continued physical mouse movement.

The engine consumes `PointerMoveEvent.Delta` correctly. The defect is in
Studio's Play-mode forwarding: it derives those deltas from absolute positions
instead of forwarding true platform-relative motion while relative-pointer mode
is active. This can make RMB mouse-look stick on an axis, limit yaw by the
cursor/window position, and prevent continuous first-person or custom
mouse-look rotation.

Resolution requires forwarding true physical relative pointer deltas whenever
relative-pointer mode is active. Absolute viewport X/Y must not determine
relative movement. Pointer capture, focus loss, Stop, and runtime exit must
continue to release relative mode reliably, and edit-mode viewport navigation
must remain unaffected. Cursor recentering/warping is acceptable only as a
fallback where raw platform-relative input cannot be obtained cleanly.

Acceptance criteria:

- continuous yaw across multiple full revolutions;
- pitch continues until the controller-defined clamp;
- viewport or screen boundaries stop neither axis;
- entering relative mode has no stale jump;
- focus loss and Stop clear relative mode; and
- re-entering relative mode begins with clean deltas.

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
