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

The native candidate changes the temporary graphs used by Jolt
`SoftBodySharedSettings::Optimize` and `CalculateClosestKinematic`. It counts
per-vertex incident constraints and stores neighbors in contiguous arrays.
Its grouping frontier accumulates scores when each vertex joins the current
group, avoiding a full rescan of the group for every candidate. It retains
the original first-seen frontier order, duplicate connection weights,
distance tie break, remap construction, and update-group sequence. The
temporary storage has one slot per incident constraint endpoint, including
duplicates; for dense graphs with many duplicate endpoints this may use more
peak temporary bytes than the original per-vertex vectors.

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

## Provisional combined default-batch results

The first combined smoke (`C256-Smoke-20261109T064632`) is a provisional
candidate. Three fresh uninstrumented processes gave a 16K cloth median cold
admission of 19.24 ms, compared with 55.07 ms for frozen B; same-world
recreation medians were 0.328 ms and 56.11 ms. The 4K cold medians were
3.237 ms and 12.63 ms, with warm medians 0.099 ms and 12.23 ms. Four
independent bodies in different world positions took 19.31 ms cold and
18.85 ms on a second pass, compared with frozen-B 55.88 ms and 54.57 ms.
The strict cache key deliberately misses translated bodies.

The instrumented 16K first body made 1,052 tracked allocation calls versus
163,857 in frozen B. Subsequent cache-hit admissions made 5 each. Its first
body added 5,320,691 tracked live bytes; after that body retired, 3,096,923
bytes remained live in the provider cache. Frozen B retained only 2,256
bytes at the same point. With one worker, world setup held 63,554,174 tracked
live bytes in C versus 63,554,134 in B; with eight workers the pair was
63,696,186 versus 63,696,146. The 32 MiB temporary arena, runtime setup,
job setup, and world initialization had no material changed allocation
counts. World retirement freed the cached topology: final tracked net was
7,110 bytes in C versus 5,998 in the frozen-B world-profile run. The C
profile performed three admissions and B performed one, so these whole-run
nets are accounting context, not a like-for-like leak comparison. They also
include process/global allocations. The phase-level world-retirement frees
the cache-held topology.
Instrumented first-body `Optimize` cost was 17.23 ms and 835 allocations;
its timing is diagnostic only.

`AdmissionTraceCompare.py` streams the binary quality traces and verifies
the cumulative hash printed for every frame against decoded float bits. All
eight 40-frame C256 smoke traces (cloth 1K, 4K, 16K, rubber 4K at one and
eight workers) exactly matched the corresponding first 40 frozen-B long
frames, including every vertex position; all finite, pin, and axial strain
gates passed. The independent frozen-B long traces also passed hash integrity
over 600 cloth or 300 rubber frames. The comparison reports are preserved
under `AdmissionReduction/C256-*.json` and `AdmissionReduction/*_selfcheck.json`.
Later native or Engine changes require a new final trace comparison.

An opt-in 512-vertex constraint batch trial is deliberately a changed graph
and has a separate quality gate. Its 40-frame cloth16K W1 and W8 comparisons
(`Group512-20261109T064817`) both passed: peak RMS displacement was
0.0002593 m, peak vertex displacement 0.0006591 m, maximum pin error
0.000000237 m, peak axial strain 0.041161, and candidate final axial strain
0.017013. The hash differs from B at frame zero as expected. This is a
quality result, not a timing recommendation; the experiment must also pass
long-horizon and comparative performance review.

## Final combined candidate admission

The final measured C source is Engine
`f4ffd911baab53ebee82a3d3d62e040568d17d7e` and native Jolt
`7e59df5fd7f3e23504f2e696e2b095bf706b26b4`. It adds lazy temporary
graph construction and uses the opt-in 1024 constraint batch only for 16K
cloth; smaller scenes retain the original 256 batch. These figures come from
`C-Admission-20261109T070737`, compared with the frozen uninstrumented
`B-Admission-20261109T060849` on the same worker. Each timing cell below is
the median of three fresh process runs for the first admission, or six
same-world repeats for the second and third admissions. Each pair has the
same authored scene and body count.

| Scene and admission | Frozen B | Final C | B/C ratio |
| --- | ---: | ---: | ---: |
| 16K cloth first body | 55.072 ms | 16.985 ms | 3.24× |
| 16K cloth same-world recreation | 56.115 ms | 0.271 ms | 206.9× |
| 4K cloth first body | 12.630 ms | 3.339 ms | 3.78× |
| 4K cloth same-world recreation | 12.231 ms | 0.107 ms | 114.2× |
| Four differently positioned bodies, first pass | 55.879 ms | 16.585 ms | 3.37× |
| Four differently positioned bodies, next pass | 54.574 ms | 16.462 ms | 3.32× |

