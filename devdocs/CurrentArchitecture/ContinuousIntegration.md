---
status: current
owner: build-and-test
last_verified: 2026-10-06
related_code:
  - .github/workflows/native-ci.yml
  - CMakeLists.txt
  - rokit.toml
  - tools/classgen.luau
related_adrs: []
---

# Continuous native build and test contract

## Supported CI configuration

`.github/workflows/native-ci.yml` is the required native engine check for every
push, pull request, and manual dispatch. Its first gate is Windows x64 on
GitHub's explicit `windows-2025-vs2026` image. It matches the production
checkpoint's Visual Studio 2026/MSVC 19.50-or-newer ABI family and uses the
image's CMake and Ninja installations. A dependent Ubuntu 24.04 job then builds
and runs the complete headless contract with Clang 19, ASan, and UBSan.

The repository has no checked-in CMake preset. CI therefore records its complete
single-configuration Ninja contract directly:

```text
CMAKE_BUILD_TYPE=Release
BUILD_TESTING=ON
GARGANTUAN_TRACY=OFF
GARGANTUAN_WITH_GNS=ON
GARGANTUAN_BUILD_RENDERER_BENCHMARKS=ON
GARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS=ON
GARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF
GARGANTUAN_GLSLC_EXECUTABLE=<discovered pinned Vulkan SDK glslc.exe>
```

The Windows vcpkg toolchain runs its app-local dependency copy after each link.
Those copies share an output directory, so CMake assigns Windows Ninja links
to a one-job pool when app-local deployment is enabled. This preserves parallel
compilation while preventing concurrent app-local copies from racing on a DLL.

Filament and the Glaze prototype are not part of either gate. The Windows job
builds the normal engine, SDL renderer sources, shaders, pinned
GameNetworkingSockets adapter, and native regression targets. The Linux job
builds the same production renderer/core sources but deliberately disables GNS
and hardware renderer benchmarks so the sanitizer gate remains headless. This
is demonstrated Linux sanitizer coverage, not general Linux product support;
the workflow makes no macOS CI claim.

## Fresh-checkout bootstrap

The Windows complete CTest pass includes the funded structural ACK matrix,
ordinary/FULL_RESERVATION and mixed FIFO isolation, simultaneous four-grant
F1/pool verification, and one paired control/funded native statistics-boundary
test. The latter observes actual pinned tracer and instantaneous-report paths
over 32 seconds per arm; it does not claim 120-second lifetime path execution.
It runs once, rather than inside every workload case. The GNS sanitizer job
selects the separate failed-send retry-safety test for segment/message ownership,
mixed FIFO, fault and exact conservation coverage without asserting healthy F1
wall-clock timing under injected faults. The strict funded F1 matrix remains
in Windows Release. No new CI gate is considered passed until its own execution
is terminal green.

All three CTest invocations write JUnit to an absolute path rooted at
`GITHUB_WORKSPACE`. CTest changes its working directory to `--test-dir`, so
passing another build-directory-relative JUnit path would create a nested file
outside the artifact upload list. Each successful test step also requires its
exact upload-path XML to exist and be nonempty. The diagnostics upload remains
`if: always()`; a green test summary without retained raw JUnit is incomplete
qualification evidence. The small Python regression executes real CTest against
temporary passing/failing tests and checks these workflow/output boundaries.

The workflow deliberately supports a cold runner:

1. `actions/checkout` initializes every Git submodule recursively and a
   preflight rejects missing, conflicted, or wrong-revision submodules.
2. The pinned Rokit release installs the exact tools in `rokit.toml`, including
   Lute 1.0.0. Rokit's cache is an optimization only.
3. Visual Studio's `clang-format` is located and `lute tools/classgen` generates
   the ignored class/service reflection sources before CMake configure.
4. The pinned Chocolatey Vulkan SDK package provides `glslc`; the executable is
   located, version-probed, and passed to CMake explicitly.
5. `vswhere` locates the x64 Visual Studio C++ workload and each configure/build
   step enters that installation's developer environment.
6. The runner's vcpkg toolchain restores Protobuf from `cmake/gns/vcpkg.json`,
   whose builtin baseline is pinned, and CMake fetches GameNetworkingSockets at
   the immutable revision declared in `cmake/GameNetworkingSockets.cmake`.

`CMakeLists.txt` also checks representative files from every required top-level
dependency and every expected output derived from the class/service declarations.
Missing dependency content
reports the recursive submodule command. Missing generated content reports the
Rokit, `clang-format`, and class-generator commands. Existing shader discovery
continues to reject a missing, stale, wrongly named, or non-executable `glslc`.
CMake itself owns missing compiler diagnostics.

## Enforced tests

CI builds the default Release target graph, including `gargantuan`, the shader
outputs, and registered test executables, then runs:

```text
ctest --test-dir build-ci -C Release --parallel 2 --timeout 300
      --output-on-failure --no-tests=error
```

Shader compilation and runtime staging are distinct build obligations. The
default `gargantuan_stage_shaders` target copies every generated shader into the
executable's `shaders` directory on every incremental build, even when the
executable does not relink. `gargantuan_packaged_shaders` compares that runtime
directory with the complete source shader set and fails when a compiled output
is missing.

