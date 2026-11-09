# Final physics timing reduction

Validated 448 uninstrumented process receipts, 224 comparison rows, and 4 separately instrumented group-stat rows. Native Jolt runs were checked for U2/C8/I5/N80/Q2 and the full-update sleep clock. Provider markers and captured Engine CMake flags show timing profiling off. P50/P95/P99 below come from measured frames only; process comparisons use the median across separate processes. Matrix B/C comparisons align the same scene, cap, tail, and repeat across separate A/B and A/C cohorts; they are not directly counterbalanced B/C pairs. Group and Final-Guards trials are direct same-folder B/C pairs. Negative Δ means C took less time.

## 16K cloth, native Provider timing

| Scene | Cap | Tail | Pairs | B P50 ms | C P50 ms | Δ P50 | B P95 | C P95 | B P99 | C P99 | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cloth16k | W1 | cold | 2 | 187.88 | 189.29 | +0.76% | 195.76 | 193.27 | 198.13 | 194.19 | changed |
| cloth16k | W1 | warm | 2 | 186.73 | 190.54 | +2.04% | 192.35 | 199.38 | 194.11 | 199.69 | changed |
| cloth16k | W2 | cold | 2 | 128.57 | 112.08 | -12.82% | 130.19 | 116.83 | 130.79 | 117.47 | changed |
| cloth16k | W2 | warm | 2 | 126.54 | 108.17 | -14.48% | 131.32 | 115.40 | 131.66 | 116.09 | changed |
| cloth16k | W8 | cold | 2 | 67.73 | 55.37 | -18.24% | 72.58 | 57.76 | 72.85 | 58.18 | changed |
| cloth16k | W8 | warm | 2 | 67.99 | 55.88 | -17.80% | 71.95 | 58.70 | 72.43 | 58.87 | changed |

The target eight-worker cloth improves in both cold and warm aligned comparisons. One-worker cloth slows in both tails. Its changed state hash is expected from the opt-in 1024-vertex grouping and must be judged by the physical envelope.

## 16K cloth, full Engine physics-call timing

| Scene | Cap | Tail | Pairs | B P50 ms | C P50 ms | Δ P50 | B P95 | C P95 | B P99 | C P99 | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cloth16k | W1 | cold_process | 2 | 186.40 | 190.33 | +2.11% | 191.31 | 193.89 | 193.28 | 195.35 | unreported |
| cloth16k | W8 | cold_process | 2 | 68.57 | 56.22 | -18.01% | 73.06 | 58.67 | 74.15 | 59.09 | unreported |

Engine receipts do not report a trajectory hash; `unreported` is not a mismatch. Provider total and Engine physics-call timing have different executable and measurement boundaries. Their difference is not an Engine overhead estimate.

## Native Jolt across the other scenes

