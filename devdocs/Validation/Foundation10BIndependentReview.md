# Foundation 10B independent promotion review

**Recommend promotion for private engineering.** The follow-up corrected the
inherited receive-ordering blocker and the related outgoing materialization
ordering gap. Focused Windows Release and Linux sanitizer suites, real
localhost GNS and the five existing standalone gates passed. Foundation 10A
remains the public distribution gate.

## Reviewed source

| Revision | Commit | Tree |
| --- | --- | --- |
| Foundation 9 baseline | `bf50b0987fde410995d6d3169c2fde68a2464a74` | `bde7e5194e2c1b08688ce431c1b6e24d523a3a69` |
| Qualified implementation | `0ea0b4a047c6fe1c6e78008604d82dd1de8a6a65` | `9994dbc423ba2f184c56644adf182341d12e8663` |
| Reported three-commit candidate | `b960c091ae2ad6385548d0cca4ce3b74599bf04b` | `784aa66f901efa12844ae465def321984294608d` |
| Original development branch, `foundation/engine-10b` | `b236bcf8dddee623b05ec53c52d7409657a56c89` | `784aa66f901efa12844ae465def321984294608d` |
| Runtime correction on the independent branch | `cded087d40ed6b4dc31e3231dd771beaf8dfc091` | `b8d0353a913ea11bd186e57ab1fca1a62b4b70fe` |
| Initial independent review hold | `c3f1a3fe64e23408c8ccfec35649bad321892d2b` | `9cd89d324da1af51ecd938c2faf94f999530761d` |

The candidate's second commit has the exact qualified implementation tree.
Its third commit changes six documentation and inventory paths. The original
development tip and reported candidate have identical complete trees. The
review uses the original developer-runtime assignment from chat
`01a210e6-e482-7390-a4fd-c25bf01189af`, the strategic reconciliation
`0195d658d`, and the Foundations 5–8 assurance register at `7c0aca159`.

## Findings and dispositions

| Finding | Classification and impact | Evidence, correction, and qualification |
| --- | --- | --- |
| Reliable callback order across bounded pumps | **Inherited promotion blocker, corrected.** A healthy connection could deliver reliable messages to native admission in order and still run gameplay callbacks out of order. The RPC monotonic-ID check could then reject the earlier valid request. | High confidence. The issue traces to `d52256135` and reproduced at `c3f1a3fe6`. Correction `39b747402` preserves the receive prefix and closes a related gap in outbound materialization deferral. The qualification below covers both changes. |
| Detached Instance equality publishes identity | **Required bounded correction, implemented.** Comparing different detached wrappers could allocate ObjectIds, or fail in a read-only domain. | High confidence. The legacy `Instance::LEqual` used `GetObjectId` on both operands after the cache made wrapping independent of publication. The regression fails on the reported implementation. The review compares the allocation owned by each wrapper. The Windows Release fixture checks read-only comparison, first-adoption journal publication with the original wrapper, separate VMs, destruction, generation reuse and GC. Authoritative identity and serialization still use ObjectIds. |
| Deadline expiry after a protected call | **Required bounded correction, implemented.** The original hook reset its sampling counter before reporting expiry, so a protected call could turn an over-budget entry into ordinary success. | High confidence. The finite protected-call fixture fails on the original runtime and passes on the review runtime. `BudgetScope::Interrupt` keeps expiry latched until the outer entry unwinds and checks every execution safepoint once expired. The fixture also checks one deadline diagnostic, hook restoration, and recovery in a fresh entry. The original continuation-remediation scope remains independent. |
| Structured decode work versus retention | **Nonblocking technical debt for private engineering.** Per-frame and retained-argument limits constrain memory. Frames rejected after decode consume transient allocation and CPU without consuming retained queue capacity. | High confidence in the accounting distinction. `HandleTransportEvent` validates wire size and queue headroom first, then parses and checks decoded retention. `GameSession::Poll` bounds one poll at 128 events. Trace rejected decode work separately and qualify worst-case allocation and mixed-peer service before making hostile-peer capacity claims. This belongs with networking input admission and overload work. |
| Tween ownership and update cost | **Nonblocking technical debt.** Callback-created tweens can outlive the Script that connected the callback. Full populations perform significant reflected-write work on Main. | High confidence. `TweenRuntime::Create` takes a weak owner only from `FindManagedScriptThread`. `Update` looks up/validates properties again and uses the ordinary setter path. The scheduler and current contract deliberately track the original managed thread and VM/target lifetime for other entry points. Bind broader ownership through the continuation correction if that campaign adopts such a contract. Any performance follow-up should separate interpolation, reflection, setter mutation and publication before changing authority checks. |
| Player RPC capability and failure ergonomics | **Accepted architectural tradeoff.** `SetPlayerHandler` can use Player-keyed closure state, but cannot read the player's reflected properties or mutate the world. | High confidence. The network-only RPC context in `RemoteFunction::BindRequestHandler` is explicit and preserves the existing handler boundary. `OnPlayerEvent` captures the subscribing context and supports normal authorized mutations. Preserve this split until there is a separate capability and continuation design. |
| Audience confidentiality and streamed possession | **Future campaign opportunities with existing release gates.** 10A distribution and possession-driven on-demand residency still need their owners' acceptance. | High confidence in scope. Exercise final package audience and late hydration with 10B's reference paths. For `ASSUR-RUN-04`, qualify streamed possession transitions through unloaded content using an authoritative active-focus contract. The Relay fixture proves game policy replaceability in the configured resident mission. |