The Windows production contract runs the complete registered matrix. The Linux
sanitizer configuration with `GARGANTUAN_WITH_GNS=OFF` and renderer benchmarks
disabled omits platform/real-transport and renderer-headless entries; that
reduced matrix is not the complete Windows gate. Current CMake registration,
rather than a duplicated numeric count in this document, is authoritative.

| Coverage | Existing CTest entries |
| --- | --- |
| Shader requirement | `gargantuan_shader_tool_validation` |
| Runtime shader package completeness | `gargantuan_packaged_shaders` |
| Foundation/runtime, persistence and serialization, EditorHost/protocol, render extraction and backend boundary | `gargantuan_foundation` |
| Asset import, canonical artifact, dependency graph, and runtime materialization | `gargantuan_asset_foundation` |
| Skeletal import/artifacts, playback/blending, CPU/GPU/headless pose equivalence, semantic Attachment/Sound/Prompt anchors, reimport/lifecycle/journal behavior, and bounded Release scaling/50K smokes | `gargantuan_animation_foundation`, `gargantuan_animation_foundation_benchmark_smoke` |
| Optional telemetry dynamic loading, ABI negotiation, consent, privacy, and fail-open lifecycle | `gargantuan_optional_telemetry` |
| Physics | `gargantuan_physics_backend` |
| Soft-body runtime and bounded Release smoke | `gargantuan_soft_body_physics`, `gargantuan_soft_body_physics_benchmark_smoke` |
| Platform input | `gargantuan_platform_input_boundary` |
| Player runtime | `gargantuan_player_runtime` |
| Provider-neutral entitlement semantics, Luau authority, vectors, headless lifecycle, and provider overhead smoke | `gargantuan_entitlement_service`, `gargantuan_entitlement_provider_benchmark_smoke` |
| GUI retained runtime and bounded Release smoke | `gargantuan_gui_foundation`, `gargantuan_gui_foundation_benchmark_smoke` |
| PreRun bootstrap | `gargantuan_prerun_bootstrap` |
| Serialization smoke | `SerializationBenchmarkSmoke` |
| Networking contracts and deterministic transport | `gargantuan_networking_contracts`, `gargantuan_simulated_transport`, `gargantuan_scheduler_contract` |
| Replication and remotes | `gargantuan_replication`, `gargantuan_remote`, `gargantuan_remote_luau` |
| SpatialPose/SpatialCellAddress separation, space generation/isolation, projection lifecycle/transfer, membership, candidate, false-negative, boundedness, and Release scale proof | `gargantuan_spatial_region_index`, `gargantuan_spatial_region_index_benchmark_smoke`, `gargantuan_replication_relevance` |
| Networking bounded-load smokes | `gargantuan_replication_benchmark_smoke`, `gargantuan_remote_benchmark_smoke` |
| Real transport lifecycle and remotes | `gargantuan_real_transport`, `gargantuan_remote_real_transport` |
| Renderer extraction/projection and renderer interface | `gargantuan_renderer_projection_headless`, `gargantuan_renderer_backend_headless` |
| Renderer Foundation 2B dirty coalescing/bounds, publication lifecycle, deformable ranges, and UI batches | `gargantuan_renderer_publication` |
| Environment publication/application scaling, ClockTime, fog, exposure, and Sky reimport smoke | `gargantuan_environment_lighting_benchmark_smoke` |
| Package format, integrity, relocation, atomicity, cancellation, asset closure, shared packaged-world bootstrap, and packaged gameplay | `gargantuan_packaging` |
| EditorHost package authority, revision, conflict, Play exclusion, progress, and cancellation | `gargantuan_packaging_editor_host` |
| Package capture/build bounded-load smoke | `gargantuan_packaging_benchmark_smoke` |
| FirstCompleteGame headless Play/Stop and semantic animated-anchor proof | `gargantuan_first_complete_game` |
| FirstCompleteGame CLI build/validate/inspect, relocation, dedicated player startup, animated-anchor asset closure, and corruption rejection | `gargantuan_first_complete_game_package_smoke` |

CTest labels mirror those coverage groups so developers can select a subsystem,
but CI runs the complete registered set rather than maintaining a second list of
test executables.

## Headless boundary

Every registered test in this configuration runs without a display or GPU.
Player-runtime tests use `HeadlessRenderer`. The renderer smoke entries invoke
the existing renderer benchmark with `projection` and `headless` backends; those
paths do not initialize SDL video or create an SDL GPU device. Foundation tests
exercise render extraction, renderer ownership, viewport picking, and
EditorHost protocol behavior using CPU-side state.

`gargantuan_editor_viewport_smoke` is built but not registered because its
purpose is offscreen pixel capture and its implementation creates an SDL GPU
device. The SDL and optional Filament GPU tests likewise remain outside the
headless gate. A future GPU-capable runner may add them as a distinct job; they
must not be reported as having run on the current headless worker.

## Failure artifacts and hardening

