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

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
