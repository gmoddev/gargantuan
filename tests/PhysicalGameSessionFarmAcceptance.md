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

The report independently re-reads the indexed Server `server.stdout.log`
native admission receipt and `admission-fairness.tsv` timeline, then compares
every final admission field and the full parsed fairness observation with the
reconciliation JSON. It rejects a rehashed timeline or plausible numeric
report change that differs from the native source. The per-provider admission
section reports exact terminal accepted/retired/debt conservation, observed
grant and credit high-water against their existing 4-grant, 512-KiB peer and
2-MiB global limits, backlog/deferral counters, and observed per-generation
eligibility waits. These are bounded subsets. A maximum observed wait by itself
does not establish the canonical fairness verdict or overload backpressure.

For Node, the report also checks the indexed authenticated manifest RPC
receipt against the run manifest and reconciliation fields. It preserves
`RealTls=NOT_MEASURED`: `grpc_ssl_credentials` and a matching root pin do not
constitute a negotiated TLS-session or full provider provenance proof. Local
evidence containing a Node receipt is rejected. Source, workload and
deployment pin parity is measured; complete application/provider parity is not.

The report records the already typed five-phase observations and final native
admission conservation as measured subsets. It intentionally remains
`INCOMPLETE` / `Foundation3LQualification=NOT CLAIMED`. Current role-local
evidence does **not** establish canonical CPU/memory/network headroom, fixed
20-second recovery, workload-derived exact convergence timing, journal
retention margin under overload, full fairness/backpressure, full provider
parity, or negotiated real-TLS transport details. Terminal zero journal backlog
and zero failures do not establish the high-water retention margin. Those gates
remain `NOT MEASURED` until their own bounded typed traces and acceptance
analyzers exist.

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
