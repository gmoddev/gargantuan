# Foundation 10B independent promotion review

**Hold promotion.** Two bounded runtime corrections passed focused Windows
Release verification. The inherited reliable receive queue can still reorder
gameplay callbacks. Its correction and integration evidence are required for
the reliable-ordered contract in the 10B assignment.

## Reviewed source

| Revision | Commit | Tree |
| --- | --- | --- |
| Foundation 9 baseline | `bf50b0987fde410995d6d3169c2fde68a2464a74` | `bde7e5194e2c1b08688ce431c1b6e24d523a3a69` |
| Qualified implementation | `0ea0b4a047c6fe1c6e78008604d82dd1de8a6a65` | `9994dbc423ba2f184c56644adf182341d12e8663` |
| Reported three-commit candidate | `b960c091ae2ad6385548d0cca4ce3b74599bf04b` | `784aa66f901efa12844ae465def321984294608d` |
| Original development branch, `foundation/engine-10b` | `b236bcf8dddee623b05ec53c52d7409657a56c89` | `784aa66f901efa12844ae465def321984294608d` |
| Runtime correction on the independent branch | `cded087d40ed6b4dc31e3231dd771beaf8dfc091` | `b8d0353a913ea11bd186e57ab1fca1a62b4b70fe` |

The candidate's second commit has the exact qualified implementation tree.
Its third commit changes six documentation and inventory paths. The original
development tip and reported candidate have identical complete trees. The
review uses the original developer-runtime assignment from chat
`01a210e6-e482-7390-a4fd-c25bf01189af`, the strategic reconciliation
`0195d658d`, and the Foundations 5–8 assurance register at `7c0aca159`.

## Findings and dispositions

| Finding | Classification and impact | Evidence, correction, and qualification |
| --- | --- | --- |
| Reliable callback order across bounded pumps | **Promotion blocker, inherited.** A healthy connection can deliver reliable messages to native admission in order and still run their gameplay callbacks out of order. An older valid RPC can also fail the monotonic-ID check after a newer request dispatches first. | High confidence. `RemoteManager::Pump` rotates ineligible entries to the back, then can reach the global message limit before visiting the original tail. A Windows Release native fixture fed four ordered structured events from one peer and two from another. It observed `1, 2, 4, 3` across two pumps. The same rotation and early exit exist at `bf50b0987` and trace to `d52256135`. Correct this in the receive-queue owner. Preserve peer fairness, FIFO for queued work, accounting, peer teardown, and reentrant enqueue/retirement. Qualify interleaved reliable events and requests, bounded pump exit and removal, simulated and real GNS service, and the existing correctness workloads after integration. |
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

The review branch consolidates the corrections on top of the original
three-commit candidate. Hold integration until the reliable FIFO dependency
is corrected and qualified, then rerun the affected integration and sanitizer
gates. The candidate's native validation and the public release gate remain
separate decisions.
