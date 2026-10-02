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

The offline parser mock test is
`pwsh -NoProfile -File tests/PhysicalGameSessionFarmCadenceTests.ps1`.
`ContentScaleGameplayLuauSyntaxTests.ps1` compiles the embedded client script
when `luau-compile.exe` is available. Neither test launches physical clients.
