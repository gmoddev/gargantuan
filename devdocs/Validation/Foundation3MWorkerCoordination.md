# Foundation 3M worker coordination

This campaign owns trusted default-space residency demand policy in branch
`foundation/3m-trusted-residency`, based on completed 3L merge `81fca069c`.
Its local worktree is `C:\Users\aiden\.codex\worktrees\foundation-3m-trusted-residency`.

Worker namespace on SSH host `dockerbox`:

- Source: `C:\Sandbox\Codex\Workspaces\foundation-3m-trusted-residency`.
- Build: `C:\Sandbox\Codex\Builds\foundation-3m-trusted-residency\msvc-release`.
- Logs and artifacts: matching campaign directories under
  `C:\Sandbox\Codex\Logs` and `C:\Sandbox\Codex\Artifacts`.
- At most two compile jobs. No unrelated workload or cache may be stopped,
  changed or deleted. Existing compatible dependency caches may be read/reused.

Asset 2B / Geometry 1A uses separate worktrees and worker namespace
`asset-geometry-20261104`, with at most two compile jobs. Shared CMake changes
are additive registrations. Neither campaign merges the other's unfinished work.
Mesh collision, Boolean compilation, physics shape semantics and collision
artifact contracts remain outside 3M scope.