| Scene | Cap | Tail | Pairs | B P50 ms | C P50 ms | Δ P50 | B P95 | C P95 | B P99 | C P99 | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cloth1k | W1 | cold | 2 | 11.28 | 11.39 | +0.98% | 11.48 | 11.57 | 11.53 | 11.67 | same |
| cloth1k | W1 | warm | 2 | 11.25 | 11.45 | +1.73% | 11.29 | 11.83 | 12.32 | 11.87 | same |
| cloth1k | W2 | cold | 2 | 10.07 | 6.97 | -30.82% | 10.50 | 8.38 | 10.57 | 9.42 | same |
| cloth1k | W2 | warm | 2 | 9.46 | 7.76 | -18.11% | 9.81 | 8.66 | 9.84 | 8.85 | same |
| cloth1k | W8 | cold | 2 | 6.16 | 5.85 | -4.83% | 6.63 | 6.29 | 6.72 | 6.35 | same |
| cloth1k | W8 | warm | 2 | 5.97 | 5.78 | -3.15% | 6.47 | 6.22 | 6.74 | 6.38 | same |
| cloth4k | W1 | cold | 2 | 46.17 | 47.20 | +2.22% | 47.69 | 48.61 | 48.99 | 48.80 | same |
| cloth4k | W1 | warm | 2 | 46.20 | 46.61 | +0.90% | 47.83 | 47.92 | 48.58 | 48.97 | same |
| cloth4k | W2 | cold | 2 | 32.19 | 27.14 | -15.66% | 32.92 | 31.29 | 33.10 | 32.35 | same |
| cloth4k | W2 | warm | 2 | 32.02 | 28.00 | -12.56% | 32.76 | 31.18 | 32.94 | 31.88 | same |
| cloth4k | W8 | cold | 2 | 18.13 | 18.11 | -0.10% | 20.15 | 18.87 | 20.51 | 19.11 | same |
| cloth4k | W8 | warm | 2 | 18.49 | 18.17 | -1.70% | 19.90 | 18.78 | 19.99 | 18.90 | same |
| cloth4x4k | W1 | cold | 2 | 184.72 | 186.75 | +1.10% | 192.05 | 191.33 | 194.92 | 193.89 | same |
| cloth4x4k | W1 | warm | 2 | 185.00 | 186.23 | +0.67% | 188.23 | 190.10 | 192.81 | 192.62 | same |
| cloth4x4k | W2 | cold | 2 | 94.58 | 95.28 | +0.74% | 96.24 | 97.54 | 96.56 | 98.14 | same |
| cloth4x4k | W2 | warm | 2 | 94.66 | 95.45 | +0.83% | 96.68 | 97.27 | 97.43 | 98.47 | same |
| cloth4x4k | W8 | cold | 2 | 31.06 | 31.29 | +0.74% | 33.78 | 34.13 | 34.04 | 35.08 | same |
| cloth4x4k | W8 | warm | 2 | 32.52 | 30.96 | -4.59% | 34.95 | 33.33 | 35.32 | 33.94 | same |
| rubber16k | W1 | cold | 2 | 390.44 | 387.06 | -0.86% | 396.11 | 394.82 | 400.23 | 398.28 | same |
| rubber16k | W1 | warm | 2 | 390.63 | 388.23 | -0.61% | 397.92 | 398.04 | 401.56 | 400.62 | same |
| rubber16k | W2 | cold | 2 | 309.24 | 294.88 | -4.64% | 314.71 | 305.45 | 317.85 | 307.55 | same |
| rubber16k | W2 | warm | 2 | 309.27 | 296.98 | -3.97% | 313.89 | 307.57 | 315.13 | 309.22 | same |
| rubber16k | W8 | cold | 2 | 234.71 | 233.13 | -0.67% | 242.37 | 239.98 | 244.34 | 240.38 | same |
| rubber16k | W8 | warm | 2 | 235.47 | 232.56 | -1.23% | 245.41 | 239.15 | 247.53 | 239.68 | same |
| rubber4k | W1 | cold | 2 | 96.08 | 95.02 | -1.10% | 98.31 | 98.38 | 98.56 | 98.90 | same |
| rubber4k | W1 | warm | 2 | 95.54 | 94.58 | -1.01% | 96.88 | 98.10 | 97.71 | 99.22 | same |
| rubber4k | W2 | cold | 2 | 75.10 | 69.50 | -7.46% | 76.55 | 73.54 | 77.23 | 74.16 | same |
| rubber4k | W2 | warm | 2 | 74.59 | 69.30 | -7.10% | 76.05 | 73.85 | 76.74 | 74.67 | same |
| rubber4k | W8 | cold | 2 | 57.55 | 58.81 | +2.21% | 60.85 | 61.32 | 61.82 | 61.71 | same |
| rubber4k | W8 | warm | 2 | 57.14 | 56.96 | -0.32% | 60.13 | 59.28 | 61.96 | 59.92 | same |

## XPBD (A) timing control

