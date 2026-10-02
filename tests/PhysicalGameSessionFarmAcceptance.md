# Farm32 cross-provider acceptance observation

`PhysicalGameSessionFarmAcceptance.ps1` is an offline follow-up to two
successful role-local reconciliations: one Local and one Node. Supply the two
reconciliation JSON files and each run's immutable Server and Clients evidence
roots. It creates one new JSON report outside all evidence roots and rejects an
existing output path.

The report binds the two reconciliation hashes, four role-local evidence-index
hashes, distinct run IDs, and identical source, workload, binary, package,
content, and deployment pins. It independently checks the indexed resource CSVs
and reports per-process working-set, private-byte, thread, handle, and observed
CPU high-water/delta. `SumOfPerProcessPeak*` is a conservative sum of separate
process peaks, **not** a synchronized aggregate high-water. The offline
analyzer also groups rows bearing the same supervisor sweep timestamp,
requires one unique row from every expected role process for a complete
sweep, and reports the largest sum of working set and private bytes across
complete sweeps. It checks each complete working-set sweep against the
`AggregateWorkingSetLimitBytes` recorded in the indexed role result. A
partial sweep is counted but never used as a 32-process aggregate. The sweep
reads processes sequentially, so even a complete sweep is not an atomic
simultaneous high-water or a host-memory-headroom proof. The two hosts have
independent monotonic clocks; their CPU samples are not combined into a
cross-host utilization percentage.

Each role also seals `host-resources.csv` in its evidence index. The role-local
supervisor samples Windows cumulative idle/kernel/user CPU time, available and
total physical memory, a bounded sweep of currently live owned processes, and
the pinned 10-Gbps fiber adapter's byte/error/discard counters. Each row names
the host, role, provider, run, adapter index/MAC/address, monotonic start/end,
and snapshot skew. The offline analyzer rejects missing or unsealed rows,
counter reset/wrap, changed host/interface identity, invalid clock/CPU ranges,
missing all-process snapshots, and physically impossible link rates. It reports
CPU and NIC rates **per host and between adjacent samples**, plus minimum sampled
available memory and maximum sampled simultaneous owned working set. The sweep
is bounded in time but is not an atomic OS snapshot, and the fixed sampling
interval can miss short peaks. The Server and Clients samples cannot be combined
as one cross-host instant.

The report also lists each role's indexed evidence file count and declared
retained bytes from the prior reconciler's hash-verified index. This establishes
only the bounded role-local evidence set. It does not establish capture
retention, long-running resource stability, or production journal retention.

When **both** provider manifests select the bounded recovery workload, the
acceptance analyzer independently replays the canonical three-case recovery
parser over each run's indexed Server stdout/stderr and all 32 indexed Clients
stdout/stderr logs. It checks the sealed role results' run, manifest, provider,
endpoint, and scale identity, then compares every recomputed recovery case to
the separate reconciliation report. The fixed 20-second service-recovery gate
is `MEASURED_PASS` only if Local and Node each pass gameplay, structural, and
mixed service recovery. A valid failing case is reported as `MEASURED_FAIL`;
an absent workload is `NOT MEASURED`. The fixed ordinary-service observation
is separate from structural convergence. For each case, a frozen 3J quote is
audited against every post-cessation accepted complete frame; the parser
recomputes all 32 peers' exact `W_i`, the canonical workload-derived deadline,
terminal reader/source/debt conservation, and client final-Name observation.
Missing or divergent quote evidence fails closed. A parser fixture is not a
physical Local or Node recovery result.

The report independently re-reads the indexed Server `server.stdout.log`
native admission receipt and `admission-fairness.tsv` timeline, then compares
every final admission field and the full parsed fairness observation with the
reconciliation JSON. It rejects a rehashed timeline or plausible numeric
report change that differs from the native source. The per-provider admission
section reports exact terminal accepted/retired/debt conservation, observed
grant and credit high-water against their existing 4-grant, 512-KiB peer and
2-MiB global limits, backlog/deferral counters, and observed per-generation
eligibility waits. The accepted exact-demand episodes now have a separate
run-scoped verdict against the canonical 220.5-ms first-grant eligibility bound;
an interruption, disposal, or open demand makes complete exact-demand episode
coverage inconclusive. Even a passing set of accepted grants does not prove
continuous semantic backlog, sustained fair share, or overload backpressure.
The V2 native admission timeline adds generation- and token-scoped ACK
retirement, grant release, and terminal release after each accepted grant.
Its offline reader rejects a second grant for the same generation before
release, a release before retirement, duplicate lifecycle transitions, and
native active-grant counts that disagree with the reconstructed chronology.
A `grant_terminal_released` event distinguishes a zero-byte owner cleanup
after verified ACK retirement (`reason=none`, valid) from unreconciled
terminal debt (`reason=terminal_release`, failure). The canonical terminal
release bound applies to bytes, not the number of owner-cleanup events.
Historical V1 traces remain readable but their grant lifecycle is
`NOT_MEASURED`. The sealed admission timeline additionally rejects any snapshot exceeding the
four-grant or peer/global credit caps, a grant-accepted snapshot with no active
grant, or regressing cumulative deferrals. Its maxima and final sampled
deferrals must fit the native final high-water and counters, and terminal
pending enter/leave totals must reconcile. This is a bounded diagnostic
subset: the trace still has no continuous semantic backlog ledger, so it
cannot prove saturated fair-share service from credit-eligible candidates
alone. V2's lifecycle verdict must be reconciled into the final provider
acceptance gate; a parser-only fixture is not a physical result.