## Architecture and developer surface

The central architecture holds. The Instance cache is per-VM, with weak values
and no cross-VM registry. Reachable userdata own the native allocation, so a
stale Lua key cannot become a key for a new object when an ObjectId slot is
reused. Existing wrap calls cover typed values, hierarchy and reflected
properties, signal payloads and remote references. First adoption stays on the
DataModel publication path. Destroy invalidates the native registry before
callbacks, and VM teardown releases the cache and the remaining userdata. A
strong cache leak and duplicated lifetime authority are unsupported concerns.

`ResolveRemotePlayer` and `ResolveRemotePeer` cross-check the exact connection
generation, replication epoch, Ready state, Player lifetime and parent, reverse
mapping, and session retirement. Explicit peer overloads stay available and
still require normal publication checks. The new Player signal rechecks
publication and association after the legacy callback and before each Player
subscriber. The two-client fixture covers identity and stale associations. A
claimed Player inside the payload is never used as the sender. Keep explicit
peer forms for advanced use. Routine targeted delivery and Player-keyed
inventory are usable through the public API alone.

`TryFire*` reports local admission, including reliable deferral. Broadcast
reports recipient counts and rejection codes, with zero recipients represented
by two zeros and an empty table. Local argument-shape errors still raise.
`scheduler_rejected` combines pressure and some lower-level failures, and a
post-admission transport failure can present as disconnect or timeout in an
invocation. `TryInvoke*` has an unambiguous Boolean first result, optional
structured handler error and finite timeout. Legacy invocation remains
ambiguous when success returns nil. Native cancellation controls pending work
and remote reply eligibility. Lua has no request handle for cancellation and a
timeout cannot roll back gameplay effects. These are documented limitations.

TweenService stores one scheduler per VM and invokes reflected setters on
Main. Target, capability, property validation and current GUI ownership are
checked. Numeric, vector, Color3, UDim, UDim2 and CFrame goals use finite typed
values. The sequence fence, strong in-flight handle, rechecks after each
synchronous setter and fixed completion queue cover cancellation, conflicting
goals, destroyed targets, reentrant replacement and shutdown. Prior accepted
property writes remain if a later setter or callback interrupts a multi-goal
sample. Two distinct reflected aliases can affect the same native state. The
limit of 1,024 active plus pending playbacks is shared by each VM. The recorded
one-property sample averages 1.28 µs, 2.1 C++ allocation requests and about
210 requested bytes per active playback step. Those costs include property
mutation and do not establish a per-frame budget for mixed gameplay.

Relay Rescue uses its actual public Player, structured-value, buffer, GUI and
Tween APIs. Its alternate character uses the same gameplay scripts, including
the period in which the Player possesses a drone without an active Character.
The persistence handoff matches the pinned Node revision and provider
boundary. It captures authenticated identity, compare-and-swap revisions,
ambiguous writes, separately owned bytes, cancellation, bounded completion and
conformance. Implementing the Engine adapter still depends on the qualified
continuation lifetime.

## Codec, lifecycle and release boundaries