| Scene | Cap | Tail | Pairs | B P50 ms | C P50 ms | Δ P50 | B P95 | C P95 | B P99 | C P99 | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cloth16k | W1 | cold | 2 | 4.83 | 4.67 | -3.25% | 4.99 | 4.97 | 5.04 | 5.08 | same |
| cloth16k | W1 | warm | 2 | 4.83 | 4.72 | -2.17% | 5.03 | 4.93 | 5.06 | 5.00 | same |
| cloth16k | W2 | cold | 2 | 4.83 | 4.64 | -3.96% | 4.98 | 4.86 | 5.00 | 4.88 | same |
| cloth16k | W2 | warm | 2 | 4.84 | 4.63 | -4.22% | 5.02 | 4.83 | 5.04 | 4.84 | same |
| cloth16k | W8 | cold | 2 | 4.77 | 4.66 | -2.21% | 4.99 | 4.90 | 5.14 | 5.00 | same |
| cloth16k | W8 | warm | 2 | 4.86 | 4.75 | -2.15% | 5.01 | 4.91 | 5.02 | 4.92 | same |
| cloth1k | W1 | cold | 2 | 0.53 | 0.53 | -0.02% | 0.66 | 0.67 | 0.80 | 0.83 | same |
| cloth1k | W1 | warm | 2 | 0.53 | 0.53 | -1.51% | 0.67 | 0.65 | 0.68 | 0.66 | same |
| cloth1k | W2 | cold | 2 | 0.53 | 0.53 | -0.84% | 0.74 | 0.66 | 0.78 | 0.77 | same |
| cloth1k | W2 | warm | 2 | 0.53 | 0.53 | +0.32% | 0.65 | 0.66 | 0.66 | 0.67 | same |
| cloth1k | W8 | cold | 2 | 0.53 | 0.53 | -0.02% | 0.67 | 0.65 | 0.79 | 0.69 | same |
| cloth1k | W8 | warm | 2 | 0.54 | 0.54 | -0.20% | 0.66 | 0.65 | 0.67 | 0.67 | same |
| cloth4k | W1 | cold | 2 | 2.19 | 2.20 | +0.24% | 2.43 | 2.41 | 2.46 | 2.45 | same |
| cloth4k | W1 | warm | 2 | 2.21 | 2.19 | -1.20% | 2.41 | 2.48 | 2.47 | 2.50 | same |
| cloth4k | W2 | cold | 2 | 2.23 | 2.21 | -1.01% | 2.40 | 2.45 | 2.47 | 2.47 | same |
| cloth4k | W2 | warm | 2 | 2.24 | 2.21 | -1.53% | 2.45 | 2.45 | 2.50 | 2.53 | same |
| cloth4k | W8 | cold | 2 | 2.23 | 2.22 | -0.51% | 2.48 | 2.45 | 2.53 | 2.48 | same |
| cloth4k | W8 | warm | 2 | 2.23 | 2.21 | -1.16% | 2.45 | 2.44 | 2.48 | 2.49 | same |
| cloth4x4k | W1 | cold | 2 | 8.57 | 8.59 | +0.18% | 8.75 | 8.85 | 8.90 | 8.89 | same |
| cloth4x4k | W1 | warm | 2 | 8.56 | 8.62 | +0.63% | 8.76 | 8.79 | 8.77 | 8.84 | same |
| cloth4x4k | W2 | cold | 2 | 4.50 | 4.45 | -1.14% | 4.82 | 4.85 | 5.05 | 4.97 | same |
| cloth4x4k | W2 | warm | 2 | 4.49 | 4.46 | -0.52% | 4.90 | 4.93 | 4.98 | 5.10 | same |
| cloth4x4k | W8 | cold | 2 | 2.28 | 2.26 | -0.92% | 2.43 | 2.44 | 2.43 | 2.45 | same |
| cloth4x4k | W8 | warm | 2 | 2.30 | 2.25 | -1.96% | 2.56 | 2.44 | 2.67 | 2.48 | same |
| rubber16k | W1 | cold | 2 | 9.76 | 9.68 | -0.89% | 9.94 | 10.28 | 9.95 | 10.31 | same |
| rubber16k | W1 | warm | 2 | 9.74 | 9.63 | -1.07% | 9.93 | 10.09 | 10.18 | 10.96 | same |
| rubber16k | W2 | cold | 2 | 9.78 | 9.71 | -0.64% | 10.07 | 10.00 | 10.17 | 10.19 | same |
| rubber16k | W2 | warm | 2 | 9.75 | 9.68 | -0.71% | 9.96 | 9.86 | 10.01 | 9.97 | same |
| rubber16k | W8 | cold | 2 | 9.78 | 9.65 | -1.30% | 10.00 | 9.95 | 10.19 | 10.04 | same |
| rubber16k | W8 | warm | 2 | 9.71 | 9.58 | -1.39% | 10.00 | 10.29 | 10.05 | 10.52 | same |
| rubber4k | W1 | cold | 2 | 4.87 | 4.88 | +0.12% | 5.06 | 5.07 | 5.14 | 5.20 | same |
| rubber4k | W1 | warm | 2 | 4.83 | 4.92 | +1.89% | 5.09 | 5.18 | 5.15 | 5.23 | same |
| rubber4k | W2 | cold | 2 | 4.87 | 4.90 | +0.65% | 5.04 | 5.06 | 5.21 | 5.19 | same |
| rubber4k | W2 | warm | 2 | 4.88 | 4.86 | -0.47% | 5.07 | 5.14 | 5.17 | 5.32 | same |
| rubber4k | W8 | cold | 2 | 4.89 | 4.88 | -0.08% | 5.04 | 5.08 | 5.05 | 5.14 | same |
| rubber4k | W8 | warm | 2 | 4.81 | 4.89 | +1.58% | 5.06 | 5.04 | 5.09 | 5.10 | same |

