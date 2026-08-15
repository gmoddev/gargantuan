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

## KI-004: Project names are interpolated into JSON without escaping

- Status: Open
- Priority: Medium
- Area: Project creation and persistence
- Relevant code:
  - `src/filesystem/Project.cpp`

When `Project::fromInit` creates a placeholder DataModel, it concatenates
`projectName` directly into a JSON string. Quotes, backslashes, control
characters, or other JSON-sensitive content can produce an invalid project
file or alter its structure.

Resolution requires constructing the placeholder through the JSON encoder used
by project persistence, followed by round-trip tests for ordinary names,
quotes, backslashes, Unicode, and control characters.

## KI-005: BasePart property edits do not synchronize existing Box3D bodies

- Status: Open
- Priority: High
- Area: Physics and authoritative mutation
- Relevant code:
  - `src/classes/WorldRoot.cpp`
  - `src/classes/BasePart.cpp`
  - `assets/classes/BasePart.luau`

`WorldRoot::CreatePartBody` copies transform, anchoring, collision, touch, and
shape information into Box3D only when the body is created. No property-change
observers update or rebuild an existing body. Authoritative edits to `CFrame`,
`Size`, `Anchored`, `CanCollide`, and `CanTouch` can therefore leave physics
behavior inconsistent with the Instance state seen by scripts, persistence,
replication, Studio, and rendering.

Resolution requires an explicit Main-domain body-update protocol that applies
committed property changes at a safe physics boundary, preserves body and
constraint lifecycle, and tests transform, geometry, body type, sensor/contact,
and repeated-edit behavior.

## KI-006: Vector2 arithmetic regression is disabled after a reported crash

- Status: Open; reproduction required
- Priority: High
- Area: Luau datatype bindings
- Relevant code:
  - `src/datatypes/Vector2.spec.luau`
  - `src/datatypes/Vector2.cpp`

The arithmetic-operator test is commented out with a note that it apparently
segfaults Gargantuan. The current bindings still expose `__add`, `__sub`,
`__mul`, and `__div`, so ordinary Luau arithmetic reaches an unverified native
path. The current environment has not independently reproduced which operator
or operand order triggers the crash.

Resolution requires isolating every vector/vector and vector/scalar operand
order under native and sanitizer coverage, converting invalid operands into
ordinary Luau errors, fixing any unsafe receiver or stack handling, and
reenabling the regression test.

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