The four-body case has no topology-cache reuse across differently translated
definitions. The strict key avoids falsely identifying world-space pin and
float-rounded rest geometry as identical. The cache benefit therefore
applies to exact-definition recreation, while the native graph changes also
reduce cold and independent admissions. XPBD remains the separate functional
comparison in the frozen baseline; the final Jolt timings do not establish
feature or material equivalence with XPBD.

In separate instrumented world-profile receipts, the final 16K first body
made 507 tracked allocation calls versus 163,857 in frozen B. Its native
`Optimize` phase made 290 calls and requested 6,370,856 bytes versus B's
163,642 calls and 10,199,596 bytes. The first C `Optimize` diagnostic time
was 16.27/17.12 ms at one/eight workers, versus B's 50.01/48.56 ms. These
instrumented intervals are attribution only; the table above uses the
uninstrumented timing executable. C's grouping has 17 update groups, 3,810
serial edges, and 93,216 parallel edges at 1024, versus the earlier
default-256 candidate's 65 groups, 8,834 serial edges, and 88,192 parallel
edges. B's group counts were not exposed through its frozen native API.

| Tracked phase bytes | B W1 | C W1 | B W8 | C W8 |
| --- | ---: | ---: | ---: | ---: |
| World setup live delta | 63,554,134 | 63,554,174 | 63,696,146 | 63,696,186 |
| First body live delta | 5,318,515 | 5,318,003 | 5,318,515 | 5,318,003 |
| Remaining after first body retirement | 2,256 | 3,094,235 | 2,256 | 3,094,235 |
| World retirement freed bytes | 63,550,864 | 66,642,883 | 63,692,876 | 66,784,895 |

The approximately 3.09 MB C residue after body retirement is the deliberate
one-entry optimized-topology cache. World retirement releases it. World setup
still makes 43/53 tracked calls at one/eight workers in both B and C; the
32 MiB temporary arena and other fixed world reservations are unchanged.
The C profile performs three admissions while the B world-profile control
performs one, so whole-run residuals and working-set deltas are not
like-for-like leak measures. Process working set includes allocator and OS
residency and is distinct from tracked live requested bytes.

The final C no-collider long traces pass the frozen protocol: cloth1K and
cloth4K are bitwise identical to B over 600 frames, and rubber4K is
identical over 300 frames. The 16K opt-in grouping changes graph order, but
all 600 frames remain within the fixed B-to-C envelope (peak RMS
0.000646364 m, peak vertex displacement 0.001817667 m); every recorded
hash reverified against the binary trace. The 16K driven-sphere stress
scene fails the previously declared 10% absolute strain bound in both B
and C; see `LargeClothContactStress.md`. It remains outside the accepted
quality domain despite the admission speed and close B-to-C trajectory.

## Independent final guard review

An independent source pass at Engine
`3c864c58d4554f03e6a587d71d6bda3fffe06f1b` and native
`7e59df5fd7f3e23504f2e696e2b095bf706b26b4` found no new cache,
resource-cap, or scheduling-guard defect. The exact cache key includes every
field consumed by soft construction and native settings, including world
position and ordered absolute pins. Both hit and miss paths enforce current
live-body, vertex, constraint, queued-result, and native layer limits.
The cache owns one immutable settings/indices entry and no native motion
state; a live body's references survive eviction, and `Shutdown` releases
the entry after body retirement. Creation and replacement stage the
Definition copy, cache candidate, and body slot before native admission;
the new post-admission cache/slot commits use moves and counter updates.

The final rigid fast schedule is confined to a sleep-disabled world with
zero enabled soft bodies, touch-enabled rigid bodies, active constraints,
live attachments, and uncertainty latches. Failed touch or joint mutations
leave those latches conservative. This new schedule does not modify the
qualified coupled path. The production `src` and `include` trees have no
diff from the frozen Engine baseline; all changes remain experimental.

`Final-Guards-20261109T072412` contains three successful fresh runs each
of provider, scheduling, and concurrent cache qualifications. The added
cache fixture proves two live bodies share indices but retain independent
mutable motion: an impulse on A leaves B bitwise equal to an independent
control; retiring A and evicting the cache with a translated C leaves B's
topology and motion intact. A post-guard cloth16K 40-frame W8 trace exactly
matches the first 40 frames of `C-Long-20261109T070355` at every hash and
vertex, with both logs independently rehashed. The saved result is
`AdmissionReduction/FinalGuards-cloth16k-W8.json`.

This review does not claim general exception atomicity for older rigid
mesh and joint operations. Existing rigid `AdmitMesh` can allocate a map
node after native body admission, and existing joint creation can grow its
neutral slot vector after `World.AddConstraint`. The new uncertainty guards
preserve conservative cadence if such an operation throws, but those older
operations can still require separate rollback work for full allocation
failure atomicity. They were not introduced by this performance candidate.
