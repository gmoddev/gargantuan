---
status: validation-in-progress
owner: runtime
last_verified: 2026-09-08
related_code:
  - src/content/ContentAvailability.cpp
  - tests/GameSessionBenchmark.cpp
  - tests/OfficialNodeHostVertical.ps1
---

# Foundation 3L.2 measured validation

This is the measured companion to [the implementation and ownership notes](ContentAvailabilityFoundation3L_2.md). Final scale, sanitizer, security and publication results are still being collected. It does not authorize Foundation 3M.

The expanded 134-item/44-question request is tracked separately in the
[final-correctness closure report](ContentAvailabilityFoundation3L_2Closure.md).
The original 110-item/32-question report below remains historical evidence;
its pending statements are not the current follow-up verdict.

## Final-correctness follow-up (in progress)

The expanded final-closure request adds 134 report items and 44 direct questions;
the numbered first-pass index below remains historical until the follow-up gates
finish. Retained RSS and structural results are not discarded. The dense
500-Character workload is already saturated without streaming and is not the
principal streaming non-starvation discriminator.

The final first-pass Windows Node-enabled suite passed **4/4 in 877.98 s** on
Engine `a68f76b` and Node `f444042`. Linux Node-enabled CTest passed 3/4 but the
500-peer scale case exposed a teardown UAF (2,194.83 s total). That failure is
not covered by the earlier green Engine-only CTest or scoped static review.

Three pre-fix sanitizer regressions establish concrete lifetime errors:

| Path | Invalid lifetime | Dynamic evidence |
| --- | --- | --- |
| Scale fixture | `RawPeer` declared Session, Renderer, then Engine; reverse destruction freed Engine before `GameSession::~GameSession` called `TearDownPeer`, which read `Runtime->Players` at `GameSession.cpp:1203`. Engine was allocated by `PollRealPeer`; exception unwind bypassed the successful path's explicit Stop. | Original full Node scale log plus `linux-unwind-prefix.log`; a deterministic post-admission throw reproduces the same heap-use-after-free without the long 500-peer run. |
| Official Server | Renderer lived inside the `try` block but Engine lived outside. After an exception the renderer was dead when catch-handler `Engine::Destroy` invoked `Renderer->Destroy` at `Engine.cpp:191`. | `linux-server-unwind-prefix-full.log`; official packaged Server with an absent trusted smoke key reaches the catch handler and ASan reports stack-use-after-scope. |
| Failed Engine construction | FullyResident corrupt-content rejection threw after Engine had registered world callbacks. C++ did not call `~Engine`; the caller's retained world later invoked its `Destroying` callback on freed Engine storage. | `linux-constructor-prefix.log`; ASan heap-use-after-free at `Engine.cpp:174`, via `Signal::Once` / `Instance::Destroy`; allocation/free are the failed `make_unique<Engine>`. |

The exact benchmark GameSession-to-Engine error is fixture-specific, but the
audit proves additional production ownership errors. The overall lifetime
investigation cannot be dismissed as test-only. Candidate fixes use ordered
RAII owners in the fixture/host and a constructor-body cleanup guard that runs
normal Engine teardown before initialized members die. No pointer-nulling catch
list, leak, disabled exception, or sanitizer suppression is used.

A 14-stage benchmark cleanup pilot and the subsequent **100-iteration Local
and 100-iteration real-TLS Node suites pass ASan/UBSan/LSan**. Local CTest took
40.48 s; Node CTest 50.07 s (native/Go fixture 42.79 s). Every iteration verifies
expired weak world/provider owners and absent captured ObjectRegistry identities.
The 24 constructor-failure regressions pass in the 2.14 s content suite; the
relocated official Server normal/exception test passes in 8.81 s. Official
headless TLS Server validation passes in 15.04 s. These results are bound to
`build-3l2-worker/evidence/lifetime-source-manifest.json`, before the new Player
phase instrumentation. They are targeted lifetime evidence, not yet the final
full-suite revision verdict. The suite
includes provider/Engine/session/observer/client construction, runtime attachment,
peer establishment, demand, active acquisition, Resident content, failed provider,
failed admission, peer stop, and teardown. The constructor test repeats corrupt
bootstrap, invalid content configuration, and missing client LocalPlayer.

The scale and official-host RPC fixtures now read the actual terminal
`nil, status, message` return when counting timeouts. Previously `pcall` success
could conceal timeout classification, although errors were counted. Completed
Windows cases had zero errors, so their zero-timeout conclusion is unchanged;
failed Linux 500-peer zero-timeout fields are not reliable evidence.

### Official Player phase attribution (pre-fix)

The retained `player-profile-prefix` run uses the official relocated Windows
headless Player over GNS, Local Server, eight churn cycles and 100 RPCs. The
first three profiles pass; the near-maximum profile times out after three
minutes. Measurements are in `build-3l2-worker/evidence/player-profile-prefix/measurements.json`.
One final truncated stdout row from the forcibly stopped maximum case is
explicitly excluded (not treated as a zero); 836 complete frames remain.

| Objects / payload bytes | Frame mean/p50/p95/p99/max ms | Event-service max ms | Poll max ms |
| --- | --- | ---: | ---: |
| 100 / 55,107 | 16.628 / 16.661 / 17.862 / 18.203 / 31.084 | 42.446 | 30.843 |
| 256 / 144,917 | 17.165 / 16.636 / 17.853 / 25.227 / 195.172 | 195.109 | 194.792 |
| 512 / 292,485 | 21.357 / 16.592 / 24.906 / 102.435 / 809.013 | 808.869 | 808.607 |
| 512 / 1,048,197 | 196.815 / 16.684 / 1,540.744 / 1,585.406 / 1,706.896 | 1,706.889 | 1,706.667 |

The worst near-limit poll processes 96 small frames / 192 operations / 16,704
bytes: decode 0.503 ms, candidate copy 53.490 ms, semantic validation 272.797 ms,
validation-world loading 1,083.495 ms, live application 6.191 ms. Loading includes
246.768 ms repeated semantic validation, 136.050 ms construction, 394.757 ms
parenting and 305.786 ms properties. Remaining apply time includes temporary
world destruction. The dominant failure is repeated whole-world validation and
discarded journal serialization, not GNS decoding or a deadlock. Processing cost
exceeds the incoming small-update cadence and the bounded event batch develops
multi-second service gaps. The fixture's existing observation checks only the
region/Ground, so its first-visible marker alone does not prove all 512 objects.

Candidate changes retain full native snapshot preflight, remove its identical
duplicate semantic pass, and avoid serializing journal payloads when suppression
would discard them. A separate regression investigates missing render dirtiness
from live replica changes. No generic client queue, protocol change or relaxed
validation has been introduced. Final maximum-content acceptance is pending.

The isolated first optimization (discarded journal payloads only) is **not a
fix verdict**: the near-limit Local official fixture exits 22 after 113.10 s.
Across 1,800 complete frames its interval mean/p50/p95/p99/max is
60.714/16.641/23.179/1,393.424/1,530.696 ms; event-service max is 1,530.740 ms.
The worst poll still processes 96 small frames: 1,524.783 ms apply versus
0.421 ms decode, including 943.493 ms validation-world load. Of 100 RPC calls,
nine time out (mean 778.372 ms, p95 5,527.441 ms, p99 5,985.140 ms,
max 6,363.430 ms). Source hashes and complete traces are retained in
`player-suppression-stage1-source.json` and `player-suppression-stage1` under
the local evidence directory. The combined candidate is a separate experiment.

The new render-dirty regression fails all three pre-fix assertions: live GRPL
publish, transform and destroy do not notify the receiver's render scope.
The first decoded-boundary fixture also failed, but for a test-assumption error:
documents waiting on non-Resident dependencies are deliberately dropped to avoid
capacity deadlock. The corrected test observes charged worker completions before
Main drains/adopts them, at two measured document charges minus one byte, exactly
two charges and plus one byte; it does not change that production policy.

Combined-candidate focused Clang 19 ASan/UBSan/LSan tests now pass: replication
(including all three render-dirty assertions) 0.30 s; content availability,
constructor cleanup and decoded boundaries 3.06 s. The measured document charge
is 2,172,554 bytes. A 4,345,107-byte ceiling retains one charge and defers the
second; ceilings 4,345,108 and 4,345,109 retain exactly two charges with no
deferral. Cancellation, renewed admission, eviction and Stop subsequently
release the charges. No admission occurs while completions remain undrained.
The 37 scoped worker native/fixture hashes match local inputs in
`player-candidate2-worker-native-source.json`. These passes still do not replace
the required final full sanitizer run or official maximum-content measurement.

The follow-up official fixture now verifies the full descendant count, expected
Part/Attribute values and distinct retained client object references on reload.
It adds lightweight and Attribute-heavy legal-content profiles alongside the
representative and long-name maximum. A non-null Instance-reference-heavy
version-4 package unit cannot be fabricated honestly: the current persisted
`Serializable`/`TrySerializeValue` domain does not encode Instance references.
Existing GRPL hard-reference/ancestry denial tests remain required; no new package
reference format is introduced to manufacture this comparison.

### Player follow-up candidates

The combined second candidate passes 100, 256, representative 512 and lightweight
512 official Local profiles, but near-limit content still causes a 1,317.680 ms
event gap and six RPC timeouts. Attribute-heavy 512 / 1,024,009-byte content hits
a 2,892.420 ms gap and the three-minute fixture deadline. Its worst poll spends
2,477.204 ms loading temporary validation worlds versus 0.560 ms decoding.
The exact source manifest and raw traces are retained as `player-candidate2`.

Candidate three reuses native preflight only when every incremental operation
is a native property assignment exactly equal to the validated semantic state.
It still runs live setters, envelope/target/sequence validation and all changed
state preflight; a direct regression covers live prediction correction, rejected
changed suffix, preserved identity and stale sequence/epoch. Clang 19
ASan/UBSan/LSan replication tests pass. In the near-limit official Local run,
1,800 frames have mean/p50/p95/p99/max 16.658/16.651/17.683/18.184/36.346 ms;
event-service max is 43.323 ms and poll max 36.011 ms. However the fixture still
fails with one RPC timeout (mean 183.656, p95 51.322, p99 4,424.720, max
5,015.605 ms). Large reliable messages arrive over seconds while frames remain
responsive. This is not a complete Player/Remote acceptance pass; transport
backlog and the full-content lifecycle timeline are being separately traced.

### Differential fixture policy and baseline gate

The full game-session sanitizer follow-up found a candidate regression in
`Instance::NotifyPropertyCommitted`: resolving a replication scope for the
non-replicated LocalPlayer property during Players destruction called
`shared_from_this` after its shared owner expired. `linux-retirement-prefix.log`
records the resulting `bad_weak_ptr`/terminate stack. Scope lookup now follows
the original rule (replicated properties only). The corrected legal retirement
regression and the full GameSession suite subsequently pass Clang 19
ASan/UBSan/LSan, including the bootstrap failure matrix and 100 normal session
lifecycles (`linux-retirement-legal.log`, `linux-game-session-retirement-final.log`).
This precedes the additional client service-gap counters and is not yet the
final exact-source full Engine suite.

The spectator fixture exposed a real client bookkeeping defect: a validated
Player.Character=nil write can destroy a subtree and the post-apply sweep
retires its Remote registrations, then later accepted Leave frames try to retire
them again. GameSession now checks whether each ObjectId was still locally
known before retiring its registration. Replica semantic/epoch/sequence checks
remain unchanged. The first direct regression accidentally exceeded its own
64-operation indivisible dependency limit with 160 children and was rejected by
the Server before client retirement; the corrected 40-child legal-group
regression passes. The real 500-peer spectator run passes that former disconnect
point with the fix and reaches the baseline measurement below.

`--content-differential <package> <none|local|node> <active-peers> <neighborhood> [peer-count]`
is benchmark-only. It defaults to 500 accepted peers, starts with 100 offered moving
peers and five-Character neighborhoods, includes ten Animator root-motion
Characters, and offers the same four phases in all modes. `none` performs no
content acquisition. The ordinary native relevance focus, 3J and reliable
protocol paths remain unchanged. Phases are paced at a 16.667 ms frame target;
missed deadlines are measured, not compensated by a simulation catch-up loop.

The first 500-connected / 100-input-active / five-neighborhood baseline is
**unhealthy before streaming**: server tick p50/p95/p99/max
239.899/521.018/527.476/557.121 ms; RPC p95/p99/max
1,053.881/1,093.383/1,124.922 ms, zero timeouts; five action submission failures;
Character wall gap 18,296.4 ms. No acquisition or admission occurred. Merely
reducing input traffic leaves 500 Characters simulated and does not establish
a feasible differential. The next fixture revision retains 500 connected
Players but uses normal `Player::RemoveCharacter` for inactive spectators,
then waits for ordinary control/3J retirement. Root-motion actors cross a cell
edge within their own neighborhood instead of being relocated into the shared
content neighborhood. Production authority and scheduling are unchanged.

