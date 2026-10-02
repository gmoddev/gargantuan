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

The report also lists each role's indexed evidence file count and declared
retained bytes from the prior reconciler's hash-verified index. This establishes
only the bounded role-local evidence set. It does not establish capture
retention, long-running resource stability, or production journal retention.

The report records the already typed five-phase observations and final native
admission conservation as measured subsets. It intentionally remains
`INCOMPLETE` / `Foundation3LQualification=NOT CLAIMED`. Current role-local
evidence does **not** establish canonical CPU/memory/network headroom, fixed
20-second recovery, workload-derived exact convergence timing, journal
retention margin under overload, full provider parity, or negotiated real-TLS
transport details. Those gates remain `NOT MEASURED` until their own bounded
typed traces and acceptance analyzers exist. A Node authenticated manifest RPC
receipt is a separate observation, not a full TLS transcript.

Mock test:

```powershell
pwsh -NoProfile -File tests/PhysicalGameSessionFarmAcceptanceTests.ps1
```