For Node, the report also checks the indexed authenticated manifest RPC
receipt against the run manifest and reconciliation fields. Without additional
Node evidence it preserves `RealTls=NOT_MEASURED`: `grpc_ssl_credentials` and a
matching root pin do not constitute a negotiated TLS-session proof. To measure
that one RPC's negotiated TLS, supply the separately pinned `node-tls-match.json`,
`node-stage.json`, and `node-run.json` paths and SHA-256 values together. The
analyzer replays the pinned Node TLS matcher against the indexed Server receipt,
owned-child stage/run receipt, and hash-bound Node log, then requires exact
agreement with the supplied match receipt. It reports the TLS version/cipher and
`NEGOTIATED_TLS_MANIFEST_RPC_MEASURED` only for that authenticated request. This
does not establish complete content delivery or full provider parity. Local
evidence containing a Node receipt is rejected. Source, workload and deployment
pin parity is measured; complete application/provider parity is not.

The report records the already typed five-phase observations and final native
admission conservation as measured subsets. It intentionally remains
`INCOMPLETE` / `Foundation3LQualification=NOT CLAIMED`. Current role-local
reconciliations also carry a hash-indexed native Character publication join
when the server and all 32 client binary traces exist. This analyzer replays
the join from both providers' sealed roots and compares the complete result,
including analyzer and parser source hashes,
with each reconciliation. It exposes exact accepted/observed state-chain
counts and server-local due-to-accept, forced-built-to-accept, and client-local
receive-to-handler durations. Eight explicit server root identities bind the
64 expected recipient relationships. When five sealed phase windows and every
root relationship are complete, the join checks recipient-local handled gaps
against 250 ms and authoritative state-tick deltas against 12 ticks. Ambiguous
phase edges remain `NOT_MEASURED`; proven threshold failures are reported as
failures. It does not infer cross-host one-way latency or full Remote recipient
cadence. A missing legacy trace set stays unmeasured, whereas an incomplete or
invalid set is rejected.

The designated Farm32 producer (client slot 0) now emits bounded Luau-local
Remote traces after each phase drains. The acceptance analyzer replays its
hash-indexed RPC invocation/return and Event offer/`OnClientEvent` callback
records, requiring 100 RPCs per phase within p95/p99/max 150/250/500 ms and
matched Events within 250 ms round trip and ACK service gap. Missing legacy
traces remain `NOT MEASURED`; partial or malformed present traces are rejected,
and a valid threshold violation is `MEASURED_FAIL`. The other 31 clients do not
produce this measured ScaleEvent/ScaleFunction workload, so their Remote
recipient service and cross-host one-way latency remain `NOT_MEASURED`.

Current role-local
evidence does **not** establish canonical CPU/memory/network headroom, fixed
20-second recovery or exact convergence without the separately indexed recovery workload,
journal
retention margin under overload, full fairness/backpressure, or full provider
parity. Negotiated real-TLS details remain unmeasured when the optional Node
owned-child evidence is not supplied. Terminal zero journal backlog and zero
failures do not establish the high-water retention margin. When the
indexed three-case recovery workload is present, the report separately records
that its 480 structural/mixed offer samples and recovery samples stayed within
the 16,384-record window with nonnegative observed reader margin. This sampled
verdict now also requires a retained, monotonic 7,680-observation count per
structural/mixed case: one reader-margin measurement immediately after each
committed Name mutation. Its native minimum may be lower than the 480
end-of-opportunity samples and is the authoritative overload minimum. The
source-log reader checks the count and bounded source-log ordering. This
closes the between-workload-mutation sampling hole for this fixed overload;
other commits between those mutations and independent sustained fairness or
backpressure behavior are not inferred from that observation.

The 4/8-GiB available-memory preflight and the endpoint's per-process/aggregate
working-set limits are protective run bounds, not a final host-memory acceptance
threshold. The 96-MiB/s backend envelope and its 64+8+4+8-MiB/s modeled
commitments are canonical deployment context; host/NIC counters alone cannot
prove reserved packet/retransmission capacity, provider overhead, or the
application latency gates. No CPU-percentage, RSS-plateau, or NIC-utilization
PASS threshold is inferred from a measured value.

Mock test:

```powershell
pwsh -NoProfile -File tests/PhysicalGameSessionFarmAcceptanceTests.ps1
pwsh -NoProfile -File tests/PhysicalGameSessionFarmHostResourceTests.ps1
```