XPBD is shown as a timing control. The Provider suites explicitly mark `QualityEnvelopeProvided=false`; this table does not rank XPBD against Jolt.

## XPBD (A), full Engine physics-call timing

| Scene | Cap | Tail | Pairs | B P50 ms | C P50 ms | Δ P50 | B P95 | C P95 | B P99 | C P99 | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cloth16k | W1 | cold_process | 2 | 4.78 | 4.64 | -3.04% | 4.96 | 4.81 | 5.01 | 4.88 | unreported |
| cloth16k | W8 | cold_process | 2 | 4.73 | 4.62 | -2.37% | 4.99 | 4.85 | 5.09 | 4.93 | unreported |
| cloth1k | W1 | cold_process | 2 | 0.53 | 0.53 | -0.04% | 0.66 | 0.66 | 0.67 | 0.66 | unreported |
| cloth1k | W8 | cold_process | 2 | 0.53 | 0.53 | -0.59% | 0.67 | 0.65 | 0.67 | 0.66 | unreported |
| cloth4k | W1 | cold_process | 2 | 2.21 | 2.20 | -0.67% | 2.50 | 2.42 | 2.56 | 2.43 | unreported |
| cloth4k | W8 | cold_process | 2 | 2.24 | 2.16 | -3.26% | 2.43 | 2.42 | 2.44 | 2.47 | unreported |
| cloth4x4k | W1 | cold_process | 2 | 8.50 | 8.55 | +0.59% | 8.69 | 8.90 | 8.73 | 8.99 | unreported |
| cloth4x4k | W8 | cold_process | 2 | 2.25 | 2.24 | -0.44% | 2.45 | 2.41 | 2.51 | 2.54 | unreported |
| rubber16k | W1 | cold_process | 2 | 9.65 | 9.58 | -0.75% | 10.00 | 9.98 | 10.20 | 10.16 | unreported |
| rubber16k | W8 | cold_process | 2 | 9.68 | 9.60 | -0.74% | 9.89 | 9.96 | 9.98 | 10.14 | unreported |
| rubber4k | W1 | cold_process | 2 | 4.80 | 4.86 | +1.22% | 5.05 | 5.06 | 5.08 | 5.18 | unreported |
| rubber4k | W8 | cold_process | 2 | 4.89 | 4.84 | -0.96% | 5.14 | 5.00 | 5.24 | 5.04 | unreported |

## Native Provider versus full Engine boundaries

| Source | Scene | Cap | Provider tail | Provider total P50 ms | Engine physics-call P50 ms |
| --- | --- | --- | --- | --- | --- |
| B | cloth16k | W1 | cold | 187.88 | 186.40 |
| C | cloth16k | W1 | cold | 189.29 | 190.33 |
| B | cloth16k | W8 | cold | 67.73 | 68.57 |
| C | cloth16k | W8 | cold | 55.37 | 56.22 |
| B | cloth1k | W1 | cold | 11.28 | 11.32 |
| C | cloth1k | W1 | cold | 11.39 | 11.42 |
| B | cloth1k | W8 | cold | 6.16 | 6.14 |
| C | cloth1k | W8 | cold | 5.85 | 6.11 |
| B | cloth4k | W1 | cold | 46.17 | 46.71 |
| C | cloth4k | W1 | cold | 47.20 | 46.60 |
| B | cloth4k | W8 | cold | 18.13 | 18.46 |
| C | cloth4k | W8 | cold | 18.11 | 17.72 |
| B | cloth4x4k | W1 | cold | 184.72 | 185.32 |
| C | cloth4x4k | W1 | cold | 186.75 | 186.93 |
| B | cloth4x4k | W8 | cold | 31.06 | 31.09 |
| C | cloth4x4k | W8 | cold | 31.29 | 31.01 |
| B | rubber16k | W1 | cold | 390.44 | 392.93 |
| C | rubber16k | W1 | cold | 387.06 | 387.58 |
| B | rubber16k | W8 | cold | 234.71 | 236.75 |
| C | rubber16k | W8 | cold | 233.13 | 232.52 |
| B | rubber4k | W1 | cold | 96.08 | 95.83 |
| C | rubber4k | W1 | cold | 95.02 | 95.21 |
| B | rubber4k | W8 | cold | 57.55 | 57.85 |
| C | rubber4k | W8 | cold | 58.81 | 57.41 |

