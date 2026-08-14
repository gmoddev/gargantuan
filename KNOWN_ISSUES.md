# Known issues

This file tracks verified current defects and engineering gaps that are useful
to contributors but do not belong in the feature roadmap. An entry remains
open until its resolution criteria are implemented and verified.

## KI-001: Render pass labels prevent a clean Windows compile

- Status: Open
- Priority: High
- Area: Rendering and Windows build portability
- Upstream reference: [teamfireworks/gargantuan#34](https://github.com/teamfireworks/gargantuan/issues/34)
- Relevant code:
  - `include/gargantuan/render/RenderPass.hpp`
  - `src/render/passes/GuiPass.cpp`
  - `src/render/passes/OpaquePass.cpp`
  - `src/render/passes/ShadowPass.cpp`
  - `src/scripting/LibVector3.cpp`

`RenderPass` declares an uninitialized `static constexpr std::string LABEL`,
which is ill-formed. Derived render passes use `std::string_view` labels, and
`LibVector3.cpp` calls `std::strcmp` without directly including `<cstring>`.

The upstream fix in commit
[`b041227`](https://github.com/teamfireworks/gargantuan/commit/b0412279a10cd70d202ff6f38223d793795a2fdc)
corrects the derived labels and missing include but leaves the invalid base
declaration unchanged. Do not cherry-pick it as-is.

Resolution requires one consistent compile-time label representation across
the base and derived render passes, the explicit `<cstring>` include, and a
successful Windows build.

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

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
