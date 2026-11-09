---
status: experimental-measured
authority: non-normative
date: 2026-11-09
---

# Jolt soft-body admission cost and bounded reuse

This note covers the private unified-Jolt provider. The production Box3D + XPBD
factories and defaults are unchanged. Admission uses the same semantic
`SoftBodyDefinition`, native Jolt body state, and neutral generation-safe IDs as
the qualified provider.

## Baseline attribution

The dedicated instrumented benchmark constructs one 128×128 cloth body in a
fresh process, destroys it, then repeats twice in the same world. It separately
snapshots tracked allocations for world setup, per-body construction, initial
state extraction, body retirement, and world retirement. The backend measures
topology generation, validation/precompute, `SoftBodySharedSettings::Optimize`,
native body creation, and slot commit. Allocation hooks include C++ `new` plus
Box3D/Jolt hooks; they exclude other `malloc`, allocator metadata, OS, and
library storage. Process peaks are lifetime peaks, not phase-local peaks.
Instrumentation timings are diagnostic and must not be compared with ordinary
timing targets.

| Fresh process | Jolt first body | Jolt Optimize | Optimize allocations | XPBD first body |
| --- | ---: | ---: | ---: | ---: |
| R1 | 56.00 ms | 53.59 ms | 163,642 | 2.10 ms |
| R2 | 58.85 ms | 56.29 ms | 163,642 | 1.99 ms |
| R3 | 56.71 ms | 54.19 ms | 163,642 | 2.00 ms |

The first Jolt body in R1 has 208 topology-stage allocations and 4 native
creation-stage allocations; the graph optimization accounts for almost all
163,857 tracked creation calls. In the four-body R1 cohort, each independent
cloth body costs 55.66–56.76 ms and adds about 5.07 MiB tracked live bytes;
destroying each returns about 5.06–5.07 MiB. World setup takes 0.77–0.89 ms in
the fresh runs and holds 60.61 MiB tracked live bytes. That figure includes
the explicit 32 MiB Jolt temporary arena plus world reservations; resident
working set is a different measure. The R1 full-run post-retirement net is
6,942 tracked bytes, including process/global allocations, and does not prove
absence of leaks. The baseline raw JSONL receipts are preserved under
`Baseline-Profile-20261109T053701` in the campaign artifact set.

The isolated contiguous-graph candidate was profiled separately from the
provider cache. Its three fresh 16K first-body admissions took 42.67, 42.81,
and 44.93 ms; their `Optimize` phases took 40.30, 40.38, and 42.53 ms.
Tracked `Optimize` allocation calls fell from 163,642 to 822 (99.5% fewer),
and requested bytes fell from 10.20 to 6.27 MB (decimal). The four-body
first admissions took 42.70, 42.63, and 43.02 ms across fresh runs. These
are instrumented diagnostics, not ordinary timing ranks. The graph still
costs about 40 ms, so the change does not make cold 16K admission inexpensive.
The candidate raw JSONL receipts are preserved under
`CSR-Profile-20261109T054420`; its one-body post-retirement tracked net is
the same 6,942 bytes as baseline. Uninstrumented results and behavior parity
remain a separate gate.

## Candidate ownership and bounds

The native candidate changes only the temporary graphs used by Jolt
`SoftBodySharedSettings::Optimize` and `CalculateClosestKinematic`. It counts
per-vertex incident constraints and stores neighbors in contiguous arrays.
Traversal order, first-seen duplicate coalescing, duplicate weights, remap
construction, and update-group logic remain in the same sequence. The temporary
storage has one slot per incident constraint endpoint, including duplicates;
for dense graphs with many duplicate endpoints this may use more peak temporary
bytes than the original per-vertex vectors. Native unit tests and before/after
trajectory or optimized-graph fingerprints must establish behavioral parity.

The provider candidate has one backend-local retained topology entry. It reuses
an optimized `SoftBodySharedSettings` and immutable index vector only when the
entire definition matches bit for bit: kind, world position, size, all three
resolutions, quality, collision and enabled flags, every material field, and
each ordered attachment vertex and absolute position. This conservative key
avoids assuming that different world translations produce identical local
floating-point pin coordinates. A cache hit inherits the full validation of
the prior successful construction. It still checks current world resource
limits, derives current native creation settings, allocates independent mutable
Jolt motion state, and assigns a new neutral ID generation.

Only successfully admitted definitions can replace the cache entry. A failed
build, native admission, or replacement does not commit a new entry. The entry
is limited to 16,384 vertices, 100,000 edge plus volume constraints, 50,000
faces, 150,000 indices, and 256 attachments. A successful different
noncacheable definition evicts it. It can retain an optimized topology after
the last body is destroyed, deliberately trading bounded provider-lifetime
memory for warm recreation latency. `Shutdown` releases it before the rigid
world. There is no process-global cache, native body pointer, or ID in the key.

## Verification gate

The dedicated provider admission target reports warm hits and per-body retained
memory. `AdmissionCacheQualification` checks exact warm reuse, misses for
position/material/pin changes, rejection of a 129×129 topology from the cache,
stale ID denial, and retirement. Native `SoftBodyAdmissionTests` exercises
duplicate and isolated graph connections, constraint remap permutations,
unpinned vertices, rod connectivity, and independent mutable bodies sharing
one settings object. Uninstrumented provider timing, native unit tests in all
qualified configurations, trajectory parity, and lifecycle resource receipts
remain required before accepting the candidate.