The GRMT v3 decoder and Luau value snapshot share limits across the full
argument tree. The bounds include eight containers deep, 1,024 values, 128
Instance references, a 256 KiB frame and a 512 KiB logical decoded argument
ceiling. Nested nil, mixed and sparse tables, metatables, cycles, unsupported
types, invalid keys and non-finite numbers reject. Empty tables arrive empty,
and repeated table references become separate values. Numeric strings retain
legacy positional coercion and remain strings inside containers. Finite
integral Luau numbers in the signed 32-bit range encode as integers, including
conversion of signed zero to integer zero. Decoder reserve checks use
counts, remaining bytes and storage limits. Compact containers can amplify
their encoded size into native vector/dictionary
storage, bounded by those checks. Logical charges exclude allocator metadata
and Luau objects. The structured inventory measured 3,850 wire bytes, 12,363
logical decoded bytes and 12,312 C++ requested bytes in 52 allocations.

Submission owns a native buffer snapshot and the receiver creates separately
owned Luau buffers. Reliable deferral retains encoded bytes and native values
under explicit budgets. Fanout is preflighted against aggregate byte and
submission ceilings, then each peer still gets visibility and materialization
checks. Unreliable modes also obey their negotiated message and datagram
limits, which can be much smaller than the 64 KiB individual buffer ceiling.
Repeated serialization, payload copying and per-subscriber Luau
construction are bounded optimization candidates. Malformed or unresolved
messages fail before gameplay dispatch. A valid reference may retire during an
earlier callback and prevent delivery to a later subscriber. GSES v2 requires
the matching GRMT version on both endpoints, including scalar-only clients.
Delivery mode and channel checks, sequence rejection and request terminal
handling remain in the existing manager. The FIFO finding concerns gameplay
dispatch after healthy reliable transport admission.

Reference mapping uses the live source registry and peer publication on the
server, and the epoch-bound replica mapping on the client. A late reliable
reference waits for materialization under the normal limits. This preserves
the live replication audience boundary. Public multiplayer release requires
10A acceptance of package audience handling, shared module visibility and
late private code hydration.

Tween and Remote notifications can enter existing synchronous lifecycle
paths. `ASSUR-RUN-01` remains relevant to callbacks that reparent or destroy
objects. `ASSUR-RUN-05` through `07` remain relevant to delayed and resumed
work, including Signal and RPC continuations. Existing GUI, replica, audio and
animation signal paths inherit the new execution budget at the normal Luau
entry point and keep their existing owners.
Qualification of the independently assigned fixes should cover Tween
property/completion callbacks and retirement during Remote dispatch.

## Execution deadline performance

Luau enters the interrupt hook at interpreter safepoints, including loop back
edges and call/return paths. An empty hook already takes the interpreter's
protected callback path. The production hook examines thread-local scope and
the VM identity, chains an existing hook and decrements a counter. The
original implementation also called the out-of-line `lua_mainthread` while
walking the active VM scopes. A trial read the equivalent field directly from
the pinned Luau VM layout. The review keeps the public API call, caching its
result within each callback. Clock samples remain at the first execution
safepoint and at most 512 safepoints apart. The countdown continues through
nested execution of that VM.
GC callbacks chain the prior hook without taking a clock sample.

The reported 341.3 µs and 669.5 µs came from one pair of runs. This review
compared cooperative execution, an empty hook and the real deadline hook in
15 rotated repeats per mode and seven alternating processes per build.
Median process measurements for the original runtime were:

| Workload | Cooperative | Empty hook | Deadline enabled |
| --- | ---: | ---: | ---: |
| 100,000-iteration arithmetic loop | 295.7 µs | 357.5 µs | 535.3 µs |
| 10,000 reflected Instance reads | 1,213.6 µs | 1,216.8 µs | 1,232.1 µs |
| 10,000 native Instance method calls | 1,357.4 µs | 1,333.7 µs | 1,373.0 µs |
| Short callback with reflected reads | 0.7 µs | 0.7 µs | 0.7 µs |

The arithmetic loop pays for every visit, with about 196 clock samples
for 100,000 loop visits. A separate MSVC Release probe on the same worker
measured 18.9 ns per steady-clock read, or roughly 3.7 µs for those
samples. Clock sampling explains little of the approximately 238 µs
paired overhead. The empty hook shows a real interpreter dispatch cost.
VM identity lookup and deadline bookkeeping account for much of the
additional hook path. The direct field trial reduced the measured gap
between the deadline and empty-hook modes, but the empty-hook and cooperative
times also moved across link layouts. Paired total overhead was 238.0 µs
before and 234.7 µs in the trial. The trial offered too little end-to-end
benefit to justify private layout coupling. The largest measured cost was
specific to tight interpreter loops.
The normal tests cover remote handlers, modules and yielding or interleaved
tasks functionally. An independent deadline cost for those paths and a
multi-script frame time remain unmeasured.

