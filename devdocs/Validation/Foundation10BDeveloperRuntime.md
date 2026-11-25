# Foundation 10B developer runtime qualification

Development started at Foundation 9 main `bf50b0987fde410995d6d3169c2fde68a2464a74`.
The runtime and fixtures under qualification are `0ea0b4a047c6fe1c6e78008604d82dd1de8a6a65`,
tree `9994dbc423ba2f184c56644adf182341d12e8663`. The final change from
`587601c33` affects only the GUI test's preallocated Luau table.

## Review and ownership

The strategic reconciliation at `0195d658d` identified Instance keys,
Player addressing, practical payloads, runtime budgets and persistence.
The Foundations 5–8 assurance register at Engine `7c0aca159` contains 32
findings, including 14 High. The verified register SHA-256 is
`7b53c9885381a2696b4c3784e7e58cf29c4661d6870baafd16ab0c7c9f735c86`.
The separate 10A package branch was at `28a3c254b`. Its acceptance gates
remain with the package owner. Runtime continuation retirement and queue
ownership retain their existing remediation assignments.
The existing hosting branches supplied no code to 10B.

## Runtime decisions

| Area | Developer contract and scope |
| --- | --- |
| Instance keys | A VM's weak-value cache returns the same retained userdata through signals, property access, remote references and hierarchy lookup. The wrapper owns the native allocation until collection. Lookup uses its address privately without publishing an ObjectId. Native destruction, generation reuse and stale access keep their existing rules. |
| Players and Remotes | Server-owned ready sessions resolve Player, connection generation and epoch in both directions. Gameplay callbacks can use the Player as a table key. Advanced callbacks retain the peer context. Malformed or unauthorized addressing, departed Players and unpublished Remotes have distinct handling. |
| Structured arguments | GRMT v3 and GSES v2 form a required peer capability. Remote arrays, dictionaries and copied buffers have a separate representation from schema properties, GAS and replication. Frame bytes, decoded storage, references, table scans, fanout and retained queues each have explicit limits. See [Luau remotes](../CurrentArchitecture/LuauRemotes.md). |
| Property animation | One scheduler per VM samples reflected goals through normal authority and property mutation. Playback and completion admission share a 1,024-slot limit. Creating a tween requires a live target in the same DataModel. See [tween runtime](../CurrentArchitecture/TweenRuntime.md). |
| Everyday APIs | Strict ancestry predicates, nearest-parent-first ancestor snapshots and the corrected ClearAllChildren declaration complement existing discovery. FindFirstChild plus lifecycle signals cover late publication. A yielding WaitForChild needs bounded timeout and continuation ownership on destruction. Task cancellation already retires scheduler-owned work. |
| Characters, controls and GUI | Relay Rescue keeps gameplay state on the server, presents collection feedback through PlayerGui and leaves movement and camera to the current possession. Its default and optional Survey Suit use the same application scripts. Existing spatial queries, semantic input actions, StarterGui cloning, ViewportFrame and GAS remain the supporting APIs. |
| Script execution | A configurable 250 ms outer-entry deadline covers VM safepoints and nested calls. Native calls return to the VM before the next check. Hostile-code isolation, total frame budgets and memory quotas remain separate. Task errors flow to the bounded runtime diagnostic sink. Studio consumes the existing Play diagnostic queue. |
| Backend | [Provider-neutral persistence handoff](../FutureArchitecture/GameplayPersistenceHandoff.md) defines revisioned writes, provider identity, cancellation, ambiguous outcomes and conformance tests against the pinned Node contract. |

## Evidence and intermediate failures

Windows Release Foundation tests passed on identity commit `f76a862c9`.
The detached wrapper retained its object until collection, two VMs owned
distinct wrappers, and a destroyed slot reused with a new generation could
not retrieve the previous table entry. Integrated coverage also exercises
the same Player across RPC, events and replica lookups. It checks first
exposure in a domain lacking publication authority and inspects the journal.

The first integrated Windows cut at `a56aa6129` passed the native codec,
GUI and both Relay transports. Its Luau fixture used an unregistered global,
its RPC callback attempted a reflected Player property read in a network-only
context, and its hierarchy fixture attempted a method on a destroyed receiver.
Corrected fixtures at `bad92ec8b` passed those three suites. The expanded
queue test then showed that a fixed item count exceeded its first-message
quota on MSVC. Its replacement sizes the same admission case from the native
value representation. A second test now checks preallocated tween goal
storage before a Luau hash insertion can shrink the table.

Clang 19 with libstdc++ 14 rejected an inline constructor over an incomplete
recursive dictionary entry. Defining it after the entry preserved the wire
format and passed the native syntax probe. At `8159d8c2f` the GRMT codec
passed 50,000 libFuzzer executions with seed 112410 under ASan, UBSan and
leak detection, followed by 125 replay executions. The run reported 1,193
coverage points and peak RSS of 242 MiB. The codec and fuzzer inputs are
identical at the qualification source.

The repo-native Python checks at `413fb27cc` discovered 446 tests. Of those,
438 passed and eight scheduler tests skipped because their native helper was
unavailable in that Python lane. The formatter retry, pinned coordinator
bootstrap, two recovery checks and missing-client-tick negative control passed.
Their source and recipes match the final cut. The controlled worker verified
9,522 pinned vendor inputs. Both platform checkouts matched all 1,688 tracked
files in the qualification archive, SHA-256
`37567f6a6d33739048f50556a487f9cc2a403f439f45d8621ca3788a5b7e5fdc`.