The offline CI reader consumes only bounded JUnit and source-marker members
from the byte-pinned diagnostics ZIP. It retains the 128-MiB compressed bound,
10,000-member cap, all path/duplicate checks and required-member bounds; it
does not reject the aggregate expanded size of members it never reads.
Five raw scheduler trace sets may legitimately exceed the historical
256-MiB aggregate limit without increasing the reader's decoded inputs.
Raw archives remain unchanged. A separately qualified offline analyzer B may
apply this correction to qualified execution source A under
`tests/Foundation3LFinalAcceptance.md`; it does not claim B's native runtime
or hosted jobs were qualified by A's results.

The standalone Release reliable-workload gate emits per-case
`[Qualification:WorkloadTiming]` maxima for step intervals, each runtime step,
session poll and session step, fixture observation, and sleep overshoot. These
are wall times and can include host preemption; they do not by themselves
attribute a stall to production CPU work. They reset between cases, unlike the
session's lifetime remote-service-gap high-water. All RPC/Event/action latency
gates remain unchanged: a host timing hypothesis does not turn a failed run into
a PASS or permit its samples to be discarded.

The bounded `[Qualification:WorkloadCpu]` rows supplement those maxima with the
case, step, phase, wall duration, current-thread CPU delta, and whole-process CPU
delta from the **same span that established that phase's maximum wall time**.
Windows counters use `GetThreadTimes` and `GetProcessTimes`; unsupported or failed
reads are `NOT_MEASURED`, not zero. The step-interval row also retains the prior
step's requested and actual sleep durations. These fixed-size records are printed
after the case, without logging on the measured per-step path. CPU counters are
quantized diagnostics and never replace the elapsed-time latency samples. Small
thread CPU with large wall time establishes time not spent on that thread's CPU;
it does not by itself distinguish blocking from preemption. Process CPU can
exceed wall time when several process threads execute concurrently.
Windows CPU accounting may also advance in coarse quanta (15.625 ms was
observed on the qualified worker). A short span can consequently report a
thread CPU delta larger than its wall duration, such as 31.25 ms of accounted
CPU for a 22.67 ms span, even though a single thread cannot execute on multiple
cores simultaneously. Preserve that raw measurement; do not clamp it or treat
it as a utilization ratio. Attribute long stalls using spans large enough to
distinguish their CPU consumption from this quantization, and use scheduler
tracing when the counters cannot resolve the question. CPU counter values and
their observed granularity never alter any service or latency gate.

Fixture-only `ClockAnchor`, `RemoteChronology`, `RemoteSpan` and
`WorkloadPhaseSpan` records bind existing RPC/Event identities and their
original latency timestamps to native thread IDs and bracketed QPC/steady-clock
anchors. Accepted, rejected, terminal and unobserved submissions remain
distinct. Maximum phase spans retain paired start/end timestamps from the same
observation. The fixed diagnostic buffer reports invalid or overflowing
evidence explicitly, and prints after the case. These records enable exact
correlation with separately collected scheduler events; they do not themselves
establish a wait reason, change the workload or replace wall-clock acceptance.

Ordinary qualifying push/PR and default dispatch retain the complete Windows
CTest/tooling matrix and all five unchanged standalone workloads. The fixed
`Full` scheduler case wraps the third `--reliable-workload` invocation and
`Aggregate32` wraps the fourth `--reliable-workload-32` invocation and
`Aggregate32Structural` wraps the fifth `--reliable-workload-32-structural`
invocation, each exactly once in its original logical order. Their owned bounded
ETL, decoded events, child logs, metadata and cleanup evidence are always
retained when reached. `PooledAggregate32Structural` now wraps the second
`--pooled --reliable-workload-32-structural` invocation with the same bounded
controller. `PooledFull` wraps the first `--pooled --reliable-workload`
invocation through that same controller after its real untraced Event/action
failure. All five
logical invocations, dependent Linux and GNS gates
remain mandatory. Helper compilation and noncapture self-tests precede this
qualification. Original child failure remains failure; capture, loss, clock
coverage or cleanup failure also prevents qualification. No request clock,
threshold, argument, workload, affinity or timer policy changes. The ordinary
child scheduling contract below supersedes inherited priority for the fixed
diagnostic child only.
The helper and exact workload executable are retained with diagnostics so their
wrapper hashes can be independently recomputed. Native DWORD failure statuses
remain raw in receipts; only returned process status uses the signed equivalent.
All five normal standalone guards reject any nonzero status, including a negative
native crash. Helper tests are a distinct mandatory suite, independently checked
from the existing physical-control/evidence tooling suites. The seven Windows
tooling suite counts are 12, 21, 4, 1, 267, 32 and 3 (**340 total**); the separate
scheduler helper suite contains **40**, for a combined inventory of **380**.
Qualified execution `ed09e7126` ran 40 helper plus 338 other checks (378);
the two new offline archive-reader tests belong to analyzer B and do not
retroactively change A's inventory or claim B's hosted/native qualification.
Original `be949192c` ran 36 helper and 338 other Python checks (374 total).
Its PR Full/mixed RPC p95 and official first pooled Event/action failures
remain failed. Two pooled Full case tests bind its exact flags/profile and
failure prefix; two hosted preparation tests reject other contexts and verify
exact task scope, disabled state when present, actual absence, held-process
identity and fail-closed errors. Original `0dd022697` fails lookup before build/tests because the image
has no matching registered task; no Windows inventory is claimed for that run.
Original `09aa075ad` ran 34 helper and 338 other Python checks (372 total)
before its second direct pooled command failed recovery peer 0's RPC p95.
The two new helper cases bind the pooled command and reject substitutions;
they add no workload invocation or latency allowance.
Original `d66444f3` ran 33 helper and 338 other Python checks (371 total) before
its separate aggregate RPC timing failure. The added helper test covers the
fourth command's exact fixed-case mapping and substitution denials.
The historical `c33d573d5` hosted jobs ran 20 helper tests plus 295 tooling tests,
or 315 total. The local full tooling matrix's 315 tests are a different scope
and do not replace either hosted suite. New source requires its own original
hosted results; the updated inventory is not a claim that those jobs passed.