## Worker-cap scaling, native Jolt warm runs

| Source | Scene | W1 P50 ms | W8 P50 ms | W1/W8 speed ratio |
| --- | --- | --- | --- | --- |
| B | cloth16k | 186.73 | 67.99 | 2.75 |
| C | cloth16k | 190.54 | 55.88 | 3.41 |
| B | cloth1k | 11.25 | 5.97 | 1.89 |
| C | cloth1k | 11.45 | 5.78 | 1.98 |
| B | cloth4k | 46.20 | 18.49 | 2.50 |
| C | cloth4k | 46.61 | 18.17 | 2.57 |
| B | cloth4x4k | 185.00 | 32.52 | 5.69 |
| C | cloth4x4k | 186.23 | 30.96 | 6.02 |
| B | rubber16k | 390.63 | 235.47 | 1.66 |
| C | rubber16k | 388.23 | 232.56 | 1.67 |
| B | rubber4k | 95.54 | 57.14 | 1.67 |
| C | rubber4k | 94.58 | 56.96 | 1.66 |

These are process P50 speed ratios between worker caps, not CPU utilization.

## Admission, retirement, CPU, and working set (full matrix cohorts)

| Source | Scene | Cap | Tail | Admission ms | Teardown ms | Run CPU s | Whole CPU s | Receipt wall s | Approx cores lower bound | Peak WS MiB | Post-retire WS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B | cloth16k | W1 | warm | 51.17 | 0.52 | 7.469 | 7.531 | 7.586 | 0.99 | 21.42 | 12.94 |
| C | cloth16k | W1 | warm | 18.09 | 0.56 | 7.617 | 7.641 | 7.726 | 0.99 | 21.54 | 13.01 |
| B | cloth16k | W8 | warm | 52.23 | 0.96 | 21.664 | 21.727 | 2.828 | 7.68 | 21.52 | 13.08 |
| C | cloth16k | W8 | warm | 18.11 | 0.96 | 17.461 | 17.477 | 2.273 | 7.69 | 21.93 | 13.27 |
| B | rubber4k | W1 | warm | 48.25 | 0.40 | 3.828 | 3.875 | 3.921 | 0.99 | 17.92 | 12.93 |
| C | rubber4k | W1 | warm | 8.15 | 0.41 | 3.797 | 3.805 | 3.851 | 0.99 | 18.67 | 12.85 |
| B | rubber4k | W8 | warm | 47.96 | 0.71 | 18.086 | 18.141 | 2.383 | 7.61 | 18.08 | 13.13 |
| C | rubber4k | W8 | warm | 8.52 | 0.79 | 17.969 | 17.969 | 2.320 | 7.75 | 18.86 | 13.06 |

`Run CPU` spans admission-end through run-end, including warmup and quality/logging. It is not measured-frame CPU utilization. `Approx cores lower bound` uses captured whole-process CPU divided by receipt wall time, which includes more startup/shutdown than the CPU scope. Peak working set is process-lifetime and does not prove a phase-local peak or leak. Exact receipt wall seconds are in `Rows.csv/json`.

## Isolated experiments

