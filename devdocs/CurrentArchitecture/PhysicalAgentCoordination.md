---
status: current-infrastructure-extraction
owner: qualification-tooling
last_verified: 2026-09-28
---

# Physical agent coordination ownership and migration

Generic LAN coordination now belongs to
[GantriaEngine/agent-coordinator](https://github.com/GantriaEngine/agent-coordinator).
It was extracted from the previously untracked `tools/physical-qualifier` working
implementation at Gargantuan base `b471b78e78376fb90a50771c1b99e3f8bfe82a4e`;
upstream records source hashes and does not claim that source was committed there.

Gargantuan's [adapter](../../tools/physical-qualifier/README.md) retains exact
probe/source manifest identities, GNS server/client argv, listener/PID checks,
fixed physical capture hook, capture-direction validation, idempotent cleanup,
readiness result classification and Foundation acceptance policy. The pinned
library owns TCP envelope/journal/manifests and coordinator/endpoint control
lifecycles. Concurrently verified late FINALIZE handling is preserved upstream;
project packet-direction acceptance remains local. Production networking is not
part of this dependency.

## Consumption decision

Use a hash-verified immutable source bootstrap rather than adding a submodule or
committing a second generic implementation. [The lock](../../tools/physical-qualifier/upstream.lock.json)
pins `24edb592ace678124731455b841e2623f64ba57a` and consumed-source SHA-256 values.
`bootstrap.py` fetches that revision into ignored `.agent-coordinator`; imports
fail closed on missing/changed pinned sources. New packages embed the snapshot and
adapter, including the skill entrypoint. Old installed standalone packages remain
valid independently of this checkout.

The current physical adapter uses the existing legacy readiness profile; its
three staged configs and wire messages remain unchanged. Standalone schema-control
v1 additionally supports locally installed schemas/capability catalogs and
authenticated short-lived assignment pull, with separate registration/execution
budgets. It is an available infrastructure API, not silently substituted for the
physical workflow. Applying discovery to Gargantuan requires a separately qualified
project adapter. Neither schemas nor assignment messages carry executable code.

The project now has a fixed, endpoint-local Foundation 2B lifecycle catalog
and one-client workflow layered over the legacy readiness barrier. The catalog
starts only the locally staged qualifier processes; Agent Coordinator carries
versioned capabilities and result metadata. The first full lifecycle attempt
registered both Codex agents but aborted before either physical endpoint ran:
the client Codex sandbox could not create its local coordinator evidence
directory. This does not qualify assignment-pull adoption or the one-client
readiness gate. The failed identity is consumed; a later attempt requires a
new run and a verified writable client evidence location. The worker's
Mellanox-miniport hook independently passed a bounded bidirectional synthetic
capture and is installed with its matching service hash pin, while the service
binary and 90-second lease remain unchanged.

The next authorized one-client attempt used physical run
`ff95b60e-8c5e-4125-bbc9-73e1e6844231` and lifecycle run
`960369f2-08a5-40d5-9253-9cfe86786220`. A fixed-root provisioning step
protected `C:\Sandbox\Codex\Evidence\physical-qualifier` from inherited ACLs
and granted only SYSTEM, Administrators and its owner full access, plus the
installed `CodexSandboxOffline` SID modify access. No UAC or elevated Codex
agent was needed. The actual restricted client identity, SID ending `-1004`,
passed fresh-directory create, file write/flush/read/rename/delete before either
agent woke. Both agents pulled fresh assignments, registered and reached the
barrier; the client returned the first LIVE result. The worker then failed
creating its fresh service-evidence directory with `WinError 5`, and the host
aborted. No GNS listener, GameSession result or packet capture was obtained.
The first cause is the worker's separate evidence-root ACL, not the qualified
capture hook. The run was not retried. Temporary daemons, tunnel and task were
removed, endpoint policy restored, and worker Packet Monitor was idle with no
filters. The next task is a bounded **worker evidence-root** access repair and
restricted worker preflight before a newly authorized one-client attempt.
KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY READY.

The subsequent per-endpoint evidence correction kept an internal allowlist of
only the client and worker qualification roots. On the worker, the installed
`HOSTPC\CodexSandboxOffline` identity has SID ending `-1006`, medium integrity
and no administrator group. The worker service-evidence ACL now retains SYSTEM
and Administrators full access and the capture service installer `HOSTPC\host`
modify access, and adds only that exact worker SID with modify access. The
capture service's configured evidence root and hook pin were unchanged. Fresh
physical run `c9130ef2-24a9-4d9f-a76f-96f56d5ae629` passed real restricted
create/write/flush/read/rename/delete preflights on **both** endpoints before
either lifecycle daemon started. The worker preflight removed its run directory
so the endpoint could create it afresh. No UAC or elevated Codex agent was used.
The one authorized lifecycle run `ea43f42e-8d02-4068-b68c-29e3527ccad1`
pulled and registered both agents; the client returned LIVE. The worker then
created and wrote its run evidence, but failed socket setup with `WinError
10013` before `STAGE_READY`, capture or GNS. The host aborted on the failed
worker result. No retry occurred. Temporary daemons and tunnel were removed,
policy restored, and worker Packet Monitor stopped with no filters. The exact
socket operation and policy owner require a separate diagnostic; this evidence
does not justify relaxing sandbox or network security policy. KI-006 remains
OPEN and Foundation 3L remains B — PARTIALLY READY.

The next investigation reproduced the worker's `WinError 10013` at the legacy
normal-LAN TCP `connect`, after socket creation, worker-source bind and timeout
had succeeded. The worker sandbox profile intentionally disables network
access; an enabled outbound firewall block targets its exact offline SID.
The same executable connected from the limited interactive worker host. A
Gargantuan-only one-run broker now owns the fixed worker qualifier endpoint
outside the sandbox, while the restricted catalog sends only a bounded
preflight/start request. The broker pins the run, source and config hashes and
rejects caller-supplied commands, addresses and ports. Neither the generic
Agent Coordinator nor the installed capture service was changed. Physical
run `722dfaa2-6ac6-4ae4-a67b-c226944c2528` passed both restricted evidence
preflights and the restricted-to-broker socket preflight. Its lifecycle run
`0cf08bfe-62f7-423e-a4b5-33636e0f2a1b` stopped before the worker
registered: the task's SSH tunnel lacked the reverse worker loopback listener
on port 49961, and the worker recorded `WinError 10061`. The client registered;
the host timed out waiting for its peer. There was no capture or GNS launch,
and no retry. The current adapter replaces the listener-only gate with
a host-owned, fixed two-direction SSH session. An authenticated worker daemon
presence call proves the main forward, and a restricted-worker nonce exchange
proves the reverse before any assignment is created or agent wakes. A consumed
label or stale listener fails closed; the owned SSH process and both listeners
are checked on cleanup. This is qualification transport only and does not
extend the worker broker's fixed physical endpoint capability.
Both agents ended FAILED and all task-owned daemons, tunnel, broker task and
staged policy were cleaned. KI-006 remains OPEN; Foundation 3L remains
B — PARTIALLY READY. The subsequent fresh run passed both tunnel handshakes
and both agent registrations, but the physical client GNS connection timed out
with zero qualified packets in either capture. The [physical receipt](PooledPhysicalQualification3L.md)
records its first failure and cleanup. The next task is bounded attribution
of the client-to-worker fiber GNS path before another fresh one-client run.

> The protocol coordinates capabilities; it does not transmit authority.
>
> Natural-language agent communication does not directly invoke endpoint capabilities.

## Two-PC migration

Keep the currently installed tools/runtime/service on the controlling PC and
dockerbox. Package the pinned adapter/library into a fresh sibling bundle, verify
every manifest hash and run control/hook tests. Stage only when directed; do not
launch probes/captures as an installation check. Under separate physical-attempt
authorization, locally qualify the extracted legacy profile on both PCs and
reconcile captures/results/cleanup. Switch installed entrypoints only after that
passes; retain the old package/service as rollback. Generic service installation
is separate, uses a fresh evidence root and a different service/pipe name, and
must preserve the exact project capture hook. Do not alter a live worker's tools.

## Validation and limitations

Standalone Python suites cover protocol/auth/replay, strict schema/capability
denial, assignment freshness/expiry, separated budgets, cleanup and local two-role
integration. Windows mock-hook tests cover fixed operations, path/hash rejection,
helper exit, real 90-second bound, service stop and unauthorized SID pipe denial.
Windows/Linux CI, wheel packaging, .NET publish, documentation links and whitespace
are checked upstream. Gargantuan's original and concurrent project tests run
against the pin, with its capture-hook simulation and bundle integrity checks.

No physical capture or both-PC migration is claimed by these tests. The standalone
foundation is not yet READY FOR GENERAL USE: trusted-LAN unencrypted TCP, one
endpoint per role, ordered numeric-parameter schemas and cooperative local handler
deadlines are explicit limits. Installed service ACL/crash recovery and actual
physical capture still require deployment qualification.

This extraction does not change [Foundation 3L physical qualification](PhysicalDeploymentPreflight3L.md),
POOLED_SERVICE, acceptance status or KI-006, and does not begin 3M. The exact next
task is a separately authorized two-PC qualification of the pinned legacy adapter
bundle, followed by a bounded Gargantuan assignment-pull adapter if desired.
