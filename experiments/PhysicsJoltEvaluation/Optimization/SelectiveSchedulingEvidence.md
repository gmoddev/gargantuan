---
status: experimental-measured
authority: non-normative
date: 2026-11-08
---

# Selective coupling scheduling: sparse and gravity-loaded scenes

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

The first sparse scene keeps all 128 unrelated bodies active and its maximum
pair penetration is only `6.1e-6 m`. It has no gravity-loaded floor or
stack. That first build did not count rigid contact callbacks, so penetration
alone cannot establish how often its pairs generated contacts. Median
live-world working set is roughly 0.4–0.43 MiB higher for
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

## Gravity-loaded contact scene

Hosted run [42469731651](https://github.com/gmoddev/gargantuan/actions/runs/42469731651)
built Engine `88d34466e7c0ac386383c7d84e253754da097e28` against the same
stock and maintained Jolt commits on Linux and Windows. Each platform ran
both sparse and dense layouts, 54 fresh processes and 24 paired comparisons
per layout. All 216 processes exited 0. All raw stdout/stderr byte SHA-256
digests match their receipts, every parsed first line matches raw stdout,
all 24 near/crossing/joint refusal controls passed, and stock and maintained
benchmark quality flags passed in both layouts. Within each backend, the
coupled trajectory hash is exact across layouts, modes, repeats, worker
budgets, and platforms. The maintained hash is
`18420796733227315746`; stock's distinct hash is
`4319684646025306226`.

The dense layout adds a static isolated floor and gravity-driven 128-sphere
stack; authored collision layers still exclude any cross-partition contact.
The coupled soft and jointed rigid bodies have zero gravity factor in both
modes and retain U2/C8/I12. The isolated world uses U1/C4 in selective mode.
Rigid contact callbacks occur in 120/120 frames in both modes. At one
worker, dense global reports 402 added and 409,917 persisted isolated rigid
callbacks; selective reports 420 added and 104,093 persisted callbacks.
These callback counts depend on collision cadence and are not comparable
per-contact work units. Dense selective maximum pair/floor penetration is
`0.012055/0.006743 m`, versus global `0.000155/0.000206 m` on both
platforms. The isolated rigid trajectories change with cadence; both stay
under the unchanged `0.25 m` gate. Initial authored pair/floor gaps are
`0.01/0.10 m`, so the gate is not passed by starting deeply overlapped.

Unprofiled maintained-Jolt paired costs use the same four fresh-process
median method as above. Sparse is repeated at the new source identity, so
its figures are kept separate from the earlier d238 checkpoint.
The 88d sparse rerun now counts rigid contact callbacks: both modes have
contacts in 120/120 frames, with 353,816 persisted callbacks globally and
88,280 selectively at one worker. Tiny penetration did not imply an absence
of contact; sparse describes the lack of gravity and floor loading.

| Platform | Layout | Workers total | Global → selective, ms/120 frames | Paired speedup | Median frame P95, ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Linux | sparse | 1 | 497.50 → 369.61 | 1.347× | 4.18 → 3.12 |
| Linux | dense | 1 | 681.14 → 420.22 | 1.621× | 6.22 → 3.65 |
| Linux | dense | 2 | 673.35 → 431.95 | 1.560× | 6.15 → 3.78 |
| Linux | dense | 8 | 937.20 → 662.34 | 1.414× | 8.67 → 5.86 |
| Windows | sparse | 1 | 345.72 → 248.28 | 1.393× | 2.91 → 2.12 |
| Windows | dense | 1 | 472.98 → 283.93 | 1.667× | 4.33 → 2.48 |
| Windows | dense | 2 | 467.64 → 294.85 | 1.592× | 4.29 → 2.59 |
| Windows | dense | 8 | 611.45 → 406.96 | 1.505× | 5.85 → 3.64 |

At one worker, adding gravity and the floor costs 183.64 ms more per
120 frames in the Linux global world but 50.61 ms more in the selective
pair; Windows adds 127.26 ms globally and 35.66 ms selectively. These
within-platform deltas show that the denser isolated load is where the
split captures more time, while the coupled trajectory stays identical.

The profiler again attributes the dense gain mainly to rigid work while
keeping soft work: at T1, Linux inclusive narrowphase scopes fall
`480.2 → 143.8 ms`, broadphase `65.3 → 20.1 ms`, and position solve
`110.1 → 34.6 ms`; SoftSimulate remains `596.7 → 592.9 ms`. Windows
corresponding figures are `207.4 → 56.6`, `44.4 → 13.7`,
`81.7 → 23.0`, and `246.1 → 244.5 ms`. These profiled scope values are
nested, instrumentation-inflated work measurements and cannot be summed
or used as the unprofiled cost table. The 88d run predates soft-subscope
classification and does not contain that breakdown.

The additional world costs roughly 0.38–0.43 MiB of live working set in
the dense T1 processes and about 0.14–0.23 ms of setup in the median
processes. These are process snapshots with live worlds, not peak-retention
or teardown leak measurements. The result demonstrates scheduling gain for
an authored no-cross collision partition. It does not implement or qualify
general interacting rigid-island cadence, arbitrary cross-partition joints,
or possible newly forming cross-partition contacts. The different isolated
rigid penetration and callback counts must remain visible in any cost claim.

Dense and repeated-sparse evidence is preserved under
`C:/Users/aiden/.codex/artifacts/jolt-optimized-foundation-20261108/Selective-Dense-Linux-88d`
and
`C:/Users/aiden/.codex/artifacts/jolt-optimized-foundation-20261108/Selective-Dense-Windows-88d`.
