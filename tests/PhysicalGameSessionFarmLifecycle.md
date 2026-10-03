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

`remote_ownership_v1` additionally records exact source-transition high-water
marks for dispatch, materialization deferral, outgoing requests and incoming
handler work, both manager-wide and per peer. The existing negotiated and native
bounds are checked at insertion. Every removal (dispatch, reply, timeout,
revocation, disconnect) contributes to an exact release total; final current
ownership must be zero. `GameSession` retains this bounded scalar summary before
destroying its RemoteManager, so an absent manager cannot create a fake zero.

Residence maxima and deadline overshoot are diagnostics. The 30-second handler
work lease is expired by the next `Pump`, not arbitrary script preemption; no
new latency constant is introduced. Destruction does not call an externally
owned clock to manufacture a timing observation. The normal session Stop removes
its peers while the manager is active and records their actual terminal ages.
RPC response scheduling still uses the existing `RpcResponse` producer queue
depth/service-age trace; this does not add a second response queue.

`Read-FarmRemoteOwnershipObservation` independently verifies all 33 receipts,
source identities, safety ceilings and accepted/released conservation. The
recorded per-peer bound-violation counter covers the actual negotiated limits at
each insertion. Historical all-absent evidence remains `NOT_MEASURED`. Whole-run
service metrics and response scheduler traces remain independent requirements;
a healthy queue summary does not by itself prove Remote latency service.