Before any content demand the benchmark requires server tick p95 <=16.667 ms,
p99 <=33.334 ms and max <=100 ms; RPC p95 <=150 ms, p99 <=250 ms and max <=500 ms
with zero errors; no owner-action submission/rejection failure and result max
<=250 ms; acknowledged RemoteEvent progress with <=250 ms maximum ACK gap; and
Character/root-motion publication gaps <=12 simulation ticks and <=250 ms wall.
These are explicit fixture health gates, not new Engine service guarantees.
An unhealthy baseline aborts the comparison rather than attributing saturation
to streaming. The preserved dense 500-peer capacity results are unchanged.
Event and action samples are bounded and reported separately from RPC samples;
the sampled percentiles end with each phase's 100-call RPC window.

With 500 connected peers and 100 actual Characters, the broadcast-heavy
spectator baseline records tick mean/p50/p95/p99/max
172.625/147.414/303.516/322.706/356.819 ms; 100 RPC mean/p50/p95/p99/max
567.906/620.672/645.382/691.378/712.982 ms with zero timeouts/errors;
one action submission failure and 2,416.17 ms Character/root publication gap.
There are zero content acquisitions/admissions. The health gate correctly
stops before Local/Node comparisons (`differential-retirement-fixed-100x5.log`).

Inspection identifies a fixture confounder: every server Event handler increments
the replicated global ScaleEvents Attribute. At 500 peers this diagnostic
itself generates approximately 500 structural updates per offered event.
The differential now counts the same handler's actual accepted reply through
the existing RemoteManager aggregate counter, exposed by one read-only scalar
`GameSessionTestAccess` helper. The ordinary dense scale fixture keeps its old
broadcast behavior for traceable comparison. No event, action, Character work,
relevance relationship or structural admission is bypassed in the differential;
only the diagnostic broadcast is absent. Its new baseline is pending.

The controlled no-counter 100-Character run confirms a large diagnostic cost:
mean tick 172.625 -> 34.839 ms; RPC p99 691.378 -> 138.085 ms; selected
structural operations 252,172 -> 53,672 over the same 400-tick baseline.
Actions have zero submission/rejection failures and maximum result 126.305 ms.
The baseline still fails: tick p95/p99/max 59.427/62.070/283.708 ms and
Character/root-motion wall gap 428.850 ms. No content demand occurs. This is
not a non-starvation pass. The next bounded trial uses fewer active Characters
and separately reports a 120-tick initialization warm-up so initial animation
rig publication/spectator retirement is not mistaken for steady gameplay.

The 200-peer / 50-Character timing split measures ordinary relative sleep
overshoot p50/p95/p99/max 14.474/15.480/15.711/16.409 ms. Server work is healthy
(p95 15.239 ms), but the fixture thereby almost halves wall-time cadence. Using
the already linked SDL precise wait, without catch-up or timer-setting changes,
reduces overshoot p95 to 0.276 ms and restores the no-stream initial baseline:
tick p99/max 15.995/58.030 ms, RPC p99/max 53.623/56.338 ms, Character gap
209.756 ms, zero RPC/action errors. The full control later has five action
submission failures; repeated next-cell relocation puts the fourth phase at
the resident ground edge. A subsequent fixture fixes the crossing location
inside the same neighborhood and records minimum root-motion height. These
corrections do not change production Character or replication configuration.

The precise-pacing run (`differential-precise-200p-50a.log`) demonstrates a real
load-associated cost despite healthy initial baselines. Local/Node load tick
p99/max are 109.544/223.022 and 134.237/219.324 ms, Character wall gaps
1,376.72/1,451.81 ms, and RPC p99 364.800/372.600 ms (100 calls each, zero
timeouts/errors/crashes). One acquisition/admission is retained, cap 8,192,
journal lag 6,135 and zero journal failures. Each load selects **1,260,086**
operations across 200 peers for one 512-object unit. The Go process-exit PASS
means completion only, **not non-starvation**; later revisions make every
phase's health gate part of the differential result.

The direct `linux-enter-history-prefix.log` regression proves why a portion of
that work is incorrect: a peer accepts a complete current Enter before reading
older journal history, then an old Attribute record rolls the replica back.
Both the no-rollback and one-post-prepare-operation assertions fail before the
fix. The narrow candidate captures a prepare-time exclusive publication cursor
and installs its per-object watermark only with exact scheduler acceptance.
It must preserve a mutation made after preparation, even if another peer has
already refreshed the shared catalog before acceptance. Final validation of
this candidate is pending; it is not yet a production acceptance verdict.

The publication-history fix passes the full replication-relevance suite with
ASan/UBSan/LSan. The next full GameSession run finishes all lifecycle work but
fails the newly added client Remote-service counter assertion in all 100
iterations: that fixture only sent a client-to-server Event and offered no
server-to-client Remote. Two explicit observed server replies are now added to
exercise the asserted client metric. This assertion failure is not a sanitizer
finding or evidence of dropped offered replies; the corrected test remains
pending.

Native worker source digest
`BF90D8A77842A8BFDB98012FFFA6BC5724FF1FBA5769EF84763BAFC7F4E2DE18`
binds 28 current scoped files (all match local) for the first publication-history
candidate, client service counters, corrected differential crossing and strict
phase gates. `enter-history-service-combined-source.json` also records both
Node fixture hashes; the subsequently corrected service-test replies differ
only in the GameSession test source. No sync commit is used.

### Maximum-content Player after publication-history correction

The official relocated Windows headless Player matrix passes both Local and
real-TLS Node, eight churn cycles per case and **100 RPC calls per case with
zero timeouts/errors/crashes** (94.58 s overall). These are official binaries
and ordinary GNS, but not a graphical GPU-present proof. Source identity is the
digest above; raw logs and measured distributions are in `player-enter-history`.

| Profile/provider | Frame mean/p50/p95/p99/max ms | Event-loop max ms | Decode max ms | Apply max ms | Render extraction max ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Near-max / Local | 16.649/16.655/17.719/18.677/46.783 | 57.888 | 2.971 | 45.810 | 0.206 |
| Near-max / Node | 16.649/16.655/17.687/18.158/46.567 | 60.417 | 5.993 | 40.289 | 0.205 |
| Property-heavy / Local | 16.648/16.656/17.696/18.532/95.473 | 108.894 | 7.552 | 84.094 | 0.236 |
| Property-heavy / Node | 16.648/16.640/17.733/23.696/88.960 | 101.981 | 7.646 | 77.532 | 0.203 |

Near-max payload remains 1,048,197 bytes/512 objects; property-heavy is
1,024,009 bytes/512 objects. Full load/evict/fresh reload is observed, not just
the root. Near-max Local/Node structural totals fall to 1,968,583/1,966,321
bytes and 3,271/3,245 operations; property-heavy totals are
1,874,923/1,874,749 bytes and 3,235/3,233 operations. Trace lengths differ
because each Player exits on successful completion. No client queue is added.

**Critical service is still not healthy.** Near-max Local/Node maximum
Character-handler gaps are 3,606.059/3,642.559 ms and Remote-handler gaps
3,796.147/3,809.294 ms. Property-heavy gaps are respectively
3,487.632/3,572.700 and 3,582.197/3,576.208 ms. The continuous Event probes
record 3,820.055/3,804.566 ms maximum ACK gaps for near-max and
3,612.926/3,590.432 ms for property-heavy. RPC p99/max remain
3,657.912/3,802.216 ms (near Local), 3,677.916/3,787.662 (near Node),
3,558.622/3,586.984 (heavy Local), 3,568.543/3,589.703 (heavy Node).
Responsive Player ticks do not erase this reliable-content delivery delay.
These completed fixtures are not an A/non-starvation verdict.

The corrected legal Character-retirement regression passes Clang 19
ASan/UBSan/LSan (`linux-retirement-legal.log`): client Ready, no old Character,
old replica destroyed, old server ObjectId invalid, one control revoke and a
fresh replacement lifetime. The full game-session suite is still being rerun.

## Revisions and environment (historical report items 1–8)

Baseline Engine: `b54256805b1e4af8f8ad60a8672507ce26bb0be2`; Node: `4e6bc0a8ff37c58a8a122262d746dd8934b71835`. Both matched freshly pulled intended remote branches; no intervening commits. Review checkpoints: Engine `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`, Node `89fbffa1a2ddf29866e0c3d34bd3a8b6f1df94bb`. These are implementation/review commits, not synchronization commits. Publication status is recorded separately below.

All heavy work runs on the authorized disposable `dockerbox` worker, Ryzen 9 5900X / 24 logical CPUs / 32 GiB RAM. Task sources, builds, caches and logs remain under `C:\Sandbox\Codex`. Local working trees are authoritative; unrelated worker containers/services and untracked local artifacts are preserved. Native MSVC 19.44 Release uses two to four bounded build jobs; Linux containers use Clang 19.1.1 with ASan/UBSan and `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`. Worker MSVC is not a substitute for the repository's newer hosted-CI MSVC requirement.

Narrow production changes: retained decoded-document accounting/deferral; final-world journal/render scope retirement; FullyResident backlog drain; negotiated structural byte-sized slices; safe coarse-coordinate conversion. No existing production ceilings, GRPL/GCHR/handshake/Remote protocol, package format, ordinary Luau API, automatic residency or provider authority changed. Tests add memory/lifecycle/scale workloads and aggregate diagnostics. Full native surface details are in the companion document.

## Official Server memory (items 9–21, 31–32)

Each row below is 1,000 official relocated Server OnDemand load/evict/reload cycles. Node rows use the production adapter and real TLS. These are **Server-only churn** measurements, not a Player vertical result. Small = 20 objects / 9,131 payload bytes; 100 = 55,107 bytes; 256 = 144,917 bytes; 512 = 292,485 bytes; near-max = 512 objects / 1,048,197 bytes. No payload limit is raised.

RSS and PrivateUsage were sampled approximately every 100 ms. The steady window is the second half of completed cycles, before the final 120-tick drain. Historical long-run logs precede the shared UTC clock anchor: steady/drain boundaries have approximately 0.22–0.30 seconds of clock-offset uncertainty. This does not support precise historical startup attribution; startup values come from separate eight-cycle aligned runs below. All memory values in the tables are MiB (1,048,576 bytes); slopes are **bytes/second**. Each profile ran about 237–270 seconds, well beyond initial allocator warm-up.

| Profile | Warm-up max | Steady min | Mean | p50 | p95 | p99 | Steady max | Drained mean | RSS slope B/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100-local | 48.426 | 46.668 | 48.397 | 48.426 | 48.426 | 48.426 | 48.426 | 48.426 | 1112.486 |
| 100-node | 53.070 | 53.027 | 53.090 | 53.094 | 53.094 | 53.094 | 53.094 | 53.094 | 138.600 |
| 256-local | 52.012 | 49.102 | 51.927 | 51.965 | 51.965 | 51.965 | 52.207 | 51.965 | 1270.994 |
| 256-node | 56.738 | 51.570 | 55.728 | 56.066 | 56.719 | 56.730 | 56.738 | 55.629 | -207.115 |
| 512-local | 56.988 | 51.129 | 55.702 | 55.973 | 56.090 | 56.098 | 58.227 | 55.597 | 1604.305 |
| 512-node | 61.367 | 55.633 | 60.387 | 60.590 | 60.734 | 61.488 | 62.559 | 60.730 | 769.278 |
| near-max-local | 70.020 | 61.461 | 67.865 | 68.027 | 68.309 | 69.207 | 69.215 | 67.962 | -629.222 |
| near-max-node | 72.820 | 66.762 | 72.145 | 72.395 | 73.023 | 73.352 | 74.293 | 72.461 | 3956.908 |
| small-local | 46.855 | 46.855 | 46.855 | 46.855 | 46.855 | 46.855 | 46.855 | 46.855 | 0.000 |
| small-node | 51.176 | 51.176 | 51.176 | 51.176 | 51.176 | 51.176 | 51.176 | 51.176 | 0.000 |

