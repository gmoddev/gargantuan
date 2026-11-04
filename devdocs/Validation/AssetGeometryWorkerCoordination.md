# Asset 2B / Geometry 1A worker coordination

Integration branch: `feature/asset2b-geometry1a`, based on fetched main
`e682be08df84d776bfa455137728265c6494b5e8`. This includes completed Foundation
3L merge `81fca069c1a0bf2aad1aa10eaf480260f5137293`; relevant asset and physics
sources are unchanged between those two references before this campaign.

This campaign uses separate source/build/log/artifact directories named
`asset-geometry-20261104` under `C:\Sandbox\Codex` on trusted SSH host
`dockerbox`. Native compilation is capped at two jobs. The concurrent 3M campaign
owns `foundation-3m-trusted-residency` and independently caps its build at two jobs.
The combined compile load is four jobs on the observed 24-logical-CPU worker.
Retained timing windows require a quiet worker; overlapping build observations
must be labeled and cannot support comparative performance claims.

Existing compatible vendor inputs may be borrowed read-only. New Manifold sources
and all writable build state remain in this campaign's namespace. No old build
tree, cache, worktree, network setting or worker service is reset or purged.
Source manifests, recipe hashes, compiler profiles and stdout/stderr/exit codes
are retained with qualification receipts.

The worker Docker CLI did not respond to a read-only probe. Only that verified
task-owned probe and its session were retired; unrelated old Docker probes were
preserved. No Docker Desktop repair or unchanged retry campaign was started.
Linux qualification uses an independently inspected existing trusted environment
and records its actual toolchain/image, rather than relabeling Windows results.
The inspected Linux environment lacks glslc. Its explicit local headless geometry
profile builds real native core/test targets without shader tooling; graphical
and packaged targets fail closed. This narrower sanitizer qualification is not
the hosted production Linux job or shader/runtime package qualification.

Shared Engine changes are additive CMake registrations and the documented asset/
physics boundaries. This campaign does not implement or modify 3M demand policy,
package content queries, hysteresis, lookahead or 3L lifecycle/networking. Neither
campaign merges the other's unfinished source. This document reserves no resources
after the active campaign's processes have actually retired.

Strict Linux sanitization exposed the pinned Box3D SIMD vector load's unaligned
`double*` access. The compatibility recipe verifies the original header hash and
builds a private corrected source copy inside this campaign's build directory.
Borrowed vendor source, public headers and the pinned submodule remain unchanged.
The unknown-source guard refuses an unrecognized header rather than guessing at
a patch. No sanitizer category is disabled to obtain a passing result.

Windows compilation paused for the separate Studio profiling reservation. An
already-started task-owned regression build was retired at 05:48:16 UTC on
November 4; the preceding overlapping interval is excluded from timing claims.
Studio subsequently released its reservation explicitly. The 3M campaign then
completed its own two-job build, tests and short benchmark, reporting retirement
at approximately 06:05:22 UTC. Only then was Source12 Windows qualification
released. These reservations coordinate worker load, not source ownership.
