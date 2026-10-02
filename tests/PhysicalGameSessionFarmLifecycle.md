# Farm32 logical shutdown evidence

`logical_stop_v1` transfers the existing qualified `GameSessionBenchmark`
post-Stop checks into both production hosts. Each successful scale invocation
emits one typed receipt after `GameSession::Stop` and `Engine::Destroy`, before
the owning C++ objects are reset. It reports the actual remaining connections,
journal readers and admission owners, exact reservation and retirement/terminal
conservation, zero outstanding debt/grants, and content transient ownership.
The session may already be terminal Failed after an expected server close;
the separate typed run result still rejects unexpected lifecycle failure.

Cached/resident bytes, record counts, resident objects and admission allocator
bytes are diagnostics, not newly invented zero-retention gates. Content's
requested/acquiring/prepared units and reserved-completion/completed/decoded
bytes use the benchmark's existing zero-transient-ownership requirements.
Absent client content services are explicit; the server must observe its provider.
Terminal release is reported independently and cannot masquerade as verified
retirement. Existing campaign service/convergence gates still apply.

`Read-FarmLifecycleObservation` independently replays all 33 identity-bound
receipts from immutable role logs, rejecting missing/duplicate/malformed fields,
retained transient owners and conservation errors. All-absent historical logs
remain `NOT_MEASURED`; partial evidence is an error. It records each source-log
SHA-256 for final reconciliation. A receipt is not a proof of whole-process heap
release or an unmeasured shutdown latency guarantee. Native and physical
qualification of a new candidate remain required.