Aggregate request identities and original steady-clock endpoints join bounded
CPU/sleep observations and buffered peer-operation spans to bracketed QPC
anchors. Retained maxima or a long low-CPU span alone do not establish the cause
of a failed RPC. Scheduler attribution requires overlapping request-linked
evidence; neither waiting nor ready-thread time is subtracted from latency.
Earlier failed runs remain failed, including those without this evidence.

The bounded Full diagnostic preserves original nonzero child exits and stops
the gate, even when an ordered failed-case evidence prefix is retained.
The always-run artifact upload preserves Full and both aggregate raw sets plus
the fixed helper and workload binaries; a command-three failure may leave the
later aggregate set absent because that workload was never started.

Full offline coverage requires the bounded actual scheduler CSV, the original
ordered eight case anchor pairs for a successful child, and complete bounded
RPC/Event and action chronologies joined to original endpoint/resource records.
A failed child can retain only a complete ordered case prefix. Phase maxima are
sampled evidence rather than complete operation coverage. Pending endpoints use
the existing post-END-anchor observation; action native thread identity and raw
CPU brackets are not measured per action. Retention does not imply native PASS,
causal attribution, a timing discount, or a change to any performance gate.
The final post-anchor observation is bounded by the measured child-exit QPC.
MSVC's steady nanoseconds use `floor(QPC × 10^9 / frequency)`; the validator
checks that relation against both measured case-anchor brackets. The difference
of two floors is at most the ceiling of their difference, so the conservative
upper observation bound uses the earliest END QPC and an exact integer ceiling.
This is clock-conversion uncertainty, not added latency slack. It asserts no
unmeasured hosted compiler-source pin or arbitrary global clock offset.

The manual `scheduler_trace` input enables one bounded Windows scheduler
diagnostic around the existing FULL `--reliable-workload` invocation. This
distinct diagnostic replaces
that invocation once before the independent CTest gates, preserves its exit
status, and skips its later duplicate. This ordering retains causal evidence
even if an independent native gate would otherwise stop the workflow first;
it does not retry a failure or omit a required test from a successful job.
It is not a physical-package dispatch. Its own kernel session, original ETL,
workload output and diagnostic metadata are retained by the always-run artifact
upload. Missing or lost scheduler evidence cannot establish a causal diagnosis,
and a later green diagnostic never changes an earlier failed result.

The separate `scheduler_pair_diagnostic` dispatch is **diagnostic only**. It
runs the fixed FULL workload and ACK statistics-boundary case once each, with
separate owned sessions and outputs. A failed first workload does not suppress
the second observation when cleanup is verified; unverified session/process
cleanup prevents the second launch. Both original child exits are retained.
This mode skips qualification CTest, standalone workload qualification,
packaging and Linux execution. Its Windows job name explicitly identifies it
as diagnostic rather than qualification, and the final acceptance verifier
rejects that job as a replacement for required CI even if it succeeds.
Ordinary push/PR execution still runs the complete native matrix and all five
unchanged logical standalone commands, with the fixed aggregate trace described
above. The two manual diagnostic selections are mutually
exclusive. No diagnostic result supplies a new physical authorization gate.

ACK statistics diagnostics are fixture-only and opt-in. Bracketed QPC/steady
observations bind F1's steady-clock microsecond timestamps; reconstruction must
include their one-microsecond representation uncertainty. The supported GNS
service-thread initialization callback records bounded thread identities for
ETL lifecycle correlation. GNS's `GetLocalTimestamp` clock has its own mutable
offset and may clamp a long gap, so a beginning/end offset cannot map all native
events to QPC. Bracketed native snapshots establish observations at those points
only. Neither clock instrumentation nor thread observation changes service
timestamps, deadlines, thread priority or transport scheduling.

The existing GNS fairness target also compiles Windows/Linux wait bodies
extracted at configure time from the actual applied pinned socketthread source.
Bounded OS/clock stubs cover absolute-deadline/clamp preservation, NULL-timer
short spin, manual/no-grant/Never bypasses, already-due work, event wake and
long timer/error fallback. The sender computes remaining time and its absolute
target from the same clock sample; Windows requires a timer handle only for
the long timer branch, not the existing at-most-1,000-us spin. These are source
regressions, not a claim that a historical uninstrumented CI plateau has been
attributed. Native compatibility and the correction's own original CI remain
required; a passing diagnostic cannot replace a failed qualification job.