| Scope | Scene | Cap | Pairs | Control P50 ms | Candidate P50 ms | Δ P50 | Range Δ | All faster | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Final-Guards | cloth16k | W8 | 3 | 70.13 | 55.83 | -20.38% | -21.11% to -19.55% | yes | changed |
| Group1024 | cloth16k | W1 | 3 | 188.60 | 190.52 | +0.89% | +0.53% to +2.33% | no | changed |
| Group1024 | cloth16k | W8 | 3 | 70.53 | 56.36 | -20.62% | -20.93% to -18.85% | yes | changed |
| Group512 | cloth16k | W1 | 3 | 189.34 | 188.77 | -0.32% | -0.49% to +0.07% | no | changed |
| Group512 | cloth16k | W8 | 3 | 70.41 | 69.12 | -2.45% | -2.60% to -1.82% | yes | changed |
| Lazy-Rubber-Confirmation | rubber4k | W8 | 5 | 58.14 | 57.69 | -1.56% | -5.88% to +0.74% | no | same |
| Lazy-Trial | cloth16k | W1 | 3 | 188.76 | 188.66 | -0.31% | -1.21% to +0.67% | no | same |
| Lazy-Trial | cloth16k | W8 | 3 | 69.24 | 67.57 | -2.20% | -2.74% to -1.65% | yes | same |
| Lazy-Trial | rubber4k | W1 | 3 | 95.51 | 94.56 | -1.05% | -1.76% to +0.19% | no | same |
| Lazy-Trial | rubber4k | W8 | 3 | 57.43 | 58.69 | +2.19% | -4.32% to +3.00% | no | same |

### Final guarded candidate, direct B/C pairs

| Repeat | B P50 ms | C P50 ms | Δ P50 | B P95 ms | C P95 ms | B P99 ms | C P99 ms | State |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| R1 | 69.74 | 56.11 | -19.55% | 74.14 | 57.82 | 75.18 | 57.94 | changed |
| R2 | 70.30 | 55.46 | -21.11% | 76.32 | 57.79 | 76.99 | 58.91 | changed |
| R3 | 70.13 | 55.83 | -20.38% | 78.36 | 57.71 | 79.40 | 59.80 | changed |

| Group trial | Update groups | Parallel edges | Serial edges |
| --- | --- | --- | --- |
| Group1024 | 17 | 93216 | 3810 |
| Group512 | 33 | 89620 | 7406 |

`Lazy-Trial` isolates lambda clearing at group size 256. The compiled 1024-group setting in `Lazy-Rubber-Confirmation` applies only to cloth with at least 16K vertices; rubber4k uses the original 256-group path in both binaries. Its five pairs therefore test the lazy-clear source on rubber without a group-order change; the signs remain mixed. Group512/1024 cloth trials change trajectory hashes. The `C-Long` physical evidence is separate and must decide whether that changed trajectory is acceptable.

## Observed regressions and decision boundary

| Benchmark | Scene | Cap | Tail | Aligned Δ P50 range |
| --- | --- | --- | --- | --- |
| engine | cloth16k | W1 | cold_process | +1.87% to +2.34% |
| engine | cloth1k | W1 | cold_process | +0.47% to +1.39% |
| engine | cloth4x4k | W1 | cold_process | +0.67% to +1.06% |
| provider | cloth16k | W1 | cold | +0.17% to +1.34% |
| provider | cloth16k | W1 | warm | +1.93% to +2.15% |
| provider | cloth1k | W1 | cold | +0.58% to +1.37% |
| provider | cloth1k | W1 | warm | +1.50% to +1.97% |
| provider | cloth4k | W1 | cold | +0.62% to +3.82% |
| provider | cloth4x4k | W1 | cold | +0.79% to +1.41% |
| provider | cloth4x4k | W1 | warm | +0.63% to +0.70% |
| provider | cloth4x4k | W2 | cold | +0.25% to +1.24% |
| provider | cloth4x4k | W2 | warm | +0.82% to +0.83% |

A positive range means every aligned same-cell/repeat comparison in that small timing sample was slower; it is not a statistical confidence interval. The 1024-group candidate's 16K-cloth W8 result is substantial, while single-worker cloth and some small-scene configurations regress. The full physical quality envelope, including preserved negative controls, is required before any adoption or cross-backend winner. All per-process P50/P95/P99, CPU/working-set, admission, retirement, hashes, and receipt identifiers remain in the CSV and JSON files. Separately instrumented native scopes and exact 40-frame prefix checks are in `ProfileComparison.md/json`; those scopes do not rank frame timing.