## Qualification environment

The controlled Windows build used MSVC 14.51, Ninja, Release, GNS and the
repository's benchmark targets. Shader tools and vcpkg dependencies came
from the pinned worker caches. PCH was disabled in this controlled build.
The Linux container used Clang 19, libstdc++ 14, RelWithDebInfo, ASan, UBSan
and leak detection. Both builds used GNS `2cb93a06350bb065db53abdb0d87cf297e0bfd34`
with the repository's configured service fairness, feedback and sender wake
changes. The documented upstream GNS exceptions applied only to
that library. Engine-owned transport, feedback and pacing retained their
sanitizer checks. The GNS and non-GNS Linux builds used separate directories.

All 13 Linux real-GNS sanitizer CTests passed. The ordinary and pooled
profile runs and production-admission benchmark also exited successfully.
All 130 separately configured headless sanitizer CTests passed in 422.21
seconds, with zero skips. This includes the custom Character mission,
packaging, remote values, gameplay deadlines, wrapper collection and tweens.
Windows and Linux source manifests, build logs, JUnit XML and standalone
workload receipts live in the controlled worker's
`C:\Sandbox\Codex\Artifacts\Engine10B` directory. The development
controller retains copies under its `foundation-10b-runtime` artifact
directory. These are local qualification runs.

## Windows Release measurements

The permanent work capture was enabled within the relevant benchmark phases.
Numbers below are observations at the pinned source on Dockerbox. Allocation
counts cover calling-thread C++ allocation requests seen by the fixture.

| Work | Result |
| --- | --- |
| 4,096 repeated Instance pushes | Zero wrapper misses. 77.5 µs total with the capture disabled and 243.3 µs with it enabled. |
| Wrapper churn with GC suspended | 1,024 new native objects wrapped twice produced 1,024 misses. The Luau heap rose from 68,967 to 167,239 bytes and returned to 68,967 after full collection. All 1,024 weak native references expired. |
| Identical 100,000-iteration finite Luau loop | 341.3 µs with a cooperative host and 669.5 µs with the deadline enabled. The deadline incurred measurable overhead on this loop. |
| Property tween step | Ten steps across 1,024 active one-property tweens sampled 10,240 playbacks in 13.13 ms total. Captured updates, including reflected writes, requested 21,504 C++ allocations totaling 2,146,304 bytes. |
| Structured inventory | 24 item dictionaries and a 2,048-byte buffer encoded to 3,850 bytes. Across 10,000 round trips the mean was 15.68 µs without capture and 15.61 µs with it. The 0.07 µs difference in this ordered pair does not establish a capture speedup. |
| Structured decode | Captured encode and decode averaged 6.34 and 8.53 µs respectively. Decode charged 12,363 logical bytes and requested 12,312 C++ bytes in 52 allocations per message. Logical admission charges and allocator requests measure different quantities. |
| Scalar remote fixture | Encode 0.831 µs and decode 0.276 µs per message. Reliable, unordered and sequenced admission averaged 0.991, 0.970 and 0.985 µs. A request/start/response/dispatch cycle averaged 2.08 µs. The scheduler received 160,000 submissions totaling 10,920,000 bytes. |

The build kept Tracy and optional span export disabled, as in the normal
Windows Release gates. Benchmarks exercised the per-phase permanent capture
with explicit readers where required. Tween timing includes native property
mutation. The scalar admission fixture uses a recording scheduler. Neither
fixture yields a per-client frame time.

## Windows workload gates

The full Release run passed 183 of 184 CTests in 295.18 seconds. The failing
pipe-drain test's child `cmd.exe` could not locate the installed `ping.exe`
with the worker's 8,195-character inherited PATH. A process-local PATH with
Windows System32 reproduced the expected command exit, and the unchanged
CTest passed in isolation in 10.04 seconds. The original XML and one-test
replay together cover all 184 registered cases. The Relay checks include
two clients over simulated and real GNS, the custom Character mission and
GAS save/load.

All five standalone commands exited zero on executable SHA-256
`6adddb6ec9698dfced00f80331efbacd543153719b9dbc08c81b2f7d2a019662`.
Each emitted its exact `POOLED_CORRECTNESS_V1` completion record.

| Existing workload | Wall time | Evidence |
| --- | ---: | --- |
| Pooled single-peer | 125.43 s | 8 timing rows |
| Pooled structural 32 | 40.92 s | 48 per-peer rows |
| Full-reservation single-peer | 125.36 s | 8 timing rows |
| Full-reservation ordinary 32 | 37.71 s | 48 per-peer rows |
| Full-reservation structural 32 | 41.23 s | 48 per-peer rows |

All 144 per-peer aggregate rows reported zero errors. The non-overload
rows ranged from 56.6 to 74.9 ms for peer RPC p95. Overload peer p95 ranged
from 219.1 to 271.0 ms across the three 32-connected fixtures. These are
RPC observations under the aggregate harness's scheduling and load. They
do not measure an individual client's render frame. The five runs took
place with the other campaign builds paused.

## Developer documentation

Documentation qualification checked 298 documents, 78 declarations and
20 compiled Luau examples. All nine Python checks and five repository-link
tests passed. Astro built 35 HTML pages and the site checker verified 2,300
links with zero errors. Studio's diagnostic handoff uses the existing
PlaySession protocol. This campaign changes the Engine side only.