The diagnostic uses a named system logger with a fresh GUID, 8 MiB of configured
buffers and a 512 MiB sequential ETL limit. Decoded CSV is capped at 512 MiB;
each child output stream is capped at 32 MiB. A Job Object owns the suspended
child before it is resumed. Capture-start failure launches no child. The child
has a 600-second deadline; decoding has a separate 180-second limit, with a
960-second outer controller watchdog and identity-matched cleanup fallback.
Only the owned session handle, or its verified GUID/name/path on recovery, may
be stopped. Original child exit is retained before decoding. Loss, unsupported
scheduler layouts, truncation or missing clock coverage fail the diagnostic;
they never become evidence of an application or scheduler cause. Self-tests
exercise decoding, bounds, clock coverage and exit precedence without starting
a kernel session or native workload.

Decoded rows remain streamed under the unchanged 512 MiB CSV byte cap and
180-second decode deadline. Each emitted row has positive byte length, so that
byte cap also bounds the row counter far below its 64-bit range. There is no
independent five-million-row cutoff: it prematurely truncated a retained Full
ETL whose capture brackets all cases while its decoded CSV was only 309,432,716
bytes. That original
development diagnostic remains failed; loss-free ETL and successful native
execution do not retroactively qualify its incomplete original CSV.

The helper's separate `--decode <existing-etl> <new-output-directory> <main-tid>`
mode reads a bounded regular ETL on a local fixed drive and invokes the same
decoder without starting a session, child, service or workload. Input ancestry
and the existing output parent must be free of reparse points; the output is
exclusive and outside the retained input directory. It writes a new CSV and
`DECODE_ONLY` receipt with actual statuses, row/byte counts, header loss, QPC
coverage and duration. It never edits original metadata or declares original
CI/native qualification. A separately identified offline reconstruction must
join the preserved input hash and original child/clock evidence to that receipt;
no offline result substitutes for a new original hosted job.

### Ordinary scheduling for the fixed traced child (2026-10-04)

The fixed traced child is created suspended with explicit
`NORMAL_PRIORITY_CLASS`, then assigned to its existing owned Job Object.
Immediately before resume, `GetPriorityClass` must report Normal and
`GetThreadPriority` must report `THREAD_PRIORITY_NORMAL`. Query failure or a
different policy prevents resume, preserves status 125 and reaps the owned
process tree. Requested flags, controller class, actual child class and main
relative priority, query status and pre-resume verification are retained in the
early child result, final metadata and wrapper. The current-source offline
validator requires this measured policy. This includes the fixed first
`PooledFull` command after original `be949192c` package dispatch failed mixed
Event/action latency before any trace. That original API priority and precise
poll wait cause remain **NOT MEASURED**. It also includes the fixed second
`PooledAggregate32Structural` command after original `09aa075ad` push timing
failed before any scheduler wrapper was reached. Its original child's API
priority and precise scheduling/wait cause are **NOT MEASURED**. The raw timing
failure remains failed; this extension closes the missing observation and
ordinary-launch precondition, without claiming that priority caused it.
It also includes the fixed fourth
`Aggregate32` command after original `d66444f3` push timing failed without a
failing-window trace. Its previous direct child's actual priority is
**NOT MEASURED**; the extension does not establish priority as the old cause.
The controller's own scheduling,
production transport, F1, latency clocks, workload,
affinity and timer policy remain unchanged. The separate hosted telemetry
preparation below defines the sole task-state change, confined to its
disposable GitHub VM.

