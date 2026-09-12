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

## KI-007: Soft-reference replacement can invalidate a structural removal frame

- Status: Resolved and validated (2026-09-11); published on `foundation/3l-content-availability` in `36d4eb668` (not merged).
- Priority: High.
- Area: Server replication reference-fixup completeness / journal ordering.
- Relevant code: `src/network/ReplicationCoordinator.cpp`,
  `tests/ReplicationRelevanceTests.cpp::TestSoftReferenceReplacementBeforeLeave`.
- Evidence: [3L.3 validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md).

A client has accepted Character.RootPart -> A. The server destroys A and changes
RootPart to fresh B, which is not Known or Desired for this peer. Servicing the
pending structural removal before the property journal emits no nil update:
the current-value predicate tests B against Leaving, overlooking the client's
old edge to A. Client snapshot preflight rejects the frame with
`Snapshot reference is outside the receiving scope`. The failure reproduces
with the original full-property loops, not only the withdrawn reference-index
candidate. The original failing checkpoint is retained as historical evidence.
No client validation was weakened.

The correction records accepted native reference values alongside existing
accepted ancestry. Structural removal derives required clear/replacement work
from those old edges, including Known referrers no longer Desired. A complete
clear/removal group is charged to the existing 3J allowance; incomplete groups
defer and genuinely oversized groups follow the existing failure policy. Soft
referrers that are themselves leaving may progress independently. Only accepted
frames update metadata/Known, and full identities preserve lifecycle safety.

Correctness-only v5 passes MSVC 6/6 and Clang 19 ASan/UBSan/LSan 7/7 with leak
detection, plus the original regression, ten-scenario matrix, 90 physics cases
and unchanged 500-peer convergence/acquisition/admission/8,192-cap checks.
Unknown/known replacement, nil, same-progression Enter, multiple reference fields,
destroy during preparation, fresh identity, disconnect and byte-budget rejection
are covered. Deliberately dangling frames remain rejected by client preflight.
This closes the ordering defect, not general planning work or Foundation health.

## KI-006: Content-coupled gameplay latency exceeds the 3L.2 readiness envelope

Update 2026-09-12: the user approved RPC p95/p99/max 150/250/500 ms and
Event/action max 250 ms, >=25% gameplay reserve, no large-group exception and
unqualified low-rate compatibility. Hierarchical reliable-byte admission is now
implemented and targeted-validated: elapsed-time per-peer/global
credit, finite backlog feedback, exact pre-acceptance sizing, encoded reuse and
generation-safe cleanup. The candidate 8 MiB/s application / 16 MiB/s backend
profile passes official near-max Local/Node 100-RPC cases at p99/max
72.058/73.316 and 74.723/75.801 ms with no timeout/error. This does **not** close
KI-006: full gameplay burst/request-path and client qualification, overload,
production journal margin, security and current-source CI remain open until
measured. The statements below that admission is absent describe the earlier
published checkpoints, not this implementation. See the
[current implementation contract](devdocs/CurrentArchitecture/NetworkingReliableDeploymentContract.md).

The published envelope assessment (`108200d07`) now has a
[deployment/atomic-group contract follow-up](devdocs/CurrentArchitecture/NetworkingReliableDeploymentContract.md).
One actual dependency group can occupy all 524,256 GRPL application bytes of
the 512 KiB GNS complete-message ceiling. At 256 KiB/s that necessarily means
two seconds of ideal FIFO serialization. The proposed finite-credit compatibility
class cannot be called a subsecond gameplay guarantee; reject an impossible rate/
latency profile rather than silently changing content or ordering. Production
admission remains unimplemented, and KI-006 remains open.

- Status: Open; Foundation 3M remains gated.
- Priority: High
- Area: Structural materialization and gameplay latency under peer scale.
- Evidence: [Foundation 3L.2 measured validation](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_2Validation.md).
- Relevant code: `src/network/ReplicationCoordinator.cpp`,
  `src/network/ReplicaApplier.cpp`, `tests/GameSessionBenchmark.cpp`.

The reliable-service envelope follow-up is diagnostic/design only. At the current
256 KiB/s backend rate, 50/75% byte pacing reduces controlled RPC maxima to about
53 ms while 1.45 MB of structure takes 10.97/7.32 s to converge. This is not an
official-path fix. Valid planned hard-reference work can encode to 123,183 bytes
in two operations; a universal small burst cap needs an explicit oversized-group
policy. Production implementation stops for the trusted deployment/aggregate rate
and compatibility decision. See
[NetworkingReliableServiceEnvelope.md](devdocs/CurrentArchitecture/NetworkingReliableServiceEnvelope.md).
No rates, lanes, wire, buffers or semantic bounds changed; KI-006 remains open.