| Profile | Private mean | p50 | p95 | p99 | Max | Drained mean | Private slope B/s | First/last steady-quarter RSS means |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100-local | 36.976 | 36.992 | 36.992 | 36.992 | 36.992 | 36.992 | 649.066 | 48.337 / 48.426 |
| 100-node | 39.408 | 39.410 | 39.410 | 39.410 | 39.410 | 39.410 | 51.761 | 53.083 / 53.094 |
| 256-local | 40.707 | 40.742 | 40.742 | 40.742 | 40.992 | 40.742 | 1178.240 | 51.852 / 51.965 |
| 256-node | 42.731 | 42.906 | 43.750 | 43.750 | 43.750 | 42.771 | -585.275 | 55.701 / 55.713 |
| 512-local | 44.694 | 44.930 | 44.930 | 44.930 | 47.559 | 44.681 | 1049.198 | 55.644 / 55.756 |
| 512-node | 47.374 | 47.621 | 47.633 | 48.664 | 50.250 | 47.633 | 1653.752 | 60.374 / 60.396 |
| near-max-local | 57.199 | 57.203 | 57.840 | 58.359 | 58.828 | 57.203 | -3722.246 | 67.932 / 67.923 |
| near-max-node | 59.185 | 59.480 | 60.051 | 60.559 | 61.797 | 59.484 | 3994.706 | 71.833 / 72.232 |
| small-local | 35.320 | 35.320 | 35.320 | 35.320 | 35.320 | 35.320 | 0.000 | 46.855 / 46.855 |
| small-node | 37.617 | 37.617 | 37.617 | 37.617 | 37.617 | 37.617 | 0.000 | 51.176 / 51.176 |

Plateau verdict: bounded steady-state oscillation in all ten measured profiles; no continuing lifecycle-proportional growth observed after warm-up. Some fitted slopes remain positive (largest approximately 3,957 B/s), so the claim is **not** zero slope or proof of no leak at arbitrary duration. Flat first/last steady-quarter means, bounded extrema and drained values near the established range distinguish these traces from sustained growth. Initial 64-cycle warm-up-only results are retained as preliminary evidence, not used as the plateau proof.

Separate aligned startup runs (eight cycles, not the plateau experiment):

| Profile | First process sample | Post-bootstrap | Post-first-load | Host/sampler offset ms |
| --- | ---: | ---: | ---: | ---: |
| 100-local | 4.973 | 34.699 | 34.699 | 282.000 |
| 100-node | 10.316 | 37.102 | 38.688 | 222.000 |
| 256-local | 5.066 | 37.301 | 37.301 | 298.000 |
| 256-node | 10.508 | 40.238 | 41.539 | 229.000 |
| 512-local | 5.117 | 36.434 | 41.000 | 291.000 |
| 512-node | 10.711 | 45.012 | 45.012 | 252.000 |
| near-max-local | 4.891 | 36.438 | 44.641 | 304.000 |
| near-max-node | 15.246 | 38.039 | 48.965 | 247.000 |
| small-local | 4.949 | 33.438 | 33.438 | 273.000 |
| small-node | 10.344 | 37.285 | 37.285 | 220.000 |

The first process sample is not an idle-running Server baseline. Milestone measurements are the first sample at/after the aligned event, with approximately 100 ms resolution; two milestones may legitimately share a sample. Full distributions, original counts, thread/handle extrema and raw traces are retained with task evidence.

### Linux official Server sanitizer soak

The first Linux 1,000-cycle pilot finished in only about 28 seconds per provider, so it was extended to the existing trusted host limit of **10,000 cycles per provider**. Both extended runs pass using the same official relocated headless Server with Clang 19.1.1, ASan/UBSan and LSan `detect_leaks=1`: Local 290.356 seconds / 2,902 samples; TLS Node 285.325 seconds / 2,852 samples; total harness 581.997 seconds. This exercises the sample's first manifest entry (20 objects / 9,131 bytes), not the full Windows five-size matrix and not a Linux graphical Player.

The sampler reads the Server PID's `/proc/<pid>/status` VmRSS/VmHWM and thread count every 100 ms, not the Go runner/container RSS. PrivateUsage and handles are unavailable on this sampler and are reported as **null, not zero**. The same Unix-millisecond clock anchor aligns Server milestones with samples. ASan quarantine/instrumentation materially increases RSS; these absolute values must not be compared directly with the Windows Release measurements.

| Provider | First sample | Post-bootstrap / first load | Warm-up max | Steady min | Mean | p50 | p95 | p99 | Max / observed VmHWM | RSS slope B/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| local | 57.434 | 110.023 / 110.023 | 505.316 | 487.074 | 505.026 | 505.078 | 505.672 | 505.852 | 505.961 / 505.961 | 2313.684 |
| node | 57.434 | 124.121 / 124.121 | 519.031 | 506.258 | 518.449 | 518.461 | 519.039 | 519.195 | 519.270 / 519.270 | -521.946 |

All memory values above are MiB. Second-half observation windows contain 1480 Local and 1452 Node samples. Local first/last steady-quarter means are 504.848 / 505.240 MiB; Node 518.553 / 518.403 MiB. These support bounded post-warm-up behavior over the measured 10,000 cycles, with a small residual positive Local trend; they are not a literal zero-slope or unlimited-duration guarantee.

Final-drain mean RSS is 497.004 MiB Local and 511.217 MiB Node, but only two/three samples cover the short Linux drain. Treat those as coarse exit-adjacent observations, not a separate long post-drain plateau. Thread maxima stay at 15 Local / 44 Node. Both Server exits pass leak checking, and each provider reports all 10,000 cycles complete. Reload uses cached verified bytes, so this is repeated runtime lifetime churn, not 10,000 fresh content RPCs.

## Memory ownership and overload (items 22–30, 87–96)

Before correction, 64 legal dependency-blocked 512-object units retained 87,718,245 tracked bytes (peak 87,720,897) while the encoded cache held 16,825,728 bytes. The missing category was parsed/decoded Available documents.

A shared RAII wrapper now charges retained documents once across worker completion, Available and Main owners. Default/hard decoded ceiling: 16 MiB. Per-input bytes/objects and at most eight workers bound transient parsing separately; allocator headers, fragmentation, live Instances and transient parser storage are not included in the decoded counter. Aggregate pressure discards DOMs, keeps verified immutable bytes, and reparses through the bounded worker path without refetching.

| Attribution | Measured result |
| --- | ---: |
| Blocked dependency decoded high-water / final current | 3,230,336 / 0 bytes |
| Blocked case tracked peak / retained | 3,416,424 / 58,421 bytes |
| Independent overload units / objects per unit | 64 / 512 |
| Provider acquisitions / authoritative admissions | 64 / 64 |
| Pending / in-flight high-water | 48 / 16 |
| Raw completed payload high-water | 3,154,824 bytes |
| Encoded cache high-water | 16,825,728 bytes |
| Decoded total and Available decoded high-water | 16,151,680 bytes |
| Decoded deferrals | 59 |
| Detached preparation high-water | 512 objects (not a measured byte estimate) |
| Resident package-origin metadata high-water | 32,768 objects (not metadata bytes) |
| Drain | 70 ticks / 3,057.12 ms |
| Maximum Main content Step | 69,823 microseconds |
| Resident tracked bytes / peak | 183,614,121 / 189,523,114 |
| Tracked bytes after Stop / service destruction | 19,481,746 / 19,474,635 |
| Tracked bytes after final world destruction | 6,726,241 |
| Tracked bytes after test-only global journal attribution clear | 1,147,098 |
| Resident RSS / peak / PrivateUsage | 340,303,872 / 341,024,768 / 340,127,744 bytes |
| After Stop RSS / PrivateUsage | 81,420,288 / 76,050,432 bytes |
| After final world release RSS / PrivateUsage | 33,312,768 / 26,763,264 bytes |

The attribution-only global clear runs after every fixture consumer is gone; production teardown never globally clears other worlds. Stop releases content documents, cache, completions, manifest and record/index tables. Final DataModel ownership release separately retires only that world's journal/render-dirty scope. A 100-world regression keeps another world alive and verifies its records and dirty state survive unchanged. Existing unscoped journal and allocator/registry capacity are process infrastructure, not a surviving authoritative content graph.

100 same-process legal-maximum lifetimes (512 objects, exactly 1 MiB) warmed by about cycle 15: tail tracked range 11,872,300–11,875,423 bytes; peak 19,730,919. Worker preparation mean/p95/p99/max = 19.135/21.884/27.949/29.015 ms; Resident mean/p95/p99/max = 40.149/47.019/49.523/49.992 ms; Main content Step mean/p95/p99/max = 19.098/25.055/28.652/30.534 ms. This is bounded, not universally frame-safe.

Coverage limitation: the 64-unit memory-overload fixture does not simultaneously run the real Character/RemoteFunction workload. Single-unit fan-out overload exercises that gameplay separately. Therefore items 94–95 are not established by the 64-unit result.

## Scale and gameplay (items 33–72, 83–86)

The matrix uses one authoritative Engine and one actual NetworkClient Engine with ordinary Luau gameplay. Remaining peers use the existing simulated transport and decode ordinary GRPL/GCHR. Node cases acquire from a separate official Node process over TLS through the production adapter. This is **not 500 graphical Player processes**; the separate official GNS host vertical is measured below. Package version is 17; 100-object payload = 52,958 bytes, 512-object payload = 273,032 bytes. These differ from the packaged RSS fixture.

Every completed combination has one content-unit acquisition and one admission on load, then one cumulative acquisition and two admissions on reload. This acquisition counter excludes manifest RPC/channel setup. All peers observe the expected hierarchy/properties and a fresh ObjectId, with the old root unregistered and old 3K/3H projections absent. Reload reuses verified bytes; it does not duplicate the authoritative region per peer.

At 32/100 peers, owner input is generated each simulated tick. At 500, raw-peer input is explicitly staggered every five ticks (nominal 12 Hz), while the real gameplay client keeps its normal cadence. The separate full-rate 500-input/tick diagnostic exceeds the existing one-128-event-Poll/tick receive service and disconnects on the bounded simulated receive queue. Limits were not raised to hide that failure. The 500-peer profile uses 20 neighborhoods of 25 Characters plus a common focus; global Player/hard-Character relationships still produce 318,225 baseline desired relationships.

The real gameplay client is established first so its code is hydrated before the remaining peer admission. An earlier 500-peer attempt hydrated before the workload Script materialized and produced zero RPC samples; that attempt is retained, not counted as an RPC pass. This benchmark ordering is not a general late-client hydration fix.

Timings are wall-clock samples from the bounded shared worker, with concurrent task activity recorded in logs; they are not isolated hardware capacity guarantees. There is no performance-ready claim. "Tick" excludes the explicit test-only full spatial-consistency traversal, but includes ordinary production work. Convergence starts at the phase request, not only after Resident commit. Diagnostic high-water gauges can be cumulative across earlier phases/admission.

### Structural convergence

Profile = provider / content object count / peers. Convergence distributions below are milliseconds across peers; tick columns are p50/p95/p99/max. Every row has zero journal failures. The configured global selected limit stays 8,192; it is the default tested budget, not the existing 65,536 trusted-native maximum. Maximum observed lag 13,961 stays below 16,384 retention.

