# Runtime Foundation 6 qualification

Status: implementation and comparison in progress. This isolated continuation
starts from qualified Foundation 5 head
`da902ea481e627d7c4d789033076b3be7dc38334`, stacked on `foundation/renderer-5`.
Main remains `5b6782e35c4e24fd80bc8d5526104c615c72d84d` at reconciliation.
PR 21 and PR 22 remain separate, open draft foundations. No Node, Studio or LUI
source belongs to this campaign.

## Baseline and comparison contract

The first checkpoint changes benchmark fixtures and CMake only. It preserves
the Foundation 5 runtime, so candidate runtime changes can be measured against
the identical fixture before promotion. Existing Foundation 5 raw evidence is
retained in `C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111`.
New evidence belongs to sibling `runtime-foundation6-20261111`.

`gargantuan_runtime_foundation6_benchmark` extends the existing semantic fixture.
`--count=N --density-matrix` constructs one world, then measures 0%, 0.1%, 1%,
10%, 50% and 100% changes without reconstructing that world between densities.
`--split-construction --diagnostic-marks` separates world setup, group setup,
Part allocation, detached transform initialization, adoption, initial publication,
measurement, semantic destruction and final object/resource release.
Every measured frame checks the exact journal sequence delta, projected update
count, no create/remove/resync, and retained object count. Ordinary native body
admission is the default. The explicitly tagged renderer-only mode remains a
controlled candidate-list fixture, not a physics admission test.

Examples, each with a fresh raw sample path:

```text
gargantuan_runtime_foundation6_benchmark 12 --count=10000 --density-matrix --group-width=256 --split-construction --diagnostic-marks --samples=<fresh.csv>
gargantuan_runtime_foundation6_benchmark 12 --count=10000 --density-matrix --property=material --group-width=256 --split-construction --diagnostic-marks --samples=<fresh.csv>
```

`--property=mixed` measures both transform and material changes. `--writes=N`
performs distinct repeated writes while publication coalesces final object state;
the journal must still contain every required ordinary mutation. Raw frame rows
and invocation receipts preserve count, writes, property mode, density, native
physics state, group layout and profiling state. Profile clocks are attribution,
not a performance candidate. A separate allocation binary counts calling-thread
C++ new requests; C allocations, driver allocations and other threads are outside
that metric. Full process memory is a separate observation.

## Competing experiments

| Workstream | Candidate / gate | Present state |
| --- | --- | --- |
| Ordinary mutation/lifecycle | Retained journal append/rollback, unobserved signal allocation; preserve nested/destructive observers and exact journal/history | Source candidate, native comparison pending |
| Explicit batching | Existing prepared-property contract; atomic observer visibility differs from ordinary setters and remains explicit | Semantic tests and comparison candidate |
| Dirty distribution | Single-object journal batch unions renderer-final-state domains; reliable records remain uncoalesced | Source candidate, capture/ack parity tests pending |
| Typed extraction | Reuse validated identity/ancestry; exact-model inverse reuse; full final fields retained | Source candidate, native comparison pending |
| Storage alternatives | Direct Instance extraction versus synchronized generation-safe SoA versus immutable per-object snapshots | Component probe, unqualified until native runs |
| Physics synchronization | Constant-time admission membership and scoped physics-authored callback suppression | Source candidate, correctness tests pending |
| Compact physics view | Derived dynamic readback and ordered pending controls with conservative full-scan fallback | Separate experiment; sparse/dense/churn comparison pending |
| Replication / F1 | Existing 3I typed catalog and 3J accepted per-peer history remain owners | Source review; no network behavior change justified yet |

The alternatives probe includes actual authoritative setters, dirty capture,
cache synchronization and consumer work. It verifies exact dirty identities,
incremental retained-state parity, stale generation retirement and full rebuild.
It does not qualify physics, networking, asset-backed MeshParts or production
capacity from primitive extraction alone.

## Ownership and admission

Instance/DataModel remain authority on Main. Ordinary callbacks remain
synchronous. Render-final-state coalescing is distinct from reliable journal
delivery and explicit prepared authoring batches. No alternative becomes an
authoritative writer or exposes native storage to Luau.

The shared worker was occupied by `LuiSdkRecovery20261111` during initial source
work. No compilation or benchmark was admitted under that lease. Component
builds may reuse this campaign's validated Foundation 5 source/build caches only
after a fresh cooperative lease and explicit ownership handoff; frozen Foundation
5 packages remain immutable. Full networking/F1 and cross-platform qualification
use the existing canonical suites and acceptance contracts, without new targets
or exemptions. No source candidate is qualified by this document alone.
