---
status: experimental-measured
authority: non-normative
date: 2026-11-08
---

# Selective coupling scheduling: first measured sparse scene

Source-bound hosted run [42468561346](https://github.com/gmoddev/gargantuan/actions/runs/42468561346)
built Engine `d238b4fb21f40ec9bbb504f70acdcca87590f1b3`, stock Jolt
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`, and maintained Jolt
`109be79fa82be0b306c56e4b4357675ed7843767` separately on Linux and
Windows. The benchmark uses the same 13×13 cloth, two joint-connected coupled
rigids, and 128 active unrelated rigid spheres in both modes. The authored
collision layers exclude cross-partition contact in *both* modes. `global`
advances one world at U2/C8/I12; `selective` advances the coupled world at
U2/C8/I12 and the isolated rigid world at U1/C4. Both share one job pool and
temporary arena in the selective process.

The reduction independently checked 54 fresh-process receipts and 24 matched
pairs per platform: all exit 0, all six near/crossing/joint controls refuse
before update, and every preserved stdout first line matches its parsed
receipt. For each backend, the coupled trajectory hash is exact across global
and selective modes, repeats, worker budgets 1/2/8, and Linux/Windows. Stock
and maintained solver hashes differ, so no cross-solver physical equivalence
is inferred. Both report 22 actual soft-contact callbacks, frame-finite
states, edge stretch below 0.1, and isolated pair penetration below 0.25 m.
The benchmark still requires the separate full internal energy/contact
qualification; these screens do not replace it.

Unprofiled maintained-Jolt cost below is the median across four fresh
processes of each 120-frame sum of `TotalUpdateMs + GuardMs`. The ratio is the
median of the four paired global/selective ratios. This is native simulation
plus the partition guard, not an Engine frame or provider lifecycle cost.

| Platform | Workers total | Global → selective, ms/120 frames | Paired speedup | Median per-process frame P95, ms |
| --- | ---: | ---: | ---: | ---: |
| Linux | 1 | 449.70 → 341.29 | 1.317× | 3.77 → 2.87 |
| Linux | 2 | 529.64 → 388.71 | 1.364× | 4.48 → 3.31 |
| Linux | 8 | 646.06 → 538.30 | 1.197× | 5.52 → 4.62 |
| Windows | 1 | 418.02 → 300.56 | 1.390× | 3.52 → 2.55 |
| Windows | 2 | 475.16 → 347.80 | 1.370× | 4.18 → 3.01 |
| Windows | 8 | 623.05 → 531.25 | 1.172× | 5.40 → 4.67 |

The separately profiled one-worker build shows where this improvement came
from. Inclusive `SoftSimulate` scope work is effectively unchanged: Linux
405.6 → 405.8 ms and Windows 307.6 → 307.3 ms over 120 frames. `Narrowphase`
scope work falls Linux 178.9 → 46.5 ms and Windows 167.9 → 43.5 ms;
`Broadphase` work also falls 37.2 → 11.0 ms and 33.0 → 9.9 ms. The second
world adds one update per frame (240 → 360 `BuildJobs` scopes), while the
coupled soft path still runs 192 internal integrations per frame. Profile
scope times nest and overlap; their totals cannot be summed or compared as
unprofiled wall time.

The sparse scene keeps all 128 unrelated bodies active, but its maximum
pair penetration is only `6.1e-6 m`; it is not a sustained dense-contact
stress. Median live-world working set is roughly 0.4–0.43 MiB higher for
selective mode on each platform, consistent with an extra `PhysicsSystem`,
without proving retained memory or a leak. `SceneSetupMs` is reported
separately and is roughly 0.15–0.2 ms higher at one worker; quality extraction
is measured separately and excluded from the table. The original selected
U2/C8/I12 correction and production Box3D + XPBD defaults are unchanged.

Preserved evidence is under
`C:/Users/aiden/.codex/artifacts/jolt-optimized-foundation-20261108/Selective-Linux-d238`
and
`C:/Users/aiden/.codex/artifacts/jolt-optimized-foundation-20261108/Selective-Windows-d238`,
including source identities, CMake caches,
profile logs, binary hashes, raw output/stderr, incremental receipts, and
pair reductions.