| Profile | Phase | Acq/admit | Pending HWM | Selected max | Lag | Converge p50/p95/p99/max ms | Converge ticks |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Local/100/32 | load | 1/1 | 0 | 8192 | 1194 | 58.431 / 58.431 / 58.432 / 58.432 | 2 / 2 / 2 / 2 |
| Local/100/32 | reload | 1/2 | 0 | 8192 | 1194 | 63.971 / 63.972 / 63.972 / 63.972 | 2 / 2 / 2 / 2 |
| Node/100/32 | load | 1/1 | 0 | 8192 | 1194 | 72.298 / 72.299 / 72.299 / 72.299 | 3 / 3 / 3 / 3 |
| Node/100/32 | reload | 1/2 | 0 | 8192 | 1194 | 70.767 / 70.768 / 70.768 / 70.768 | 2 / 2 / 2 / 2 |
| Local/100/100 | load | 1/1 | 1808 | 8192 | 2574 | 170.458 / 227.888 / 227.888 / 558.540 | 2 / 3 / 3 / 14 |
| Local/100/100 | reload | 1/2 | 1808 | 8192 | 2574 | 182.161 / 220.924 / 220.924 / 534.715 | 2 / 3 / 3 / 14 |
| Node/100/100 | load | 1/1 | 1808 | 8192 | 2574 | 184.369 / 235.981 / 235.982 / 588.194 | 2 / 3 / 3 / 14 |
| Node/100/100 | reload | 1/2 | 1808 | 8192 | 2574 | 231.221 / 277.754 / 277.754 / 617.244 | 2 / 3 / 3 / 14 |
| Local/100/500 | load | 1/1 | 41808 | 8192 | 13961 | 1517.360 / 1985.510 / 8295.470 / 12323.400 | 5 / 7 / 44 / 69 |
| Local/100/500 | reload | 1/2 | 41808 | 8192 | 13961 | 1508.410 / 1916.150 / 8056.790 / 11919.600 | 5 / 7 / 44 / 69 |
| Node/100/500 | load | 1/1 | 41808 | 8192 | 13961 | 1538.100 / 1988.890 / 8418.190 / 12554.100 | 5 / 7 / 44 / 69 |
| Node/100/500 | reload | 1/2 | 41808 | 8192 | 13961 | 1473.170 / 1874.660 / 7933.480 / 11625.800 | 5 / 7 / 44 / 69 |
| Local/512/32 | load | 1/1 | 8192 | 8192 | 6139 | 127.301 / 266.767 / 266.767 / 266.767 | 3 / 4 / 4 / 4 |
| Local/512/32 | reload | 1/2 | 8192 | 8192 | 6139 | 183.256 / 290.402 / 290.402 / 290.402 | 3 / 4 / 4 / 4 |
| Node/512/32 | load | 1/1 | 8192 | 8192 | 6139 | 147.459 / 286.993 / 286.993 / 286.993 | 6 / 7 / 7 / 7 |
| Node/512/32 | reload | 1/2 | 8192 | 8192 | 6139 | 230.187 / 343.401 / 343.401 / 343.401 | 3 / 4 / 4 / 4 |
| Local/512/100 | load | 1/1 | 43008 | 8192 | 6148 | 563.960 / 769.822 / 965.117 / 965.117 | 5 / 7 / 8 / 8 |
| Local/512/100 | reload | 1/2 | 43008 | 8192 | 6148 | 584.222 / 880.126 / 1046.260 / 1046.260 | 5 / 7 / 8 / 8 |
| Node/512/100 | load | 1/1 | 43008 | 8192 | 6148 | 629.488 / 850.931 / 1065.600 / 1065.600 | 5 / 7 / 8 / 8 |
| Node/512/100 | reload | 1/2 | 43008 | 8192 | 6148 | 568.560 / 853.566 / 1014.360 / 1014.360 | 5 / 7 / 8 / 8 |
| Local/512/500 | load | 1/1 | 247808 | 8192 | 13961 | 5787.900 / 10397.900 / 11272.200 / 11437.300 | 17 / 31 / 32 / 33 |
| Local/512/500 | reload | 1/2 | 247808 | 8192 | 13961 | 5244.510 / 9237.370 / 10056.300 / 10216.300 | 17 / 31 / 32 / 33 |
| Node/512/500 | load | 1/1 | 247808 | 8192 | 13961 | 5073.400 / 9130.270 / 9965.500 / 10126.100 | 17 / 31 / 32 / 33 |
| Node/512/500 | reload | 1/2 | 247808 | 8192 | 13961 | 5393.130 / 9566.550 / 10493 / 10667.400 | 17 / 31 / 32 / 33 |

### Tick distributions and process high-water

All tick values are milliseconds. RSS HWM is whole-process lifetime high-water, not solely the phase's content memory. The 500-peer phase runs approximately 498–501 ticks; the smaller cases run approximately 401. No-streaming baseline already includes peers, Character/root motion, actions, RemoteEvent and 100 RemoteFunction calls.

| Profile | Phase | Mean | p50 | p95 | p99 | Max | Phase wall ms | RSS HWM MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Local/100/32 | baseline | 2.659 | 2.324 | 4.502 | 4.832 | 10.385 | 2153.580 | 35.980 |
| Local/100/32 | load | 4.624 | 3.867 | 8.832 | 9.237 | 31.087 | 4008.980 | 44.527 |
| Local/100/32 | evict | 3.090 | 2.642 | 5.270 | 5.882 | 20.261 | 2387.820 | 44.527 |
| Local/100/32 | reload | 4.959 | 4.193 | 9.201 | 9.967 | 31.094 | 4164.870 | 47.113 |
| Node/100/32 | baseline | 3.363 | 2.971 | 5.534 | 6.427 | 13.304 | 2791.170 | 42.012 |
| Node/100/32 | load | 5.655 | 4.835 | 9.987 | 10.849 | 33.441 | 4927.560 | 51.820 |
| Node/100/32 | evict | 3.568 | 3.117 | 5.998 | 6.515 | 21.625 | 2830.570 | 51.820 |
| Node/100/32 | reload | 5.960 | 5.117 | 10.372 | 12.039 | 32.504 | 5038.130 | 56.465 |
| Local/100/100 | baseline | 23.073 | 20.008 | 38.197 | 44.005 | 57.099 | 12814.700 | 61.520 |
| Local/100/100 | load | 29.569 | 24.869 | 53.127 | 56.429 | 92.761 | 16792.800 | 73.102 |
| Local/100/100 | evict | 23.347 | 20.195 | 39.405 | 41.967 | 82.110 | 12801.200 | 73.102 |
| Local/100/100 | reload | 29.849 | 25.389 | 53.530 | 56.024 | 95.687 | 16807 | 79.582 |
| Node/100/100 | baseline | 24.286 | 21.169 | 40.114 | 43.608 | 61.015 | 13507.900 | 65.570 |
| Node/100/100 | load | 32.076 | 26.965 | 58.490 | 70.219 | 101.002 | 18379.100 | 80.344 |
| Node/100/100 | evict | 25.464 | 21.838 | 43.177 | 47.036 | 90.826 | 14007.100 | 80.352 |
| Node/100/100 | reload | 31.842 | 26.980 | 58.494 | 63.210 | 119.375 | 18191.600 | 87.160 |
| Local/100/500 | baseline | 254.377 | 272.136 | 581.798 | 619.285 | 637.669 | 136602 | 293.922 |
| Local/100/500 | load | 270.075 | 306.072 | 685.280 | 712.761 | 1029.140 | 145522 | 321.426 |
| Local/100/500 | evict | 259.506 | 277.645 | 593.510 | 623.281 | 653.389 | 138316 | 322.992 |
| Local/100/500 | reload | 261.659 | 296.069 | 667.301 | 721.423 | 746.899 | 140431 | 327.008 |
| Node/100/500 | baseline | 257.660 | 276.250 | 596.819 | 624.758 | 655.167 | 137804 | 288.469 |
| Node/100/500 | load | 264.530 | 297.605 | 670.300 | 709.210 | 1048.970 | 141946 | 317.059 |
| Node/100/500 | evict | 249.813 | 268.037 | 581.905 | 606.215 | 639.184 | 132151 | 319.762 |
| Node/100/500 | reload | 252.485 | 288.611 | 651.477 | 673.178 | 741.836 | 134711 | 319.762 |
| Local/512/32 | baseline | 2.693 | 2.277 | 4.767 | 5.182 | 10.200 | 2260.870 | 35.973 |
| Local/512/32 | load | 29.553 | 28.293 | 46.725 | 53.692 | 90.587 | 25619.400 | 67.020 |
| Local/512/32 | evict | 3.720 | 3.095 | 5.978 | 6.716 | 48.951 | 2575.580 | 67.020 |
| Local/512/32 | reload | 29.600 | 28.523 | 45.676 | 48.486 | 95.839 | 24991.100 | 75.309 |
| Node/512/32 | baseline | 2.610 | 2.221 | 4.713 | 5.210 | 10.779 | 2210.970 | 40.590 |
| Node/512/32 | load | 30.912 | 29.112 | 48.265 | 51.691 | 92.691 | 26843.600 | 74.297 |
| Node/512/32 | evict | 5.246 | 4.577 | 7.885 | 9.601 | 56.336 | 3639.230 | 74.297 |
| Node/512/32 | reload | 35.001 | 31.470 | 51.761 | 58.705 | 129.792 | 30093.400 | 80.438 |
| Local/512/100 | baseline | 21.381 | 18.696 | 36.405 | 40.338 | 55.078 | 12060.800 | 59.051 |
| Local/512/100 | load | 70.162 | 59.249 | 138.205 | 159.390 | 173.005 | 44635.800 | 103.750 |
| Local/512/100 | evict | 29.052 | 25.400 | 49.374 | 53.703 | 108.134 | 15405.800 | 103.750 |
| Local/512/100 | reload | 72.258 | 61.135 | 140.670 | 148.999 | 179.067 | 45608.800 | 112.938 |
| Node/512/100 | baseline | 25.953 | 22.633 | 43.853 | 45.928 | 62.716 | 14537.200 | 65.836 |
| Node/512/100 | load | 71.435 | 59.525 | 138.717 | 153.792 | 193.187 | 45822.300 | 111.695 |
| Node/512/100 | evict | 28.096 | 24.361 | 45.751 | 50.389 | 105.917 | 15195.100 | 111.695 |
| Node/512/100 | reload | 69.482 | 59.474 | 135.508 | 143.691 | 175.347 | 43683.200 | 118.109 |
| Local/512/500 | baseline | 267.923 | 282.908 | 620.351 | 658.411 | 727.971 | 143889 | 280.395 |
| Local/512/500 | load | 263.778 | 122.312 | 796.115 | 1033.890 | 1497.870 | 150880 | 387.934 |
| Local/512/500 | evict | 255.072 | 275.195 | 598.871 | 626.028 | 704.964 | 136517 | 387.934 |
| Local/512/500 | reload | 247.572 | 111.268 | 757.574 | 976.874 | 1021.530 | 141040 | 387.934 |
| Node/512/500 | baseline | 249.324 | 270.068 | 578.506 | 602.890 | 634.874 | 133452 | 282.258 |
| Node/512/500 | load | 251.501 | 111.158 | 749.723 | 1036.720 | 1457.330 | 143799 | 392.945 |
| Node/512/500 | evict | 262.067 | 284.939 | 616.854 | 648.923 | 766.688 | 139821 | 398.363 |
| Node/512/500 | reload | 258.370 | 118.225 | 785.530 | 1033.030 | 1091.380 | 146906 | 398.363 |

### RemoteFunction

Each row is 100 ordinary paced calls (one simulation yield between calls), not an unbounded concurrent request flood. Values are milliseconds. All completed rows have zero timeouts/errors; no access violation occurred. This preserves the exercised 100-call regression, not a proof for arbitrary traffic. Streaming materially worsens p99, and 500-peer baseline responsiveness is already poor.