The 250 ms outer-entry budget covers Script resumes, task and signal resumes,
RPC entries, Character policy and entitlement continuations. Modules loaded
synchronously inherit the caller's interval. Separate resumes after yielding
start fresh intervals. A native binding or GC slice can run until the next
execution safepoint. The documented bound counts execution safepoint visits.
Parallel script ownership, per-frame cumulative time and VM
memory admission are separate concerns. Keep the default for ordinary
gameplay. Trusted offline hosts can opt into cooperative debugging through
native configuration. Untrusted creator execution needs independent
isolation and quotas.

## Reliable receive ordering follow-up

With budget four and two busy peers, the old global deque started as
`A1 A2 A3 B1 B2 A4`. The pump served `A1 A2`, rotated `A3` to the tail
and served `B1 B2`. At the global budget exit the remaining queue was
`A4 A3`. This was a dispatch-ordering defect after valid transport admission.
The GNS and simulated transport polls drain their admitted events in order,
and `GameSession::Poll` hands Remote frames to the manager in that order.
The ordinary scheduler takes a connection's reliable application intents
from the front of their traffic-class queue.

Correction `39b74740244e94a955672dd6b676794a5e4945d6`, tree
`7ed2441fd20f865162669e7a99dbf77dc9d0dd27`, separates receive queue ownership
from scheduling. Each peer owns an ingress FIFO. A rotating list of ready
peers advances at service time. The original global and per-peer allowances
still bound every pump. The snapshot fences each peer's eligible prefix
before timeout or message callbacks. Reentrant arrivals wait for a later
pump, retirement adjusts the prefix and returns its charges, and nested
pumps dispatch zero additional messages. Older work stays at the front of
its own lane when the global budget ends. Receive validation either dispatches
or retires the selected frame in that turn. Materialization waits live on the
outgoing side, where they affect only the relevant ordering stream.

The adjacent outbound dependency queue previously let newer requests advance
the peer-wide request-ID watermark while an older request waited for an
Instance to materialize. It also let a newer event overtake an older waiting
event in the same publication. Waiting successors now share the existing
bounded deferred budgets. The promotion pass advances each shared request
stream and each event publication in order. Other peers, event publications
and replies retain separate progress. Replies correlate by request ID and
remain free to complete in handler completion order. Cancellation, expiry,
unpublication and disconnect retire deferred ownership through one accounting
path. The correction retains the wire protocol, deadlines, limits and
scheduler policies.

The updated native regressions fail on the old manager and pass on the corrected
manager. They cover one-shot and repeated budget exhaustion, a new tail behind
older work, mixed events, requests and correlated replies, callback admission,
nested pumping, unpublication and disconnect retirement, structured buffers,
outbound dependency release and cancellation, and repeated saturated rotation
with five continuously backlogged peers. A staged two-client localhost GNS
test repeats the receive failure with real reliable events and RPCs and checks
all correlated buffer replies exactly once.

Seven alternating MSVC Release processes per build measured the same admitted
and dispatched message counts. Each row reports medians in nanoseconds per
message for the original manager and the corrected manager.

| Peers, depth per peer, pump budget | Admission before → after | Pump before → after | Combined before → after |
| --- | ---: | ---: | ---: |
| 1, 1, 1 | 186 → 219 | 352 → 312 | 539 → 530 |
| 1, 64, 16 | 151 → 167 | 118 → 89 | 269 → 256 |
| 32, 16, 8 | 161 → 185 | 492 → 166 | 653 → 351 |

The empty-to-ready transition adds one list node allocation in the single-message
case. The pump now avoids rebuilding and scanning per-message fairness state.
Total requested allocations for that case fell from about eight to four per
message. Combined admission and pump time fell by about 46% in the 32-peer fixture.
These are manager microbenchmarks with a recording scheduler. The standalone
workloads exercise real GNS scheduling and workload correctness separately.