Latest post-publication latency attribution (`latency-v3`) distinguishes due-tick
cadence from accepted-packet delay. Healthy Local/Node p99 is 31.00/33.88 ms;
raw Character/root observer gaps are 645.85/658.24 ms, throughput loss 9.02%/9.56%,
and all sampled ordinary publications are produced on their due tick. The worst
Local root sample spans 12 dilated shared-fixture iterations, not scheduler
rejection. The observer precedes real-client application; it is not a presentation
guarantee. A 512-operation client callback costs 24.03 ms including 8.89-ms native
preflight and 9.18-ms live application. No transaction redesign is implemented.

The official reliable service follow-up (2026-09-12) identifies fixed GNS
capacity plus same-lane FIFO backlog: configured minimum/maximum/effective send
rate are all 262,144 bytes/s. The fuller capture peaks at 1,449,887/1,449,885
pending reliable bytes, dominated by four large GRPL frames. Correlated RPC 6
has a 5.32–5.34-second estimated backend wait, microsecond engine handoff, and
5.52–5.54 seconds of client-local request-to-response availability; Player polling
and callbacks add milliseconds, not seconds. Smaller messages with identical
queued bytes do not help; test-only 1/4 MiB/s rates reduce backend drain by about
4x/16x. No production rate/buffer/lane/order/wire change is retained. The next owner
is **Networking Reliable Service Envelope**: supported rate/capacity policy,
dependency-complete/KI-007 atomic-group byte bound and bounded backlog admission,
before considering separate lanes. See the
[transport attribution ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#official-reliable-service-attribution-2026-09-12).

The preceding official near-max GNS test FAILS for both Local and Node: one RPC
times out at approximately five seconds; Remote receive gaps reach 5.544/5.529 s
while Player event-loop gaps remain below 54 ms. Sampled pending reliable bytes
reach 1,420,323; GRPL and reliable application use the same backend connection
stream. Backend rate/capacity versus head-of-line service is the next dedicated
owner; precise per-RPC backend wait remains unmeasured. No lane, wire, budget or
priority change is justified here. See the
[post-publication ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#post-publication-due-to-recipient-attribution-2026-09-11).
Client preflight, official transport guarantees, overload/recovery, the 170-entry
startup journal margin, current-source security and terminal CI remain open.

The preceding attribution/lookup checkpoint (`derive-lookup-v3`) preserves bounded
planning and reduces healthy Local/Node tick p99 to 30.82/30.74 ms by making
existing 3E examinations cheaper. Recipient maximum gaps remain 652.59/649.24 ms,
and throughput losses remain 9.15%/9.05%. 500-peer reload p99/max is 43.27/52.41 ms,
convergence 4.257 s / 158 ticks; load still has an 846.56-ms recipient gap.
3E dominates average derivation, but saturated structural publication and
client/observer service dominate the recorded gap windows. No jobified phase
was added: current spatial queries lack a safe pinned/read-only contract.
The [assessment](devdocs/CurrentArchitecture/ReplicationDerivationAssessment3L.md)
defines the proposed boundary and measured scaling ceiling. MSVC 6/6 and targeted
Clang 19 ASan/UBSan/LSan 7/7 pass; health, official delivery, client preflight,
overload, startup journal, security and CI remain open.

The preceding dependency-complete checkpoint (`planning-boundary-v13`) bounds private
planning at 65,536 charged steps/tick, rotates after at most 2,048 peer steps,
and exposes only complete, revision-valid, costed batches to unchanged 3J.
Maximum measured 500-peer discovery examinations are 56,007; global planning
reservations peak at 4,574,325 credits under an 8,388,608 ceiling. Reload tick
p99/max improves from 95.95/178.46 to 48.89/56.29 ms and Character/root max gap
from 1,147.77 to 459.61 ms, but convergence slows from 3.233 s / 33 ticks to
4.743 s / 158 ticks. Healthy 200-peer Local/Node recipient gaps worsen to
686.79/703.88 ms, despite lower maximum ticks and RPC tails. Streaming health
still fails. Recurring 3E query/hysteresis and post-selection publication/apply
cadence remain measurable owners; the new work cap is not an end-to-end latency
guarantee. One acquisition/initial admission, exact 8,192 cap and KI-007 semantics
are preserved. Startup journal observed margin improves to 170 entries but
remains unproved as a production bound. Client preflight, official reliable
transport, overload, current-source security and CI remain open. See the
[current validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md)
for exact final validation and unfavorable results as well as improvements.

The following paragraphs retain **historical checkpoints**. Statements that
general planning was unbounded describe those older sources, not v13.

The original 2026-09-11 attribution reproduces a 488.404-ms reload tick: three
whole-peer fixup passes examine 8,517,714 properties in 433.633 ms. The first
pass alone has 2,840,760 examinations, 90.36% scalar; encode retries are zero.
After the separately validated KI-007 correction, a canonical immutable
reference-property index reduces worst-tick examinations to 99,692 and combined
fixup CPU to 170.473 ms. Reload tick maximum is 221.427 ms and Character/root
gap is 1,200.67 ms; convergence remains 33 ticks (3,312.04 ms). Healthy Local/Node
load p99 remains 40.079/40.327 ms, with 404.849/397.816-ms recipient gaps and
approximately 11.47% throughput loss. Streaming health still fails. General
planning remains unbounded by a narrow per-tick work cap: 138,752 discovery
examinations and over half a million referrer visits occur in measured ticks.
The measured index is not the previously discarded, unvalidated candidate;
exact source and validation checkpoints are separated in the 3L.3 ledger.

The next instrumented attribution separates 138.570 ms of Desired/referrer work
(133.467 ms outside reference-cost construction), 6.628 ms accepted-edge work and
31.194 ms restoration. A call-local ordered map join reduces combined fixup work
from 176.494 to 128.261 ms with the same scopes/counts. Reload maximum becomes
178.456 ms, recipient gap 1,147.77 ms and convergence 3,233.36 ms / 33 ticks.
It retains no frontier and does not bound the 138,752 discovery examinations or
536,510 Desired referrer visits. KI-007 semantics and the exact cap are unchanged.
Dependency-complete resumable planning requires a larger reviewed completeness/
revision barrier; adding a partial scan cap is not a safe closure of this issue.

The [3L.3 diagnostic checkpoint](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3.md)
now retains the settled-static physics, typed Character-inspection and bounded
retired-catalog reclamation corrections. Retirement formerly made 143,447,770
peer-Known probes in the instrumented 500-peer eviction; private prepared/accepted
identity leases now permit at most 4,096 ownership checks/tick without peer
searches. Retirement CPU is 8.569 ms total / 0.604 ms maximum, and the previous
1.055-second eviction Character/root gap falls to 449.639 ms. Eviction converges
in 1.457 seconds / 32 ticks; load remains 2.690 seconds / 33 ticks.
The first new 200-peer control/Local/TLS Node load p99 is
4.571/39.901/42.373 ms, with Local/Node recipient gaps 621.101/680.095 ms.
General pre-3J discovery still reaches 68,258 examinations (106,176 at 500 peers)
and is not covered by the retirement cap. Complete Desired/Known/dependency/
fixup planning and recurring 3E query/hysteresis work remain the owning paths;
the general bounded-discovery objective is not completed. The historical
26-entry journal margin is startup-cumulative, not
the observed eviction margin; its whole-session bound remains unproved.
The separately rebuilt official Player keeps polling while RPC handlers return
promptly and replies wait behind reliable structural backlog: Remote service
gaps historically reach 3.785/3.796 seconds. That official transport path was not
rerun by the loopback correction. No new discovery queue, selection-cap increase,
wire change or legal content-limit change was made. The new retired-catalog
cardinality ceiling and ownership memory cost are explicit in the ledger;
full overload/security/CI closure, B and the 3M gate remain unchanged.
The 2026-09-11 reviewed-owner slice confirms that scalar payload changes already
avoid dependency-plan invalidation and retains only a shared per-step send
allowance to service late Character/Remote production after drained structural
work. Actual recording-transport Send now occurs in the production step, but
Local/Node load p99 remains 40.42/41.89 ms. Their max recipient gaps are
398.41/403.00 ms; the unchanged completion-driven RPC fixture now runs 301 rather
than 401 ticks, so raw cumulative measurements are not equal-duration comparisons.
500-peer reload still exposes a 478.171 ms server tick dominated by structural
selection/fixup work, 138,752 discovery examinations and a 1.508-second recipient
gap. The exact 8,192 selection cap and convergence remain intact, but surrounding
work is not bounded by it. One changed client property at 8,193 identities still
preflights the entire candidate world (roughly 89--101 ms total across retained
runs); localizing that requires an accepted transaction-validation design.
Official transport, client, overload, security and CI gates remain open. See the
current ledger for unfavorable 500-peer eviction/reload tails as well as the
isolated handoff improvement. No general frontier or readiness upgrade is claimed.
Historical measurements follow.

The candidate 32-peer 512-object workload converges within the existing 3J cap,
but RemoteFunction p99 rises from about 26 ms without the streamed region to
about 382 ms with it Resident. A 100-peer run similarly converges but records
one owner-action submission failure during reload. The first grouped 500-peer
fixture missed client-code hydration before the workload Script materialized;
that attempt produced zero RPC samples and is not an RPC failure. Establishing
the gameplay client first exposed a separate full-rate receive overload:
500 input commands/tick exceed the official host's one 128-event Poll call/tick,
and reliable gameplay traffic is disconnected when the simulated queue fills.
An explicitly staggered 12 Hz input profile converges without raising the host,
transport, or 3J limits, but the 500-peer baseline already has roughly one-second
RPC latency and streaming raises p99 to about 1.9 seconds; owner-action submission
failures remain. These are measured
performance/availability failures, not evidence that the previously fixed
RemoteFunction access violation has returned. Concurrent bounded build activity
is recorded and isolated confirmation is still required.

Resolution requires attributed baseline/resident/streaming measurements, healthy
owner/action/root-motion and Remote traffic, and clean 32/100/500 convergence
without raising limits, bypassing 3E/3J, or weakening replica validation. Full
semantic snapshot validation during incremental replica application is an
inspection lead, not a proven exclusive root cause.

The original 512-object / 1,048,197-byte official network-first Player case exceeded
its three-minute deadline with both Local and TLS Node providers. A non-invasive
main-thread stack, symbolized against an identical executable text section,
passes through string allocation, property/subtree encoding, SetParent,
LoadSnapshot, ReplicaApplier::ApplyFrame and GameSession::Poll. The Server reaches
Resident/reload while Player does not finish. Async output drainage excludes the
previous redirected-pipe explanation. Preserve this as a client materialization
regression target; do not bypass semantic validation to make it pass.

The final-correctness candidate removes discarded validation-world journal work
and reuses native preflight for exact repeats of already validated native
properties, while retaining live setters and changed-state preflight. The
isolated official Local/TLS Node maximum-content runs now have maximum frame
intervals of 35.193/35.911 ms and event-loop gaps of 44.184/45.566 ms. Both fully
materialize, evict and reload the 512-object hierarchy, but their 100-call RPC
gates still fail with two/one timeouts. Server handler traces show prompt
execution and replies queued behind large reliable structural messages. This is
a remaining transport-service problem, not evidence of a multi-second client
CPU freeze or a RemoteFunction access violation.

The 500-connected / 100-Character spectator baseline is also unhealthy before
streaming (tick p99 322.706 ms; RPC p99 691.378 ms). The fixture broadcasts a
diagnostic Attribute update to all 500 peers for every RemoteEvent. A controlled
follow-up uses the existing accepted-ACK counter instead of that diagnostic
broadcast; no Character, Remote, relevance, transport or 3J limits are raised.
The broadcast-heavy results remain capacity evidence, not an isolated
Character-only or content-streaming attribution.

The final-correctness follow-up now has an isolated, healthy deterministic
200-connected / 50-active-Character control (five Characters per spatial
neighborhood, 12 Hz staggered owner input). Correcting Windows relative-sleep
overshoot and keeping root-motion crossings on the fixture ground makes all
four no-streaming phases pass their health gates. Local/TLS Node baselines also
pass, but their load, eviction and reload phases fail. During load, Character
wall throughput falls from 8,218 states/s in the equivalent no-streaming phase
to 2,778/2,707; maximum publication gaps rise from 205.696 ms to
1,075.46/1,133.61 ms. RPC p99 rises from 52.780 ms to 424.622/430.907 ms.
This is now a genuine streaming differential failure, distinct from dense
500-Character baseline saturation.

A separate 3J correctness regression reproduced replay of pre-publication
Attribute history after an accepted complete Enter, rolling a replica backward.
Per-object prepare-time journal watermarks prevent that replay while retaining
post-prepare mutations and the existing structural cap. Load selections in the
200-peer fixture fall from approximately 1.26 million to 104,840 operations;
the remaining gameplay degradation is not thereby resolved.

After that correction, all four near-maximum/property-heavy official Player
Local/TLS Node profiles complete load/evict/reload and 100 RPCs without timeout,
error or crash. Maximum frame intervals are 46.783/46.567 ms for the near-limit
payload and 95.473/88.960 ms for property-heavy content; event-loop maxima are
57.888/60.417 and 108.894/101.981 ms respectively. Nevertheless continuous
Character/Remote handler service has 3.49–3.81 second maximum gaps and RPC p99
is 3.56–3.68 seconds. CPU responsiveness improvement does not close reliable
transport/service latency. These are headless official Windows hosts, not a
graphical GPU-present latency proof. See the final-correctness closure report
linked from the measured validation document for exact revisions and remaining
gates. Foundation 3M remains blocked.

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