| Profile | Phase | Mean | p50 | p95 | p99 | Max | Timeouts/errors |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Local/100/32 | baseline | 15.689 | 15.539 | 18.164 | 18.929 | 19.342 | 0/0 |
| Local/100/32 | load | 30.911 | 31.099 | 35.611 | 43.077 | 75.842 | 0/0 |
| Local/100/32 | evict | 17.333 | 16.794 | 20.512 | 24.963 | 26.709 | 0/0 |
| Local/100/32 | reload | 32.161 | 32.105 | 36.304 | 42.947 | 82.525 | 0/0 |
| Node/100/32 | baseline | 20.296 | 20.040 | 24.673 | 27.513 | 29.329 | 0/0 |
| Node/100/32 | load | 36.141 | 35.597 | 41.470 | 47.631 | 80.134 | 0/0 |
| Node/100/32 | evict | 20.512 | 20.312 | 24.238 | 25.192 | 25.333 | 0/0 |
| Node/100/32 | reload | 38.789 | 37.908 | 46.569 | 52.178 | 90.364 | 0/0 |
| Local/100/100 | baseline | 92.560 | 87.163 | 107.331 | 117.988 | 118.031 | 0/0 |
| Local/100/100 | load | 130.385 | 137.632 | 152.462 | 168.499 | 207.463 | 0/0 |
| Local/100/100 | evict | 92.381 | 87.271 | 106.865 | 109.240 | 110.113 | 0/0 |
| Local/100/100 | reload | 130.483 | 137.537 | 144.151 | 148.879 | 191.222 | 0/0 |
| Node/100/100 | baseline | 97.797 | 92.991 | 112.652 | 122.340 | 123.168 | 0/0 |
| Node/100/100 | load | 142.915 | 146.003 | 173.358 | 195.856 | 203.900 | 0/0 |
| Node/100/100 | evict | 100.957 | 95.775 | 121.884 | 127.298 | 137.905 | 0/0 |
| Node/100/100 | reload | 141.254 | 146.072 | 164.572 | 168.528 | 240.771 | 0/0 |
| Local/100/500 | baseline | 1009.385 | 1075.597 | 1174.113 | 1204.867 | 1295.701 | 0/0 |
| Local/100/500 | load | 1079.020 | 1240.816 | 1313.848 | 1348.074 | 1827.488 | 0/0 |
| Local/100/500 | evict | 1014.335 | 1095.624 | 1168.348 | 1198.420 | 1201.992 | 0/0 |
| Local/100/500 | reload | 1035.825 | 1198.584 | 1296.037 | 1321.578 | 1323.709 | 0/0 |
| Node/100/500 | baseline | 1018.043 | 1090.930 | 1189.669 | 1246.131 | 1256.595 | 0/0 |
| Node/100/500 | load | 1051.478 | 1193.560 | 1287.588 | 1316.103 | 1850.949 | 0/0 |
| Node/100/500 | evict | 968.712 | 1043.930 | 1137.311 | 1159.487 | 1205.375 | 0/0 |
| Node/100/500 | reload | 995.188 | 1153.906 | 1221.440 | 1267.971 | 1318.297 | 0/0 |
| Local/512/32 | baseline | 16.432 | 15.907 | 20.322 | 22.227 | 27.296 | 0/0 |
| Local/512/32 | load | 190.305 | 189.158 | 226.452 | 239.795 | 247.589 | 0/0 |
| Local/512/32 | evict | 18.446 | 17.752 | 21.399 | 22.503 | 60.214 | 0/0 |
| Local/512/32 | reload | 183.894 | 179.643 | 202.570 | 215.412 | 259.888 | 0/0 |
| Node/512/32 | baseline | 16.104 | 15.793 | 18.688 | 19.802 | 22.013 | 0/0 |
| Node/512/32 | load | 204.046 | 206.834 | 228.470 | 268.929 | 310.565 | 0/0 |
| Node/512/32 | evict | 26.381 | 26.002 | 31.239 | 36.082 | 72.490 | 0/0 |
| Node/512/32 | reload | 221.861 | 218.335 | 242.476 | 255.885 | 308.881 | 0/0 |
| Local/512/100 | baseline | 87.138 | 85.367 | 105.424 | 117.979 | 118.763 | 0/0 |
| Local/512/100 | load | 347.585 | 373.571 | 418.983 | 431.522 | 464.533 | 0/0 |
| Local/512/100 | evict | 111.303 | 107.600 | 132.323 | 199.945 | 224.876 | 0/0 |
| Local/512/100 | reload | 354.986 | 386.961 | 418.938 | 432.975 | 474.993 | 0/0 |
| Node/512/100 | baseline | 105.111 | 101.643 | 124.680 | 125.909 | 131.629 | 0/0 |
| Node/512/100 | load | 356.880 | 379.049 | 427.466 | 451.345 | 457.100 | 0/0 |
| Node/512/100 | evict | 110.002 | 103.116 | 125.939 | 190.614 | 217.313 | 0/0 |
| Node/512/100 | reload | 340.173 | 375.069 | 397.546 | 415.343 | 458.937 | 0/0 |
| Local/512/500 | baseline | 1062.090 | 1120.607 | 1257.167 | 1339.154 | 1466.992 | 0/0 |
| Local/512/500 | load | 1178.317 | 1233.795 | 1892.170 | 1944.811 | 2010.029 | 0/0 |
| Local/512/500 | evict | 1005.946 | 1102.283 | 1167.670 | 1195.226 | 1202.287 | 0/0 |
| Local/512/500 | reload | 1098.297 | 1187.085 | 1712.110 | 1765.108 | 1772.605 | 0/0 |
| Node/512/500 | baseline | 984.712 | 1068.491 | 1130.739 | 1162.820 | 1178.853 | 0/0 |
| Node/512/500 | load | 1115.302 | 1181.149 | 1793.296 | 1884.834 | 1914.477 | 0/0 |
| Node/512/500 | evict | 1027.167 | 1112.659 | 1203.572 | 1230.503 | 1276.767 | 0/0 |
| Node/512/500 | reload | 1143.581 | 1220.877 | 1825.022 | 1888.453 | 1897.374 | 0/0 |

### Character publication and owner input

States/s and bytes/s aggregate recipient observations (not unique simulated Characters) and use **simulated** 60-Hz time; the separately reported states/wall-second prevents interpreting slow ticks as real-time health. Gaps are simulation ticks. Metrics do not include a wall-time maximum publication gap, so 12 ticks must not be presented as a guaranteed 200 ms wall latency. Scheduler rejections are zero in every completed phase. Owner input remains accepted, but that alone is not a responsive-control pass.

| Profile | Phase | States/s sim | Bytes/s sim | States/s wall | Gap ticks | Accepted inputs | Max owner age ticks |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Local/100/32 | baseline | 17927.300 | 1349440 | 55634.800 | 6 | 12801 | 4 |
| Local/100/32 | load | 16531.600 | 1239390 | 27559.600 | 12 | 12832 | 4 |
| Local/100/32 | reload | 12678.400 | 939258 | 20344.900 | 12 | 12832 | 4 |
| Node/100/32 | baseline | 17903 | 1347590 | 42868 | 6 | 12801 | 4 |
| Node/100/32 | load | 16537.300 | 1239830 | 22429.800 | 12 | 12832 | 4 |
| Node/100/32 | reload | 12664.900 | 938261 | 16800.700 | 12 | 12832 | 4 |
| Local/100/100 | baseline | 187019 | 13907800 | 97537 | 12 | 40001 | 7 |
| Local/100/100 | load | 174322 | 12947400 | 69378.200 | 12 | 40100 | 7 |
| Local/100/100 | reload | 131983 | 9767790 | 52483.300 | 12 | 40100 | 7 |
| Node/100/100 | baseline | 186992 | 13905700 | 92518.400 | 12 | 40001 | 7 |
| Node/100/100 | load | 174221 | 12939800 | 63353.300 | 12 | 40100 | 7 |
| Node/100/100 | reload | 131891 | 9761020 | 48455.100 | 12 | 40100 | 7 |
| Local/100/500 | baseline | 417854 | 31153300 | 25542 | 12 | 49723 | 60 |
| Local/100/500 | load | 420328 | 31225300 | 24022 | 12 | 49968 | 60 |
| Local/100/500 | reload | 229002 | 16946900 | 13534.900 | 12 | 49821 | 60 |
| Node/100/500 | baseline | 417475 | 31044600 | 25296.100 | 12 | 49721 | 60 |
| Node/100/500 | load | 420066 | 31206000 | 24611.800 | 12 | 49970 | 60 |
| Node/100/500 | reload | 229125 | 16955800 | 14117.100 | 12 | 49817 | 60 |
| Local/512/32 | baseline | 17930.100 | 1349660 | 53003 | 6 | 12801 | 4 |
| Local/512/32 | load | 16212.600 | 1215580 | 4229.370 | 12 | 12832 | 4 |
| Local/512/32 | reload | 12376.600 | 916925 | 3309.860 | 12 | 12832 | 4 |
| Node/512/32 | baseline | 17925.900 | 1349340 | 54186.700 | 6 | 12801 | 4 |
| Node/512/32 | load | 16212.100 | 1215540 | 4036.380 | 12 | 12832 | 4 |
| Node/512/32 | reload | 12315.100 | 912374 | 2735.020 | 12 | 12832 | 4 |
| Local/512/100 | baseline | 187055 | 13910500 | 103654 | 12 | 40001 | 7 |
| Local/512/100 | load | 173225 | 12865400 | 25937.100 | 12 | 40099 | 7 |
| Local/512/100 | reload | 130917 | 9688930 | 19184.100 | 12 | 40097 | 7 |
| Node/512/100 | baseline | 186936 | 13901400 | 85941.800 | 12 | 40001 | 7 |
| Node/512/100 | load | 173194 | 12863100 | 25260.900 | 12 | 40097 | 7 |
| Node/512/100 | reload | 130995 | 9694690 | 20041.700 | 12 | 40100 | 7 |
| Local/512/500 | baseline | 417291 | 31048300 | 24215.700 | 12 | 49719 | 60 |
| Local/512/500 | load | 419449 | 31167500 | 23120.400 | 12 | 49968 | 60 |
| Local/512/500 | reload | 228466 | 16907200 | 13444.900 | 12 | 49823 | 60 |
| Node/512/500 | baseline | 417670 | 31046000 | 26133.300 | 12 | 49722 | 60 |
| Node/512/500 | load | 419933 | 31205500 | 24287 | 12 | 49968 | 60 |
| Node/512/500 | reload | 228197 | 16887400 | 12892.900 | 12 | 49818 | 60 |

### Owner actions and RemoteEvent

Counts are phase deltas except maximum result latency, which is a cumulative diagnostic. Endings include the action catalog's native scripted/peer paths and are not one-to-one with the one gameplay client's submissions. Rejections and unexpected ending reasons are zero throughout completed runs, but submission failures occur at 500 peers (including baseline), so owner actions are **not** declared healthy. The known-good smaller repeat does not erase earlier observed failures. RemoteEvent progresses every phase; no event-stall assertion fired. Its latency distribution was not separately instrumented.

| Profile | Phase | Accepted / resolved | Submission failures | Endings | Max result ms | RemoteEvents |
| --- | --- | --- | --- | --- | --- | --- |
| Local/100/32 | baseline | 10/10 | 0 | 10 | 18.168 | 400 |
| Local/100/32 | load | 11/11 | 0 | 10 | 54.668 | 402 |
| Local/100/32 | evict | 10/10 | 0 | 11 | 54.668 | 402 |
| Local/100/32 | reload | 10/10 | 0 | 11 | 54.668 | 402 |
| Node/100/32 | baseline | 10/10 | 0 | 10 | 24.031 | 400 |
| Node/100/32 | load | 11/11 | 0 | 10 | 42.506 | 402 |
| Node/100/32 | evict | 10/10 | 0 | 11 | 42.506 | 402 |
| Node/100/32 | reload | 10/10 | 0 | 11 | 43.769 | 402 |
| Local/100/100 | baseline | 10/10 | 0 | 10 | 107.890 | 400 |
| Local/100/100 | load | 11/11 | 0 | 10 | 179.004 | 402 |
| Local/100/100 | evict | 10/10 | 0 | 11 | 179.004 | 402 |
| Local/100/100 | reload | 10/10 | 0 | 11 | 179.004 | 402 |
| Node/100/100 | baseline | 10/10 | 0 | 10 | 110.712 | 400 |
| Node/100/100 | load | 11/11 | 0 | 10 | 194.046 | 402 |
| Node/100/100 | evict | 10/10 | 0 | 11 | 194.046 | 402 |
| Node/100/100 | reload | 10/10 | 0 | 11 | 194.046 | 402 |
| Local/100/500 | baseline | 13/13 | 0 | 40 | 1205.440 | 499 |
| Local/100/500 | load | 11/11 | 1 | 46 | 1828.090 | 500 |
| Local/100/500 | evict | 12/12 | 1 | 39 | 1828.090 | 500 |
| Local/100/500 | reload | 8/8 | 5 | 33 | 1828.090 | 500 |
| Node/100/500 | baseline | 9/9 | 4 | 27 | 1233.800 | 499 |
| Node/100/500 | load | 12/12 | 0 | 48 | 1851.530 | 500 |
| Node/100/500 | evict | 8/8 | 5 | 31 | 1851.530 | 500 |
| Node/100/500 | reload | 7/7 | 6 | 23 | 1851.530 | 500 |
| Local/512/32 | baseline | 10/10 | 0 | 10 | 19.846 | 400 |
| Local/512/32 | load | 11/11 | 0 | 10 | 203.896 | 402 |
| Local/512/32 | evict | 10/10 | 0 | 11 | 203.896 | 402 |
| Local/512/32 | reload | 10/10 | 0 | 11 | 216.836 | 402 |
| Node/512/32 | baseline | 10/10 | 0 | 10 | 17.268 | 400 |
| Node/512/32 | load | 11/11 | 0 | 10 | 226.110 | 402 |
| Node/512/32 | evict | 10/10 | 0 | 11 | 226.110 | 402 |
| Node/512/32 | reload | 10/10 | 0 | 11 | 242.185 | 402 |
| Local/512/100 | baseline | 10/10 | 0 | 10 | 104.716 | 400 |
| Local/512/100 | load | 11/11 | 0 | 42 | 425.708 | 402 |
| Local/512/100 | evict | 10/10 | 0 | 11 | 425.708 | 402 |
| Local/512/100 | reload | 10/10 | 0 | 46 | 425.708 | 402 |
| Node/512/100 | baseline | 10/10 | 0 | 10 | 122.127 | 400 |
| Node/512/100 | load | 11/11 | 0 | 43 | 415.299 | 402 |
| Node/512/100 | evict | 10/10 | 0 | 11 | 415.299 | 402 |
| Node/512/100 | reload | 10/10 | 0 | 48 | 415.299 | 402 |
| Local/512/500 | baseline | 9/9 | 4 | 30 | 1200.860 | 499 |
| Local/512/500 | load | 12/12 | 0 | 51 | 1870.620 | 500 |
| Local/512/500 | evict | 12/12 | 1 | 42 | 2188.880 | 500 |
| Local/512/500 | reload | 9/9 | 4 | 38 | 2188.880 | 500 |
| Node/512/500 | baseline | 9/9 | 4 | 27 | 1123.390 | 499 |
| Node/512/500 | load | 12/12 | 0 | 55 | 1733.410 | 500 |
| Node/512/500 | evict | 12/12 | 1 | 40 | 2335.460 | 500 |
| Node/512/500 | reload | 9/9 | 4 | 39 | 2335.460 | 500 |