The [Windows process creation contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
allows an unspecified child class to inherit an Idle or Below Normal parent.
[Scheduling priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)
maps ordinary relative priority to base 6 under Below Normal and base 8 under
Normal. The original `3e030710` PR Full trace records base-6 PowerShell/helper/
workload threads competing with base-8/9 Compatibility Appraiser threads.
The original API priority-class queries were **NOT MEASURED**; those base
priorities alone are not reclassified as API observations. Its action failure
remains failed. The new explicit ordinary class removes this launcher's ability
to inherit a lower class; it does not promise a future hosted latency PASS.

The separate `--priority-self-test` launches only bounded, exit-only copies of
the helper. Three owned parent processes, created at Normal, Below Normal and
Idle, each compare an unspecified child against an explicit Normal child.
Actual class/relative-priority and ownership/reap results are printed for all
nine process launches. Only test-owned creation flags select those classes;
the controller's class is preserved. Nested parents have a 25-second bound,
each probe has a 5-second bound, cleanup is bounded and logs are capped at
8 KiB. No session, transport or workload starts. Existing `--self-test` retains
its no-child guarantee. Positive API controls and wrong-class/query/missing-
evidence denials supplement unchanged loss, clock, cleanup and original-exit
tests. Historical original validators and receipts remain immutable; this
current-source requirement does not retroactively qualify old jobs.

### Hosted scheduled-telemetry preparation (2026-10-06)

The subsequent CI-only candidate `9ecc96619` push Windows job `112265212600`
records Appraiser process count zero immediately before CTest, but fails test
80 `gargantuan_gns_funded_ack` in the 393,652-byte, 16,667-us-poll second grant.
The payload/ACK/retirement and chronology checks pass before the completed
grant's independent F1 failure flag rejects it. Its original failure remains
failed. The old failure path throws before printing the completed grant's
activation, first-send, completion, deficit and segment ledger; its exact
finite/running violation and blocked/ready cause are NOT MEASURED.

Failure-only ACK-cycle output now retains that original completed snapshot and
its existing bounded segment records before the unchanged rejection. It does
not resample, move the activation clock, extend a deadline or print on the
running service path. Cold supplied-record checks exercise snapshot identity,
stale-token visibility and the existing 512-entry bound before any connection.
The separate `scheduler_ack_cycle_diagnostic` dispatch route traces only the
exact `--ack-cycle-funded 1348` command once through the existing owned Job,
ETW, ordinary Normal child and bounded stream/cleanup path. It is mutually
exclusive with the existing FULL and pair diagnostics. The normal complete
CTest and five standalone workload commands remain unchanged.

This opt-in route reuses the existing GNS service-thread initialization callback
and QPC/native snapshot brackets, with distinct ACK-cycle labels. Native timer
state is sampled only after a rejected grant or the selected pair's retirement;
the original completed first-send ledger stays authoritative. Loss, lifecycle,
clock, source hashes and native exit remain explicit. Diagnostic output cannot
qualify a failed original job or physical provider, and no automatic diagnostic
retry, package, Linux qualification or F1 waiver is introduced.
The private borrowed timing sink records timer creation/fallback, actual wait,
global-lock reacquisition, receive-drain count/bytes and thinker-pass
count/collisions. With no sink, these hooks read no clocks, count nothing and
allocate or print nothing. Opt-in fixture storage is allocated before GNS
initialization, capped at 65,536 records by the existing 32-MiB stream limit,
and rendered only after pair destruction and sink removal. Overflow, invalid
clock or missing phase/lifecycle evidence leaves diagnostic coverage incomplete.
The original wait APIs, parameters, thinker cutoff, retry policy and F1 bounds
are unchanged; neither observation overhead nor externally scheduled time is
subtracted from a failed grant.

Diagnostic source `bc56bbc2491aa5377d80a4d93b80ca7f0fd0306b` passes all six
original attempt-one jobs: Windows 96 native +382 Python each, Linux 53 each,
and GNS 11 each, zero failures/errors/skips; original source and merge trees
agree. Worker and hosted fixed ACK-cycle probes do not reproduce the failure.
Subsequent pooled-recovery observations reproduce four-grant contention and
test a scoped ordinary-DATA correction. Native development v8 and its healthy
control v9 pass their grant certificates, but the correction still requires
its own original hosted jobs and exact-source package before fresh physical
acceptance. The active-eight control is refused by the old active-32 coverage
parser; it is not a coverage PASS. No latency subtraction, failing-result
substitution, F1 waiver or automatic retry is allowed. The current correction
changes ASAP normalization and ordinary DATA scheduling; the unchanged-wake
statement above describes the earlier diagnostic-only revision.

`da16d5aba` original push Windows job `112458311111`, run `37518739890`,
fails ACK statistics CTest 83 (95/96 native pass). Control token 24's 347
segments exactly reproduce 18,605,932,544 >18,025,216,000 byte-us from a
1,109-us zero-service gap. Finite/ACK/retirement pass; actual wake/scheduler
cause is NOT MEASURED. Both GNS sanitizer jobs pass 11 but cannot replace the
failed Windows original. The exact statistics diagnostic reuses the bounded
ACK-cycle phase buffer under its existing opt-in environment; no normal
runtime clock/allocation, timer policy or F1 acceptance changes. Its parser
retains per-arm/thread scope and all existing bounds. A later passing probe
does not retroactively explain or qualify this original failure.

Analyzer B `8c5a22327` PR Windows job `112239065328`, run `37454619898`,
fails CTest 83 `gargantuan_gns_funded_ack_stats`: the prompt arm's token 36
conserves and retires all 393,652 bytes, but its maximum running deficit is
45,011,623,360 byte-us against the unchanged 18,025,216,000 bound. The finite
shortfall is zero; that does not waive the independent running gate. The
347 recorded first-send segments reproduce the failure exactly. The largest
gaps are 2,319 and 1,366 us. No failing-window CPU or scheduler trace was
retained, so blocking, descheduling and Appraiser presence are NOT MEASURED.
Native, transport, fixture, workflow and CMake blobs are unchanged from
qualified execution A `ed09e7126`; the offline parsing change is not a
production cause. The original B failure remains failed.

Its early Appraiser-zero observation is at 11:12:52.820881 UTC; CTest 83 starts
at 11:55:03, over 42 minutes later. The prior late guard follows CTest and was
skipped after this failure. Reusing that exact guarded, held-object, 15-second
cleanup immediately before CTest closes this host-evidence gap without a new
helper, native change, deadline, F1 allowance, priority or timer policy. It
neither proves the old cause nor guarantees a later PASS. New original CI
remains required for this workflow change; no failed job is rerun or relabeled.

Original `be949192c` PR Full/mixed p95 failure retains loss-free ETL proving
external `CompatTelRunner.exe` CPU competition during workload ready waits.
This is a measured environment defect; it does not establish the exclusive
cause or blocking API of the separate 119-ms wait. The original failed result
remains failed, and no scheduler duration is deducted from any latency sample.

Before build and tests, the Windows job disables and stops only the exact
registered `\Microsoft\Windows\Application Experience\Microsoft Compatibility Appraiser`
task. It requires `GITHUB_ACTIONS=true`, `RUNNER_ENVIRONMENT=github-hosted`
and `RUNNER_OS=Windows` before any task query or mutation. Successful task
enumeration is filtered to that exact identity. A present target requires
disabled readback and actual `CompatTelRunner.exe` process absence. Zero
matches causes no task mutation and retains truthful `NOT_REGISTERED` output.
Original `92e3c9b9` demonstrates that Appraiser can nevertheless be active.
Only the exact normalized Windows System32 `CompatTelRunner.exe` image may
then be stopped: validate name/PID/native birth/path, open a Process object's
handle, requery the same CIM identity, then terminate and wait using that held
object. Wrong or changed identity and API/handle errors refuse termination.
The object is disposed on every path. Successful disappearance skips the
kill but still requires the final fresh census. No tree termination, arbitrary
image or bare PID-kill fallback is allowed. Duplicate target, enabled task,
persistent process or reappearance fails setup.
The monotonic quiescence observation is bounded to 15 seconds;
the entire preparation step is separately bounded to one minute, including
synchronous APIs, and each exit wait consumes only the remaining budget.
Actual PowerShell/framework versions are logged. The associated-process
[.NET implementation](https://github.com/dotnet/runtime/blob/release/10.0/src/libraries/System.Diagnostics.Process/src/System/Diagnostics/Process.Windows.cs)
reuses an opened handle for termination and wait; it is retained until finally
disposal rather than reopened by PID. This establishes only the fixed image's
absence, not descendant cleanup or absence of every background CPU consumer.
Immediately before complete Windows CTest and again before the five standalone
workloads, separately bounded copies of the exact same closed process cleanup
require actual Appraiser absence. The pre-CTest copy is skipped only when the
existing diagnostic-only mode skips CTest; the later guard remains unchanged. Query failure
cannot become an empty process list. These facts replace `0dd022697`'s unnecessary assumption that
the registered task must exist; its original setup failures remain failed.

This preparation changes only the disposable hosted VM. User endpoints,
installed capture tools, security services, thread priority, affinity, timer
policy, workload and acceptance clocks are untouched. GitHub documents the
[runner environment variables](https://docs.github.com/en/actions/reference/workflows-and-actions/variables)
and [fresh hosted VM scope](https://docs.github.com/en/actions/reference/runners/github-hosted-runners);
Microsoft documents the fixed
[disable](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/disable-scheduledtask)
and [stop](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/stop-scheduledtask)
operations. All five workload invocations, unchanged latency gates and all
six original hosted qualification results remain mandatory for the new source.

The decoder recognizes the originally supported CSwitch version 2/24-byte
layout and the exact observed version 5/28-byte, 64-bit-header layout. The
version 5 branch retains raw `Version=5` and reads only NewThreadId,
OldThreadId, OldThreadWaitReason and OldThreadState from byte offsets
0/4/12/14. These common fields follow the [Microsoft PerfView parser at
`33a2fb6c`](https://github.com/microsoft/perfview/blob/33a2fb6cdb3bb920ae0210b28d39ce66578ad0cf/src/TraceEvent/Parsers/KernelTraceEventParser.cs#L4306-L4356),
and 16 matching raw-QPC samples were independently decoded by Windows
Performance Toolkit `xperf` 10.0.26100.7705 (binary SHA-256
`6e0c15d303e9369a8756b7f9b63978220ba3c8f9b9471a00b0e63958cc095561`).
Version 5 byte 13 is not treated
as the version 2 OldWaitMode; the CSV field is blank, and the unneeded trailing
bytes are not interpreted. Other versions, lengths or header flags remain
unsupported. The native non-capture self-test exercises both valid layouts and
rejects unknown version 5 lengths, versions and flags.

The manual hosted diagnostic run `37115431988` predates this decoder change.
Its original `DiagnosticComplete=false`, 299,065 unsupported CSwitch events,
coverage `INCOMPLETE`, and failed hosted job remain historical facts. An
independent offline replay of its retained ETL established the exact version 5
layout. The retained ETL SHA-256 is
`b6680a4472090830746662c2b24012d2646a1f5970a6d3fa07bf73885b4c5ac3`;
the replay reproduced all 160,537 originally accepted CSV rows byte-for-byte
at SHA-256 `d9f0544393e415b32f687c07eb44177d3a6159566b81c92b49f8cbb4c8838fd9`;
that offline result does not relabel the original diagnostic or CI gate.

The controller uses the Windows SDK system-logger API rather than changing
thread priority, affinity, timer resolution or service policy. See Microsoft's
[system tracing contract](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session)
and [logging mode bounds](https://learn.microsoft.com/en-us/windows/win32/etw/logging-mode-constants).
Ready-to-running delay and time waiting are distinct evidence. A gap in guest
events alone does not identify a hypervisor cause, and neither time category
is subtracted from the measured RPC/Event/action latency.

CTest prints failing output and writes JUnit results. On any failed job, the
workflow uploads the configure log, CTest log, and JUnit file when available.
Windows CTest supplies a fixed workspace artifact root for the socket-free
resource-sampler fixture. Each invocation creates its own fresh GUID directory;
failed raw counter preparation, generated source, phase records and bounded
child logs are retained there and included by the always-run diagnostics
upload. The fixture's original startup/query/stop deadlines remain unchanged.
Retaining a failed fixture never changes its test or job verdict.

Each bounded RPC/Event record also reports `RemoteResourceSpan` observations
for that exact latency interval. CPU reads bracket each steady-clock endpoint;
monotonic endpoint samples yield lower/upper differences in reported thread and
process CPU counters (100 ns units). These are bounds on sampled accounting,
not physical timing precision or proof of an infrastructure pause. Process CPU
includes every process thread and can exceed elapsed wall time. Missing reads,
counter rollback, mismatched threads, or invalid boundaries remain unmeasured.

The fixture's same-thread cumulative sleep ledger reports only whole completed
measured sleep windows between those endpoints, with requested and actual
nanoseconds. Each window includes the existing measurement envelope around
`sleep_until`; it is not a kernel wait-state measurement. Earlier/later sleeps
are excluded, and overlapping, invalid, or overflowing observations are not
attributed. Observation overhead remains in the original latency measurement;
no thresholds, waits, scheduling policy, or assertion results are adjusted.
Earlier failed runs without these endpoint samples remain unattributed.

The native ACK statistics-boundary fixture retains each failed conservation,
retirement, pending, unacked and zero-retransmission predicate separately, plus
bounded sender/receiver ACK event records. Its combined assertion is not proof
that conservation specifically failed. The four-grant fixture snapshots all
four peers before validating or printing, so one failure cannot suppress the
other simultaneous native timelines. Failure output is bounded by the existing
128 ACK events per endpoint and 512 first-send segments per peer. These
diagnostics do not extend a wait, reset evidence or change any gate.

No dependency or compiler cache is required for correctness; only the
repository-tool download cache is currently enabled. Protobuf and the pinned GNS
source must therefore restore successfully on a cold runner.

ASan/UBSan are deliberately a dependent second gate rather than part of the
Windows production configuration. Ubuntu 24.04 uses Clang 19 plus the GCC 14 C++
runtime, `-fsanitize=address,undefined`, leak detection, immediate failure, and
symbolized stacks. It runs registered tests serially with the SDL dummy video
driver. The sanitizer job uploads the same configure, CTest, and JUnit evidence
as the Windows gate on failure. UBSan is not described as an MSVC feature, and
the workflow does not weaken the normal Windows ABI/toolchain contract to obtain
sanitizer coverage.

## GNS sanitizer safety versus F1 capacity

The dedicated `.github/workflows/gns-sanitizers.yml` workflow exercises pinned
GNS and Gargantuan's adapter under ASan/UBSan/LSan. Its four-grant case uses
`gargantuan_f1_four_grant_sanitizer_safety`: four real 512 KiB reliable
structural grants must first-send exactly once, preserve event chronology,
packet bounds and the canonical finite-envelope packet-overhead budget, then
ACK and retire exactly. A bounded wait accommodates sanitizer instrumentation
before evidence is read. The test reports `f1-not-qualified` and does **not**
use instrumented wall-clock intervals to assert 16 MiB/s per peer or 64 MiB/s
for the pool; it still reports the observed F1 failure count and deficits for
diagnosis. A green sanitizer job is therefore a memory/undefined-behavior
and transport-accounting result, not F1 throughput qualification.

The separate `gargantuan_f1_four_grant` test retains the strict native
first-send curves in the uninstrumented Windows Release CTest matrix.
Dedicated uninstrumented repeatability and fresh physical qualification are
also required before claiming F1 capacity. A failure in either strict setting
remains a qualification blocker even when sanitizer safety passes.

The mixed-traffic fixture separately pumps application receive events after
native ACK/retirement convergence. The funded ACK can complete on the native
service thread before the fixture's ACK predicate is first evaluated; that
predicate does not prove that the application has polled its ordered messages.
The separate bounded receive wait retains exact control/structural/gameplay
FIFO and payload checks. It does not extend or resample the preceding native
first-send service-curve gate. Hosted GNS sanitizer run `37080031865` exposed
the old fixture's FIFO failure at `140a2539f`; its exact collected payload count
was not recorded and that failed run remains failed evidence.

The deliberate receive-loss regression also exercises repeated fragmented ACK
serialization. Native suite `1722a9ebb` passed 13 of 14 selected tests, but that
case filled the bounded opt-in event log with the same packet identity and lost
its later message-ACK event. Observation now retains the first fragmented-ACK
event per changed identity plus exact count and last timestamps for repetitions,
matching the existing ordinary-ACK strategy. Distinct-event and counter overflow
remain failures. This observer change does not alter transport scheduling,
packet accounting, injected loss, or the failed run's qualification result.