The corrected Windows Release runtime passed nine focused invocations and all
five existing standalone gates with zero performance misses. Its standalone
binary SHA-256 is
`67680c419fce0000dd35f2e253976d897101e76f4824d5aff4842249e1b51206`.
All 144 aggregate peer rows conserved accepted and completed RPCs with zero
errors. Ordinary peer RPC p95 was 56.2–74.8 ms and overload peer RPC p95
was 218.8–275.0 ms, close to the original 56.6–74.9 and 219.1–271.0 ms ranges.
These are latency observations within the aggregate fixture. The 32-peer
sample supports healthy service under the preserved workload limits.

The follow-up also passed nine focused Clang 19 RelWithDebInfo invocations
with ASan, UBSan and leak detection. The headless lane covered native Remote,
Remote Luau, GameSession, simulated Relay and the custom Character mission.
The GNS lane covered the two-client Remote regression, Relay, GameSession
and pooled feedback refresh. Windows and Linux recompiled the changed units
with the original build flags and relinked them with pinned compatible objects.
The follow-up worker outputs are under
`C:/Sandbox/Codex/Artifacts/Engine10BReceiveOrdering` and
`C:/Sandbox/Codex/Artifacts/Engine10B/Linux/ReceiveOrdering`.

## Qualification and provenance

- All 13 receipt hashes matched. Both source manifests bind 1,688 tracked
  files to the qualified source and the verified full-archive SHA-256. JUnit
  verified Windows 183 of 184 in the full run and the same pipe-drain CTest
  passing on replay, Linux headless 130 of 130 and real GNS 13 of 13 with zero
  skips. These are source-bound local worker results.
- The pipe-drain first stage passed in both runs. The original second stage
  had all 2,500 stdout rows and an extra stderr row reporting that `ping`
  could not be found. The retained replay used the same CTest entry and script,
  a process-local PATH containing System32, and all 33 children and 66 streams.
  The independent review repeated the same PowerShell command with System32
  available and passed. The CTest exercises the PowerShell pipe-drain
  implementation and Windows subprocesses directly. Historical binary hashes
  for the Windows dependencies before and after the original replay are absent.
- The five Release workload inputs changed the scalar C++ container type
  while preserving payload values, timing schedules and correctness gates.
  All five pinned runs emitted `POOLED_CORRECTNESS_V1`. Overload peer RPC p95
  was 219.1–271.0 ms across the three 32-peer cases measured in the aggregate
  fixture.
- The sanitizer fuzzer calls production `DecodeRemoteMessage`, asserts exact
  re-encoding for accepted bytes and checks decoded accounting against a
  separate argument walk. The archived 50,000 executions and 125 corpus
  replays exercise matching codec sources with seed 112410. RSS is process-wide.
  Per-packet allocation, session visibility, Luau materialization, delivery
  ordering and admission pressure need their own fixtures.
- The documentation receipts confirm 20 compiled Luau examples and 2,300
  checked site links. Those records apply to the documented original pin.
- The Windows review used the pinned MSVC Release objects, flags, link inputs
  and runtime resources on Dockerbox, with only the changed source objects
  and new fixtures recompiled in the review area. Foundation, GUI, Remote
  Luau, GameSession, three Relay variants, and localhost GNS Remote passed
  with the final reviewed objects. The earlier native-codec fuzz and
  untouched workload evidence retain their original provenance.
- The focused Linux RelWithDebInfo review recompiled the changed objects
  with the pinned Clang 19 flags and linked against the retained, separately
  configured headless and GNS objects. Foundation, GUI, Remote Luau and
  GameSession passed in the headless lane. Remote and Relay passed with
  real localhost GNS. All six passed with ASan, UBSan and leak detection.

The Windows worker reused the pinned `Engine10B/ReleaseGns` build and ran up
to three compilations in parallel. The Linux worker reused `/build-headless`
and `/build-engine` from the original qualification container with up to two
compilations in parallel. Review outputs are in
`C:/Sandbox/Codex/Artifacts/Engine10BIndependentReview` and
`C:/Sandbox/Codex/Artifacts/Engine10B/Linux/IndependentReview`. Selected raw
logs and the candidate receipt are in the controller's
`foundation-10b-independent-review` artifact directory.

The promotion branch is a documentation-only descendant of the qualified
correction at `39b747402`. It includes the accepted detached Instance equality
and latched deadline fixes and carries forward the source-equivalent
qualification evidence described above. Foundation 10A and the independent
continuation-lifecycle assignments retain their own acceptance criteria.