### Real Animator root motion

Ten authoritative Animator tracks drive the ordinary root-motion admission/commit path near a coarse-cell boundary. Every produced request in these rows commits; ordinary GCHR observes root states and each fixture checks current spatial cell addresses at the relevance safe point. Counts establish continued progress and spatial correctness, not healthy wall-time latency at 500 peers.

| Profile | Phase | Requests / commits | GCHR states | Max gap ticks | Cell crossings |
| --- | --- | --- | --- | --- | --- |
| Local/100/32 | baseline | 2130/2130 | 30211 | 6 | 10 |
| Local/100/32 | load | 1980/1980 | 29174 | 12 | 11 |
| Local/100/32 | reload | 2070/2070 | 26572 | 6 | 11 |
| Node/100/32 | baseline | 2550/2550 | 30050 | 6 | 10 |
| Node/100/32 | load | 1780/1780 | 29211 | 12 | 11 |
| Node/100/32 | reload | 1930/1930 | 26482 | 6 | 11 |
| Local/100/100 | baseline | 2170/2170 | 88600 | 12 | 10 |
| Local/100/100 | load | 2010/2010 | 85425 | 12 | 11 |
| Local/100/100 | reload | 2120/2120 | 76849 | 6 | 11 |
| Node/100/100 | baseline | 2140/2140 | 88419 | 12 | 10 |
| Node/100/100 | load | 2010/2010 | 84749 | 12 | 11 |
| Node/100/100 | reload | 2100/2100 | 76237 | 6 | 11 |
| Local/100/500 | baseline | 3220/3220 | 308973 | 12 | 21 |
| Local/100/500 | load | 3180/3180 | 285544 | 12 | 21 |
| Local/100/500 | reload | 3220/3220 | 256461 | 12 | 27 |
| Node/100/500 | baseline | 3200/3200 | 301809 | 12 | 21 |
| Node/100/500 | load | 3170/3170 | 283365 | 12 | 21 |
| Node/100/500 | reload | 3210/3210 | 257475 | 12 | 27 |
| Local/512/32 | baseline | 2280/2280 | 30230 | 6 | 10 |
| Local/512/32 | load | 2130/2130 | 27051 | 12 | 11 |
| Local/512/32 | reload | 2120/2120 | 24564 | 12 | 11 |
| Node/512/32 | baseline | 2190/2190 | 30202 | 6 | 10 |
| Node/512/32 | load | 2160/2160 | 27049 | 12 | 11 |
| Node/512/32 | reload | 2150/2150 | 24153 | 12 | 11 |
| Local/512/100 | baseline | 2080/2080 | 88840 | 12 | 10 |
| Local/512/100 | load | 2230/2230 | 78128 | 12 | 11 |
| Local/512/100 | reload | 2210/2210 | 69754 | 12 | 11 |
| Node/512/100 | baseline | 2120/2120 | 88044 | 12 | 10 |
| Node/512/100 | load | 2210/2210 | 77914 | 12 | 11 |
| Node/512/100 | reload | 2220/2220 | 70272 | 12 | 11 |
| Local/512/500 | baseline | 3330/3330 | 300279 | 12 | 21 |
| Local/512/500 | load | 3230/3230 | 278728 | 12 | 21 |
| Local/512/500 | reload | 3210/3210 | 252011 | 12 | 27 |
| Node/512/500 | baseline | 3210/3210 | 303437 | 12 | 21 |
| Node/512/500 | load | 3220/3220 | 282750 | 12 | 21 |
| Node/512/500 | reload | 3320/3320 | 249782 | 12 | 27 |

Verdict: all twelve bounded-input combinations converge structurally, including load/evict/fresh reload. They do **not** close simulation non-starvation. 500-peer baseline is slow before content, streaming adds material latency, and actions miss submissions. Content admission still feeds normal 3K/3H -> 3E -> 3J; no Desired/Known injection or cap bypass makes these fixtures pass.


## Lifecycle and failure behavior (items 28–30, 73–82)

100 complete official Node -> GargantuanServer -> GargantuanPlayer cycles passed in 665.251 seconds, including the additional failure/equivalence checks in the same fixture. Each main cycle uses fresh processes/provider/channel, the same Node TCP and Server UDP endpoints, LocalPlayer/Character/movement, RemoteEvent, five paced RemoteFunction calls, load/evict/reload, disconnect and process waits. Runtime PATH is restricted to the relocated package and Windows system directories; configured Node credential variables are removed from Player. This proves the exercised headless official Player/GNS path, not 100 interactive graphical windows.

| Quantity | Samples | Mean ms | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Complete main process cycle | 100 | 5762.180 | 5746 | 5971 | 6047 | 6120 |
| Node startup | 100 | 26.340 | 25 | 34 | 51 | 78 |
| Server shutdown diagnostics, including additional cases | 105 | 17.439 | 23.514 | 24.615 | 25.091 | 25.749 |

The additional official 100-call client stress completed with mean/p50/p95/p99/max = 49.979/49.982/51.772/52.467/54.430 ms, zero errors/timeouts. The 100 five-call cycles also completed without errors/timeouts. No `0xc0000005` occurred in completed 3L.2 runs. The earlier RemoteFunction fix is not reimplemented or reclassified merely from these runs.

The existing real-TLS official fixture confirms a GetContent request has entered the delayed provider before Server Stop. Cancellation is observed by the Node-side conditioned provider; the stopped Server reports zero admitted/stale content, and a fresh official Server subsequently loads/reloads normally. Stop order remains peer acceptance/GameSession -> authoritative Engine/content Stop -> provider cancellation and worker join -> runtime/provider destruction. Provider Stop cancels calls; native credential-string clearing occurs at provider destruction, not necessarily at content Stop.

The same fixture reruns Local/Node x FullyResident/OnDemand semantic digest equivalence; missing/wrong token, capability, tenant, manifest/package mismatch and corrupt payload rejection; Node outage with one already Resident unit; and a fresh same-port Server. Corrupt content admits zero authoritative units. Resident content survives Node loss without eviction. The outage survival subcase is Server-only: it is not a simultaneous 500-peer outage/movement/RPC proof.

Cancellation tests include 100 delayed acquisitions released before admission, legal-maximum preparation cancellation, ten Stop-during-request worlds, wrong package-version completion rejection, and malformed schema/object count rejection. Failed payload/dependency paths assert decoded current bytes return to zero. Stop/overload tests assert decoded bytes and retained records return to zero, in addition to root destruction. Separate per-failure and per-cancellation **RSS distributions** were not collected; LSan, ownership assertions and the lifecycle/pressure allocations provide the release evidence.

The 100-world same-process regression retains a second world while retiring each test world; that survivor's journal/render state is unchanged. The 100 legal-max content lifetimes and 1,000-cycle Server soaks are separate from full process cycles and must not be conflated.

Threads/handles remain bounded in the Windows 1,000-cycle traces:

| Profile | Thread min–max | Handle min–max | Whole-run RSS max MiB |
| --- | ---: | ---: | ---: |
| small-local | 3–18 | 57–154 | 46.855 |
| small-node | 1–22 | 43–173 | 51.176 |
| 100-local | 1–18 | 50–154 | 48.426 |
| 100-node | 4–22 | 58–171 | 53.094 |
| 256-local | 1–18 | 46–154 | 52.207 |
| 256-node | 1–22 | 36–171 | 56.738 |
| 512-local | 4–18 | 62–154 | 58.227 |
| 512-node | 1–22 | 34–173 | 62.559 |
| near-max-local | 1–18 | 49–154 | 70.020 |
| near-max-node | 4–22 | 56–171 | 74.293 |

Started Node processes are killed/waited by the existing Windows harness and tested for endpoint reuse; this proves reclamation, **not graceful Node signal shutdown**. Server/Player complete and are waited/disposed; failure paths stop only their owned processes. No retained package lock prevented temporary directory cleanup or later relocation/reuse. There is no dedicated OS lock-counter metric. Forced termination of a whole test runner can bypass its child cleanup; disposable container exit reclaimed the timed-out Linux run's children, and normal 100-cycle success is a different claim.

Coverage gaps retained explicitly:

- The scale workload waits for each phase to converge before eviction/reload. It proves fresh identities and old-state absence, not eviction while that same 500-peer region is still partially materialized.
- Existing 3J tests cover destroyed pending objects, cancelled pending selection, hard-reference indivisibility and connection generations. A new combined 500-peer disconnect-during-content-fan-out/corruption/capacity-failure matrix was not completed.
- The 64-unit memory-overload drain does not combine Character/actions/root-motion/RPC in the same run (items 94–95 remain unproven).
- Idle, provider-wait, parse-active, preparation and authoritative-commit **per-tick distributions** are not all independently attributable in the mixed scale harness. The report provides whole mixed phase distributions, the legal-max preparation/Resident/Main measurements and overload max Main step; it does not manufacture missing percentiles.

## Cross-platform, security and publication (items 97–107)

