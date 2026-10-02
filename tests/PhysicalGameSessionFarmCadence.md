# Farm32 actual-callback evidence

The five-phase farm workload counts invocations inside each Player's actual
`RunService.PostSimulation` Luau callback. Every 60 callbacks, the script
updates a local `ScaleCallbackBeat` attribute. PlayerHost validates that it
advanced by exactly 60 and records the local simulation tick and monotonic
timestamp after that callback returns. At each phase transition, the script
prints its phase-local callback count and cumulative total. The bounded farm
can produce at most 600 beat records per client under its existing 36,000-frame
manifest maximum, well below the 4-MiB per-stream log limit.

After role-local evidence is sealed, run `PhysicalGameSessionFarmCadence.ps1`
with its run ID, immutable Clients evidence root, and a new report path outside
that root. It checks all 32 ready identities, five ordered phase summaries,
exact callback-count conservation, 60-callback beat sequence, phase ordering,
and process-local monotonic tick/time order. The report is `OBSERVED`, not a
provider or Foundation 3L PASS. It gives the maximum interval between two
successive 60-callback beats and does not infer individual callback gaps from
that batch duration.

This trace does not join authoritative Character due/accepted states to each
recipient's observation. It also does not compare monotonic timestamps from
different PCs, claim one-way network latency, or prove a rendered frame. Those
gates stay `NOT_MEASURED` until distinct bounded identity-preserving traces
are available. The existing `ClientCharacterMaximumServiceGapNanoseconds`
metric is a cumulative gap between any handled Character messages, not a
per-recipient/root cadence measure.

The farm-only `publication-service.bin` server artifact and
`publication-service-<slot>.bin` artifacts for the 32 real Players now retain
fixed 80-byte records for authoritative due/snapshot/acceptance and first
polled native receive/handler completion. They retain full connection and
ObjectId generations, control and materialization epochs, authoritative tick,
state and frame sequences; no packet payload is retained. The server reserves
at most 320 MiB for 4,194,304 records, and each client reserves at most 10 MiB
for 131,072 records. Both buffers are allocated before measured work and
written to exclusive role-local files after it; any overflow, decode failure
or write failure invalidates the host result. The independent streaming parser
is `tools/physical-qualifier/farm_publication_trace.py`.

The native receive timestamp means first successful GNS poll of a decoded
message, not kernel arrival or rendered application visibility. The offline
`tools/physical-qualifier/farm_publication_join.py` joins sealed server/client
traces by full recipient/ObjectId generations, control/materialization epochs,
authoritative tick, state and frame sequences, and exact attributed state
bytes. Run-scoped ready nonces map server connections to separately allocated
client-local connections; numeric `ConnectionId` values are never equated
across hosts. The join fails on overflow or missing/duplicate accepted and
observed states. It reports forced states, unchanged suppression, forecast
rescheduling, server-local due-to-accept and client-local receive-to-handler
durations separately. The bounded database lives only under an explicit
task-owned analysis directory and is removed after analysis.

The Stage 11 `RecipientRetired` record is emitted only after an accepted GRPL
Unpublish/Destroy and retains full recipient/ObjectId generations. The offline
join treats it as an ordered relationship transition: it disposes only pending
due work before that accepted leave, counts confirmed due work retired
separately, and permits a later same-generation schedule and second retirement.
Missing due origins, overdue forecasts, unresolved due work, unaccepted
production, or unobserved accepted states invalidate due-chain conservation.
The binary trace does not itself record the corresponding GRPL republish; a
later Character schedule is evidence of reentry, not direct republish proof.
An accepted due can also be explicitly rescheduled and rediscovered within
the same authoritative tick; the join assigns a new obligation epoch only at
that later schedule, so a second due event without rearm remains a replay.
The direct reliable `PublishState` path from `Step` is distinct from scheduled
`PublishStateFrames`: Stage 12 `CharacterDirectOffered` precedes scheduler
submission and carries the exact state/materialization identity and reliable
bit. Stage 8 acceptance completes that offer, while Stage 13
`CharacterDirectRejected` explicitly disposes a failed submission. A direct
offer may satisfy only a previously confirmed due for the same relationship;
an unconfirmed due forecast is not a fabricated due observation. The join
requires every direct offer to be accepted or rejected exactly once and
reports direct built-to-accepted time separately from scheduled due latency.
Older sealed traces without Stage 12 cannot be reclassified as direct by
inference; an unmatched Stage 8 still fails closed.
Server and client steady clocks have unrelated origins, so cross-host
due-to-handler latency and phase-long drift remain `NOT_MEASURED`. This
role-local result alone is not Foundation 3L acceptance.

The Farm32 reconciler now runs that hash-indexed join when the server and all
32 client traces are present. It records the join and its index/manifest pins
as `PublicationObservation`, including the join and trace-parser source hashes,
and marks only the Character accepted-state chain
and role-local due-to-accept, forced-built-to-accept and receive-to-handler
delays as measured. The cross-provider acceptance analyzer independently
replays the same sealed join for Local and Node and rejects a changed
reconciliation value. Legacy evidence with no publication traces remains
`NOT_MEASURED`; a partial trace set is rejected. Each replay uses a bounded,
temporary analysis directory outside the sealed roots. Full Character
recipient cadence, cross-host due-to-handler latency, and Remote cadence stay
unmeasured because these records do not contain a shared clock or Remote
offer/send/handler events. Both reports remain `INCOMPLETE` and do not claim
provider or Foundation 3L qualification.

This artifact is diagnostic-only until its timing cost is qualified against a
full 32-client workload. A Release MSVC/GNS microbenchmark on the worker,
excluding buffer allocation and terminal file output, measured 48.6–49.8 ns
per retained `CharacterDue` record and 1.901–1.908 µs per eight-state GCHR
packet decoded and retained at `SchedulerAccepted` (three repeats). The
inactive-sink cases measured about 0.66 ns per scalar event and 0.44 ns per
packet. This isolated A/B estimate does not prove that recording leaves the
physical due-service timing unchanged. Run the reproducible estimate with
`gargantuan_game_session_tests --farm-publication-overhead`.

The offline parser mock test is
`pwsh -NoProfile -File tests/PhysicalGameSessionFarmCadenceTests.ps1`.
`ContentScaleGameplayLuauSyntaxTests.ps1` compiles the embedded client script
when `luau-compile.exe` is available. Neither test launches physical clients.
