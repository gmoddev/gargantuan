# Play input inspection qualification — 2026-11-03

The implementation is based on the explicitly unmerged Input Action foundation
`60baf9c61437cd23fbc957531466122013aa4e70`. It is a separate additive branch,
`feature/editor-input-inspection`; nothing is assumed present on `main`.

## Qualified execution

Windows x64 MSVC 14.44 Release, GNS disabled, on the trusted DockerPC worker:

- Focused InputAction/Foundation/Player tests: **3/3 PASS**.
- Incremental configured build and complete existing matrix: **54/54 PASS**, 46.26 s.
- Final executable SHA256:
  `00A8595BF1C0F0A6209C76F27AC5137D8E3B2F537C0013C77544669A725ABBF3`.
- Source: original foundation plus the exact four changed code files
  `EditorHost.cpp`, `PlaySession.cpp`, `PlaySession.hpp`, `InputActionTests.cpp`;
  worker pre/post source fences matched the candidate. Documentation does not
  contribute to the executable.

Workspace `C:/Sandbox/Codex/Workspaces/gargantuan-input`, incremental build
`C:/Sandbox/Codex/Builds/gargantuan-input/msvc1444-release`; final receipts under
`C:/Sandbox/Codex/Artifacts/LUI-Primary-20261103/Engine-Input-Inspection-03`.
The prior source preimages and qualified dependencies were preserved. Compilation
used at most four jobs; CTest used two. Existing vendor/generated caches were
retained. Per-process error-mode and dummy SDL drivers prevented native modal
error reporting. No machine-wide settings or production deployment changed.

Reproduce by synchronizing these four code files over the recorded foundation
export (checking its baseline/generated manifests), then building the configured
tree with `cmake --build <build> --parallel 4` and running `ctest --test-dir <build>
--output-on-failure --parallel 2`. The original Input Action validation contains
the toolchain/cache configuration and full test runner. Never substitute an
arbitrary Engine executable into Studio's source/binary pin table.

## Behavior and independent review

Tests cover exact session/capability/field rejection; runtime/authoring identity
isolation; persisted chord definitions; Ctrl+Shift+S through the existing physical
pipeline; owning-copy semantics; focus cancellation; evaluator bounds;
Stop/restart; unchanged authoring revision/dirty/history; exact active evaluator
ownership through detach/replacement/destruction; and no service construction
during inspection. Independent review found and corrected hierarchy lookup of
the evaluator and an invalid test detachment fixture before final qualification.

The IPC test reproduced an existing key admission defect: enum reflection's
default upper bound excluded modifiers/navigation. Explicit ranges now retain
the declared vocabulary, and uint16 bounds are checked before casts. Tests reject
enum holes and values that would otherwise wrap to accepted keys.

No networking, replication authority, Foundation 3L, Character admission or
authoring transaction contract was changed. A client action remains untrusted
server input. GNS-enabled hosted CI, physical keyboard/IME/device behavior and
Studio native UX are separate gates; this Windows GNS-disabled headless result
does not qualify them. The Studio adapter independently consumes the public
contract and owns its own integration/performance/native receipts.

## Continuation

Studio must check `PlayInputInspection`, send one exact active PlaySessionId and
discard old session/scope responses. It can author Input Instances through the
ordinary schema/creation/property/history APIs. It must not map runtime IDs to
authoring resources by name or persist them as document bindings. The exact DTO
and lifecycle rules are in [PlayInputInspection.md](PlayInputInspection.md).
Subsequent work includes source-associated validation errors, broader physical
input qualification and separately scoped gamepad/vector/touch authoring.
