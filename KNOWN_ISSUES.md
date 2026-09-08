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

## KI-006: Content-coupled gameplay latency exceeds the 3L.2 readiness envelope

- Status: Open; Foundation 3M remains gated.
- Priority: High
- Area: Structural materialization and gameplay latency under peer scale.
- Evidence: [Foundation 3L.2 measurements](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_2.md).
- Relevant code: `src/network/ReplicationCoordinator.cpp`,
  `src/network/ReplicaApplier.cpp`, `tests/GameSessionBenchmark.cpp`.

The candidate 32-peer 512-object workload converges within the existing 3J cap,
but RemoteFunction p99 rises from about 26 ms without the streamed region to
about 382 ms with it Resident. A 100-peer run similarly converges but records
one owner-action submission failure during reload. The first grouped 500-peer
fixture missed client-code hydration before the workload Script materialized;
that attempt produced zero RPC samples and is not an RPC failure. Establishing
the gameplay client first exposed a separate full-rate receive overload:
500 input commands/tick exceed the official host's one 128-event Poll call/tick,
and reliable gameplay traffic is disconnected when the simulated queue fills.
An explicitly staggered 12 Hz input profile is being measured separately, without
raising the host, transport, or 3J limits. These are measured
performance/availability failures, not evidence that the previously fixed
RemoteFunction access violation has returned. Concurrent bounded build activity
is recorded and isolated confirmation is still required.

Resolution requires attributed baseline/resident/streaming measurements, healthy
owner/action/root-motion and Remote traffic, and clean 32/100/500 convergence
without raising limits, bypassing 3E/3J, or weakening replica validation. Full
semantic snapshot validation during incremental replica application is an
inspection lead, not a proven exclusive root cause.

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
