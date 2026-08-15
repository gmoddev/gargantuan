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

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