| Validation | Result |
| --- | --- |
| Worker MSVC 19.44 Release, Engine `a68f76b` | 52/52 CTests, 35.11 s, real GNS and official packaged hosts included |
| Worker Clang 19.1.1, ASan + UBSan + LSan `detect_leaks=1`, Engine `a68f76b` | 48/48 supported CTests, 168.98 s, executable tmpfs package scratch |
| Linux direct Node adapter/TLS + official headless Server | Three-test focused suite passed in 74.65 s; subsequent full-suite configuration/direct/headless checks also passed |
| Linux full Node native scale suite | First native allocator-instrumentation conflict corrected; 32/100 completed cleanly, 500 exceeded the original 20-minute runner deadline; full 60-minute-deadline rerun pending |
| Node Go build/test/vet/race | Local worker checks passed; opt-in native matrix is separate from ordinary `go test ./...` |
| Node generated protobuf consistency | No protocol/generated-source change; hosted CI check passed |
| Hosted Node `f444042` | [Final Go validation](https://github.com/gmoddev/gargantuan-node/actions/runs/34291742759) terminal green on retry, including build/test/vet/race/protobuf/pinned actions; earlier `f21c533` also green |
| Hosted Engine `a68f76b` | [Native Windows/Linux CI](https://github.com/gmoddev/gargantuan/actions/runs/34289903325): MSVC 19.50+ Windows job terminal green; dependent Clang 19 Linux job in progress at this checkpoint |
| Documentation `a68f76b` | [Documentation deployment](https://github.com/gmoddev/gargantuan/actions/runs/34289903445) terminal green |
| Scoped security | Sealed Engine 33-file and Node three-file exact-range reviews: zero reportable findings; follow-up test-only additions reviewed separately |

Linux CTest initially timed out packaging at its unchanged 240-second limit, twice, including a run without competing task stress. Moving disposable package scratch from the Docker Desktop Windows bind mount to executable tmpfs made the identical packaging binary pass in 138.57 seconds; the relocated smoke fell from 112.21 to 13.88 seconds. No test deadline or production limit was changed for this correction. A first tmpfs attempt used Docker's default noexec mount and was stopped after permission-denied relocation; it is retained as an environment setup failure, not an Engine defect.

The first hosted Node `f444042` run failed before compilation when the pinned protobuf download action hit GitHub's unauthenticated API rate limit. Rerunning that failed job passed without source, permission or check changes. The opt-in Linux 10,000-cycle harness remains a separately measured worker run; ordinary CI deliberately skips it when its trusted paths/output selector are absent.

The Node scale benchmark's global allocation override initially conflicted with ASan's native nothrow allocation path in gRPC. ASan builds now leave allocation interception to ASan and explicitly report that benchmark allocation counts are unavailable. There are no new suppressions. The original 20-minute Go deadline then expired at 500 peers without a sanitizer report; the test-only CMake deadline became 60 minutes / 3,660-second CTest outer bound. A killed/incomplete 500-peer run is **not** an LSan pass. Full rerun status is recorded before the final verdict.

The standalone Linux Engine suite uses the supported GNS-off sanitizer configuration. The Node build still exercises production TLS and official headless Server; its existing direct vertical omits the GNS session on Linux because of the separately documented upstream function-type sanitizer incompatibility. Windows supplies the actual GNS official-host path. No sanitizer option is disabled to mask a new content defect. Linux and Windows absolute RSS are not compared as interchangeable quantities.

### Security scope and conclusions

Exact sealed ranges:

- Engine `b54256805b1e4af8f8ad60a8672507ce26bb0be2..a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`, scan `20238180-faf5-4a1f-8330-cc5c5ec5febd`: all 33 changed files plus necessary consumers reviewed.
- Node `4e6bc0a8ff37c58a8a122262d746dd8934b71835..89fbffa1a2ddf29866e0c3d34bd3a8b6f1df94bb`, scan `a732c73f-692b-4dd0-80cb-98695df3885f`: all three changed files plus necessary consumers reviewed.
- Subsequent test deadline and opt-in Linux sampler are supplemental reviewed scope, not silently included in the immutable sealed ranges.

Source review follows configuration -> provider -> immutable bytes -> Engine verification -> bounded admission -> normal 3K/3H/3E/3J consumers. Ordinary client/package state still cannot select provider/endpoint/credentials, pin/retry arbitrary content or choose authoritative residency. Node supplies bytes, not ObjectId allocation, Instances, Desired/Known, Character authority or LocalPlayer. Exact package version/digest and generation/demand/cancellation checks remain at Engine admission. Outage does not revoke Resident authority; exhausted budgets defer/fail closed. Negotiated structural byte slicing preserves whole hard-reference/critical groups, and accepted Known/cursor/sequence state commits only after scheduler acceptance.

The newly exposed controls are trusted host/native test controls, not a game protocol: decoded aggregate metrics, private final-world retirement helpers, bounded Server churn count, test-only safe-point validation/read access, and native benchmark/Go environment selectors. Player rejects Server/Node role flags as before. No Engine -> Server dependency or ordinary Luau streaming API was added.

New diagnostics contain aggregate counts/bytes/timing. Server token values flow only from a trusted environment reference to provider/gRPC metadata over TLS; fixed transport status descriptions do not include remote credential strings. Configured Node token variables are removed from official Player processes. Harness logs are raw captured streams relying on runtime sanitization, not a blanket redaction layer. The Linux sampler additionally refuses to retain output containing any of its fixture credentials. This is not comprehensive inherited-environment allowlisting or a production deployment ACL audit.

The security tool's reported usage is 16,179,580 total tokens (15,323,776 cached input) for the Engine scan and 8,039,690 (7,607,680 cached input) for Node. These rollout-derived totals include overlapping task history/workers and **must not be added as independent incremental scan costs**. Canonical reports, coverage, models and SARIF are retained in local task evidence. No plausible candidates required vulnerability validation/attack-path adjudication; zero findings is not a proof of no vulnerabilities.

## Remaining blocker: near-limit official Player

The 512-object / 1,048,197-byte network-first official Player case timed out after three minutes with **both Local and Node**. Server acquisition/admission and reload completed, but Player did not finish the workload. Pipes are drained asynchronously from launch; this is not attributed to blocked redirected output.

A non-invasive Release Player stack capture was symbolized using a linker map whose executable .text SHA-256 exactly matched the packaged binary. Main was in scalar allocation/string copy -> EncodeNativeWireValue -> EncodeCommittedProperty -> PublishReplicationSubtree -> SetParent -> LoadSnapshot -> ReplicaApplier::ApplyFrame -> GameSession::Poll -> RunPackagedPlayer. Source confirms each incremental frame copies SemanticState and reconstructs a temporary snapshot for validation. This establishes an observed client materialization cost path, not an exclusive root-cause proof or a Node transport defect. No replica-validation bypass or broad refactor was introduced to make the test pass.

## Evidence and reproduction

The local task evidence directory is `build-3l2-worker/evidence` under the authoritative Engine checkout. Worker raw logs are under `C:\Sandbox\Codex\Logs\gargantuan-3l2`; native incremental builds and dependency caches remain under the task's `Builds` and `Cache` directories. They are validation artifacts, not runtime distribution inputs.

| Result | Retained evidence |
| --- | --- |
| Windows 1,000-cycle raw traces | `rss-long-small/`, `rss-long-100/`, `rss-long-256/`, `rss-server-only/` and corresponding `*-summary.json` |
| Aligned short startup runs | `rss-clock-aligned/`, `rss-clock-aligned-summary.json` |
| Linux 10,000-cycle raw traces | `rss-linux-long/`, `rss-linux-long-summary.json`, `rss-linux-official-long.log` |
| Overload ownership/drain | `baseline-decoded-pressure.log`, `bounded-decoded-pressure.log`, `decoded-overload-final-metrics.log`, `decoded-overload-world-release.log` |
| Current Windows 48-phase scale matrix | `scale-measured-summary.json` and its exact per-row `Source` log |
| Official 100-process cycles | `official-lifecycle-100.log` |
| Near-limit Player failure/stack | `rss-near-network-final/`, `rss-near-stack/`, `near-limit-player-stack.log` |
| Final Engine suites | `ctest-windows-a68.log`, `ctest-linux-a68-tmpfs-exec.log` |
| Security sealed reports | `security-engine/`, `security-node/`: report, coverage, findings, manifest and SARIF; architecture-review JSON beside them |
| Final worker source fidelity | `source-hash-verification.json`: all 28 scoped Engine native/build/test files plus four Node harness/build files match the local source bytes by SHA-256 |

Do not aggregate every historical log into the current scale result. The selected sources are:

```text
scale-local-512objects-current-32.log
scale-local-512objects-current-100.log
scale-local-500-bounded-input.log
scale-node-512objects-current-32-100.log
scale-node-512objects-bounded-input.log
scale-local-100objects-current-32.log
scale-local-100objects-current-100.log
scale-local-100objects-current-500.log
scale-node-100objects-bounded-input.log
```

Use `tests/AnalyzeContentScale.ps1 -InputFiles <selected files> -OutputFile <output.json>` and `tests/AnalyzeContentMemory.ps1 -InputDirectory <raw profile directory> -OutputFile <output.json>` to regenerate summaries. AnalyzeContentMemory was rechecked against both Linux output (missing private counters correctly null) and the existing Windows small profile (unchanged memory distributions/slopes).

Native reproduction entry points are `gargantuan_content_availability_benchmark --decoded-pressure`, `--decoded-overload` and `--lifecycle`; `gargantuan_game_session_benchmark --write-content-scale <package-dir> <100-or-512>` followed by `--content-scale <32-or-100-or-500> <package-dir> local`. Append `--input-every-tick` for the separate full-rate input diagnostic. TLS Node uses the native target `gargantuan_node_content_scale` through `go test ./internal/host -run '^TestContentScaleRealTLS$' -count=1 -timeout 60m -v`; `GARGANTUAN_CONTENT_SCALE_TEST_EXE` and `GARGANTUAN_OFFICIAL_NODE_HOST_NODE` select trusted built executables. `GARGANTUAN_CONTENT_SCALE_OBJECTS` selects 100 or 512. The Go fixture generates its own ephemeral test TLS/auth configuration.

Windows RSS uses `TestOfficialContentMemorySoak` with trusted `GARGANTUAN_OFFICIAL_NODE_HOST_*` runtime/packager/project/script paths, `GARGANTUAN_CONTENT_MEMORY_OUTPUT`, `GARGANTUAN_CONTENT_MEMORY_CYCLES=1000` and `GARGANTUAN_CONTENT_MEMORY_SERVER_ONLY=1`. Linux uses `TestOfficialContentMemoryLinux`, the Node/packager/Server-distribution/project path subset and `GARGANTUAN_CONTENT_MEMORY_LINUX_OUTPUT`; it fixes the measured count at 10,000 and has a 20-minute per-Server deadline. The full Windows lifecycle uses `TestOfficialRuntimeHostsRealTLSVertical` with `GARGANTUAN_OFFICIAL_HOST_LIFECYCLE_CYCLES=100` and a 30-minute outer Go test deadline. No production credential is needed or copied into these fixtures.

## Acceptance report index (all 110 requested items)

Detailed numerical distributions are above. “Not established” is an explicit coverage result, not an implied pass.

| Item | Result / evidence |
| ---: | --- |
| 1 | Engine baseline: b54256805b1e4af8f8ad60a8672507ce26bb0be2. |
| 2 | Node baseline: 4e6bc0a8ff37c58a8a122262d746dd8934b71835. |
| 3 | Engine implementation end: a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1; documentation/analyzer-only descendant publication is identified in the final handoff. |
| 4 | Node ending HEAD: f4440423c0701ff396f589fd51b6ce41edc63539 (Linux sampler); preceded by reviewed harness 89fbffa1a2ddf29866e0c3d34bd3a8b6f1df94bb and test deadline f21c5331ee2be7168e53448a3283662eec112f4c. |
| 5 | No intervening remote commits at the initial pull/HEAD verification; subsequent commits are the scoped work listed here. |
| 6 | Decoded retained ownership, final-world scope retirement, FullyResident drain, negotiated byte slicing, safe coarse-coordinate conversion; no new residency policy. |
| 7 | Memory/scale/gameplay/lifecycle fixtures, current-revision TLS matrix, official process sampler/analyzers, bound/retirement/hard-reference regressions. |
| 8 | Shared 16 MiB decoded charge; current/HWM/deferral counters plus detached/resident-origin object counts, not invented byte estimates. |
| 9 | First sampled process RSS is tabulated as startup evidence, not mislabeled idle-running RSS. |
| 10 | Separate clock-aligned post-bootstrap RSS table. |
| 11 | Separate clock-aligned post-first-load RSS table. |
| 12 | Warm-up high-water in per-profile RSS table. |
| 13 | Steady mean in per-profile RSS table. |
| 14 | Steady p50 in per-profile RSS table. |
| 15 | Steady p95 in per-profile RSS table. |
| 16 | Steady p99 in per-profile RSS table. |
| 17 | Whole-run max in thread/handle table; steady maximum separately identified. |
| 18 | Final-drain mean in per-profile RSS table; not an after-exit measurement. |
| 19 | Second-half OLS slope in bytes/second; uncertainty and positive slopes retained. |
| 20 | All ten Windows profiles show bounded plateau over 1,000 cycles; longer Linux result separately scoped. |
| 21 | Windows PrivateUsage mean/p50/p95/p99/max/drain/slope table; Linux private memory explicitly unavailable. |
| 22 | One RAII retained-document owner shared without double counting across worker/completion/Available/Main. |
| 23 | Available decoded HWM 16,151,680 bytes in the draining 64-unit overload. |
| 24 | Raw completion HWM 3,154,824 bytes in that overload; exact completion-capacity regression also rerun. |
| 25 | Encoded cache HWM 16,825,728 bytes. |
| 26 | Detached preparation HWM 512 objects; bytes not independently measured. |
| 27 | Resident package-origin HWM 32,768 objects; metadata bytes not independently measured. |
| 28 | 100 delayed cancellations plus legal-max cancellation and clean release/Stop checks; no cancellation-only RSS distribution. |
| 29 | Invalid shape/schema/count and failed dependency return decoded bytes to zero; corrupt content admits zero. |
| 30 | Stop decoded/current records zero; tracked/RSS attribution distinguishes service, final world and process infrastructure. |
| 31 | Five Local Windows profiles x 1,000 cycles; explicit no-Node local composition remains supported. |
| 32 | Five TLS Node Windows profiles x 1,000 cycles; optional Linux sample-entry extended soak recorded separately. |
| 33 | 32 peers: one acquisition in every completed Local/Node/content-size case. |
| 34 | 32 peers: one admission on load, two cumulative on reload. |
| 35 | 32/512 pending HWM 8,192; 32/100 objects zero pending in sampled completed matrix. |
| 36 | 32/512 current Node max load 286.993 ms/7 ticks, reload 343.401 ms/4 ticks; Local 266.767/4 and 290.402/4. |
| 37 | Full baseline/load/evict/reload mean/p50/p95/p99/max tick table. |
| 38 | 100 peers: one acquisition in every completed combination. |
| 39 | 100 peers: one load admission, two cumulative reload admissions. |
| 40 | 100/512 pending HWM 43,008; 100/100 objects 1,808. |
| 41 | 100/512 Node load 1,065.6 ms/8 ticks, reload 1,014.36/8; Local 965.117/8 and 1,046.26/8. |
| 42 | Full 100-peer phase tick distributions above. |
| 43 | 500 peers: exactly one acquisition, including after reload. |
| 44 | 500 peers: one authoritative load admission, fresh second admission on reload. |
| 45 | 500/512 pending HWM 247,808; 500/100 objects 41,808. |
| 46 | Selected global maximum exactly 8,192, never greater; no raised production ceiling. |
| 47 | 500/512 Node load convergence p50/p95/p99/max 5,073.4/9,130.27/9,965.5/10,126.1 ms; all provider/size quantiles above. |
| 48 | Maximum journal lag 13,961 records, including admission, below 16,384 retention. |
| 49 | Zero journal failures in all completed representative combinations. |
| 50 | Full 500-peer baseline/load/evict/reload tick distributions; worst measured mixed tick 1,497.87 ms. |
| 51 | 500-peer process RSS HWM reaches 417,714,176 bytes (Node/512); not a Server-only or content-only RSS number. |
| 52 | 500/512 Node reload converges by 10,667.4 ms/33 ticks; Local 10,216.3/33; fresh identities and cached bytes. |
| 53 | Old Server root unregistered, old spatial projections absent, old client root absent after reload; pending-race limitation explicit. |
| 54 | 500/512 Node baseline Character 417,670 states/s simulated, 26,133.3 states/s wall. |
| 55 | 500/512 Node load Character 419,933 states/s simulated, 24,287 states/s wall; complete table above. |
| 56 | 500/512 Node baseline Character 31,046,000 bytes/s simulated. |
| 57 | 500/512 Node load Character 31,205,500 bytes/s simulated; not real-time network throughput. |
| 58 | Maximum observed Character publication gap 12 simulation ticks; wall-gap percentile unavailable. |
| 59 | Zero scheduler rejections in completed bounded-input matrix; separate full-rate receive queue exhaustion retained. |
| 60 | Accepted owner inputs continue, but 500-peer responsiveness fails the healthy-control envelope. |
| 61 | Actions resolve/complete, but 500-peer submission failures prevent health claim; all phase counts above. |
| 62 | Maximum observed action-result diagnostic 2,335.46 ms; cumulative timing, not a fresh per-phase percentile. |
| 63 | Native end events continue; no unexpected reason/rejection, but counts are not one-to-one with the gameplay client's starts. |
| 64 | Real Animator root-request counts per phase above (e.g. Node/512/500 load 3,220). |
| 65 | Every produced root request in completed runs commits (3,220/3,220 in that example). |
| 66 | Node/512/500 load root GCHR states 282,750; full per-profile counts above. |
| 67 | Root publication maximum gap 12 simulation ticks in completed runs. |
| 68 | Safe-point current-cell checks pass with coarse-boundary crossings; old evicted projections absent. |
| 69 | RemoteEvents continue each phase and official cycle; no dedicated event-latency percentiles. |
| 70 | 100-call no-streaming mean/p50/p95/p99/max/timeout/error table. |
| 71 | 100-call load/evict/reload equivalent table; material p99 degradation remains. |
| 72 | Zero access violations in completed 3L.2 runs; near-limit Player stall is a timeout, not counted as a crash. |
| 73 | 100 full official Node/Server/Player cycles, plus failure/equivalence subcases, 665.251 seconds. |
| 74 | 100 same-process legal-max content lifetimes and 100-world survivor regression; ten Windows x 1,000 and two Linux x 10,000 official Server churn profiles separately reported. |
| 75 | Real-TLS delayed request enters provider before official Stop; cancellation observed and fresh Server succeeds. |
| 76 | Existing generic pending-selection/generation tests pass; a new combined 500-peer content disconnect-during-fan-out run remains missing. |
| 77 | Official Resident-survival outage subcase passes without eviction; not a simultaneous 500-peer outage gameplay proof. |
| 78 | 100 acquisition cancellations, legal-max cancelled preparation and ten Stop-during-request worlds pass. |
| 79 | 100 cycles reuse Node TCP/Server UDP endpoints; explicit same-port restart also passes. |
| 80 | No owned process/thread leak observed on normal fixture completion; arbitrary whole-runner kill cleanup is not established. |
| 81 | No retained package lock prevents teardown/reuse; no independent kernel lock-counter metric. |
| 82 | Fresh process/provider/channel per official cycle; Stop cancellation/join and endpoint reuse checks pass. |
| 83 | Resident Parts have current projections; evicted old generation has none; safe-point validation active in scale tests. |
| 84 | Resident-only spatial index and root-motion cell crossings validate; no manifest ghost injection. |
| 85 | 3E remains sole relevance authority; trusted focus inputs are normal policy inputs, not direct Desired edits. |
| 86 | 3J remains sole structural scheduler; negotiated byte cap and hard-reference indivisibility regressions pass. |
| 87 | 64-unit overload pending HWM 48, in-flight HWM 16; independent 1,024 pending-key exact-bound test passes. |
| 88 | Overload raw completion HWM 3,154,824 bytes. |
| 89 | Overload encoded cache HWM 16,825,728 bytes. |
| 90 | Overload decoded/Available HWM 16,151,680 bytes, 59 deferrals, bounded by 16 MiB. |
| 91 | Overload RSS peak 341,024,768 bytes; private Resident 340,127,744 bytes. |
| 92 | 64 units drain in 70 ticks / 3,057.12 ms with 64 acquisitions and admissions. |
| 93 | Maximum overload Main content Step 69,823 microseconds. |
| 94 | Not established for simultaneous 64-unit overload; gameplay is measured separately with shared-region fan-out. |
| 95 | Not established for simultaneous 64-unit overload; each scale phase separately executes 100 RPCs. |
| 96 | 512 objects/exact 1 MiB native lifetime passes; near-limit 1,048,197-byte official network Player stalls with both providers. |
| 97 | ASan clean for completed Engine/Node focused suites; complete 500-peer native run must finish before claiming the whole matrix clean. |
| 98 | UBSan float-to-int boundary defect reproduced and corrected; completed reruns clean; full matrix status above. |
| 99 | LSan detect_leaks=1 enabled, no new suppressions; timed-out processes are not counted as leak-clean exits. |
| 100 | Node worker Go race passes; hosted Node CI race green for published harness, supplemental test rerun recorded separately. |
| 101 | Windows worker 52/52, 35.11 s; hosted MSVC 19.50+ result in publication table. |
| 102 | Linux supported Engine 48/48, 168.98 s with executable tmpfs; Windows-bind scratch timeout retained as environment evidence. |
| 103 | Hosted Node build/test/vet/race/generated protobuf/pinned-actions green; native opt-in integration coverage is a separate worker result. |
| 104 | Published a68 documentation deployment terminal green; final measured-document publication referenced in handoff. |
| 105 | Two sealed scoped security reviews, zero findings; supplemental test/analyzer review separate, with stated limitations. |
| 106 | Published Engine implementation a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1 plus final documentation/analyzer commit in handoff. |
| 107 | Published Node 89fbffa1a2ddf29866e0c3d34bd3a8b6f1df94bb, f21c5331ee2be7168e53448a3283662eec112f4c and f4440423c0701ff396f589fd51b6ce41edc63539. |
| 108 | B — Foundation 3L remains partially ready; structural/memory success does not override gameplay blockers. |
| 109 | No. Foundation 3M remains gated. |
| 110 | Bound/attribute incremental replica validation/materialization, fix near-limit Player regression and 500-peer baseline/action receive pressure, then rerun mixed overload/pending-failure envelope without policy or raised limits. |

## Required direct answers

1. Yes in the measured Server-only workloads: all ten 1,000-cycle Windows profiles and both 10,000-cycle Linux sanitizer profiles settle into bounded ranges. Finite runs do not prove arbitrary-duration absence of leaks.

2. Windows steady RSS: Local 46.668–69.215 MiB across profiles; Node 51.176–74.293 MiB. Linux sanitizer small-entry steady RSS is 487.074–505.961 MiB Local and 506.258–519.270 MiB Node. Per-profile ranges, rather than their union, are the plateau evidence; platforms/instrumentation are not directly comparable.

3. Some second-half fitted slopes are positive; largest Windows value is about 3,957 bytes/s. Linux Local is +2,314 bytes/s and Node -522 bytes/s. Near-flat steady-quarter means and bounded extrema support measured post-warm-up stability, not a literal zero-slope assertion.

4. Parsed/decoded Available JSON documents retained outside the encoded payload cache/completion accounting.

5. Yes. One shared retained-document charge spans worker/completion/Available/Main owners, with default/hard 16 MiB ceiling. Transient parser storage and allocator overhead are separately bounded, not included in that counter.

6. Yes for the exercised cancellation/lifetime checks; cancelled work cannot admit, RAII drops prepared owners and Stop joins workers. No separate cancellation-only RSS distribution was measured.

7. Yes. Malformed/schema/count/dependency failures return decoded current bytes to zero; no failed content is authoritatively admitted.

8. Yes for session-owned content: decoded/cache/completion/manifest/record storage is released after cancellation/join. Process allocator/registry capacity and a separately owned live world's journal are not required to become zero RSS.

9. One shared acquisition, not one per peer. Reload remains one cumulative acquisition by using verified cached bytes.

10. One authoritative region; reload creates a new lifetime of that region, not 500 Server copies.

11. Yes: selected work never exceeds the configured 8,192 default in the measured matrix; indivisible over-limit hard-reference groups fail closed.

12. Node/512/500 load: p50/p95/p99/max 5,073.4/9,130.27/9,965.5/10,126.1 ms, 17/31/32/33 ticks. Reload max 10,667.4 ms/33 ticks. The Node/100-object case has a longer 12,554.1 ms/69-tick tail.

13. Yes in completed representative runs: max 13,961 records versus 16,384 retention, zero journal failures.

14. Progress continues with no scheduler rejections, but no: healthy wall-time responsiveness is not established. The 500-peer baseline is already slow and streaming adds stalls.

15. Twelve simulation ticks in the completed matrix. No separately measured worst wall-time publication gap; slow ticks mean this is not a guaranteed 200 ms wall bound.

16. No at the required envelope. Submission failures occur at 500 peers, including no-streaming baseline and reload; maximum result latencies are reported above.

17. Authoritative progress and matching commits continue, but real-time health is not established at 500 peers. Do not substitute produced-count equality for responsiveness.

18. Yes in the exercised safe-point checks: moving root-motion Characters cross coarse cells; current 3K/3H state validates, and evicted old content has no remaining projection.

19. RemoteEvents continue in all completed phases and the official vertical; no event-stall assertion fires. A dedicated RemoteEvent latency percentile was not measured.

20. The known 100-call case remains passing: 100 calls per phase, zero timeouts/errors and zero access violations. This is not a guarantee for an unbounded concurrent request flood.

21. Yes. For Node/512/32, p99 rises from 19.802 ms baseline to 268.929 ms load; for Node/512/500, 1,162.820 to 1,884.834 ms. Provider source does not remove the degradation.

22. Yes for the measured 64-unit overload: decoded HWM 16,151,680 bytes within 16 MiB; encoded/completion/work queues remain bounded. Total RSS has a different, measured bound.

23. Yes: 64 units drain in 70 ticks / 3,057.12 ms with 59 decoded deferrals, 64 acquisitions and 64 admissions, no refetch or fail-open admission.

24. Yes in the Windows long-run profiles: drained means return near each established range. After full world teardown the independent overload RSS drops further; allocator RSS is not forced to zero.

25. No accumulating lifecycle-proportional leak observed in the completed 1,000-cycle Windows traces or 100 same-process legal-max lifetimes. This is bounded empirical evidence, not proof for all durations.

26. No surviving task processes/ports/package locks observed in the 100 successful official cycles. They use forced Node termination; this does not establish graceful Node shutdown or every forced-runner failure path.

27. Yes: all 100 official cycles reuse the same Node TCP and Server UDP endpoints, and explicit same-port Server restarts pass.

28. No in the tested generation/identity checks: old roots are destroyed/unregistered, old projections absent, reload obtains a fresh ObjectId and wrong-version completions fail.

29. No in tested connection/materialization generation checks and fresh reload observation. The combined 500-peer disconnect-with-content-still-pending variant remains unmeasured.

30. Windows and supported Linux Engine suites are clean; Node Go race CI is green. Full Linux 500-peer ASan/UBSan/LSan completion is pending at this draft; the timed-out earlier run is not a sanitizer pass.

31. No. Memory and structural bounds improve and measured subgates pass, but gameplay latency/actions, the near-limit official Player stall and coverage gaps prevent A.

32. No. Foundation 3M stays gated until these substrate failures are corrected and the full acceptance envelope is rerun.

## Final acceptance

**B: Foundation 3L remains partially ready. Foundation 3M may not begin.** Terminal CI/sanitizer results are recorded above; a green build does not change the measured gameplay verdict.

Exact next recommendation: first attribute and bound incremental replica-application validation/materialization cost while preserving transactional semantic validation and hard references; reproduce the near-limit official Player stall with a deterministic regression. Then address the already slow 500-peer no-content baseline/receive service and owner-action scheduling, and rerun the same bounded-input and full-rate diagnostics. Add the missing combined pending-fan-out failure and 64-unit mixed-gameplay overload cases before reevaluating A. Do not implement automatic spatial residency to conceal substrate failures.

B — FOUNDATION 3L REMAINS PARTIALLY READY
