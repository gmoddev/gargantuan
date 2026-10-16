---
status: current-infrastructure-extraction
owner: qualification-tooling
last_verified: 2026-10-04
---

# Physical agent coordination ownership and migration

The September 28 interactive physical attempt used temporary named profiles
under the two logged-in non-admin users and bypassed the offline worker socket
broker. Both host-owned SSH tunnel directions and the lifecycle registration
passed. GNS and GameSession reached one-client Ready, but worker miniport capture
missed all inbound packets, so the physical and lifecycle verdicts failed.
This [receipt](PooledPhysicalQualification3L.md#interactive-one-client-gns-and-ready-passed-worker-ingress-capture-failed-2026-09-28)
does not revise the independently qualified Foundation 2B restricted profile or
claim that its sandbox permits physical egress. No retry or four-client run was
made. The temporary profiles and tasks were removed; KI-006 remains OPEN and
Foundation 3L B — PARTIALLY READY.

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
pins `cf1628de587ee0ccfbd74742d62946dac1adf522` and the exact consumed
source SHA-256 values, including the two local owned-process modules. The installed
Farm32 baseline is `5ee889fab571a9047cbc0f8724f2077c7bb9eb74`; a later source
pin does not qualify replacement endpoint artifacts or installed services.

The pinned legacy control implementation now exposes a separate, bounded
control-only preflight through the same lifecycle child-launch path used by
physical Phase 1. It instruments the actual coordinator listener and worker
child context, reuses the production source-bind/connect and authenticated
`STAGE_READY` implementation, and completes without a capture or GNS probe.
The Gargantuan adapter gates it behind dedicated capabilities and a distinct
workflow/result classification. Its evidence is a prerequisite for a fresh
physical attempt, not a substitute for physical F1 service, delivery, capture,
or convergence evidence.
During the first actual-child control preflight, Windows Firewall created an
inbound Block rule for the newly versioned client Python executable at listener
startup. The earlier physical timeout had the same cause; its rule became Allow
only after the run ended. The deployment now uses the hash-identical installed
client runtime with an existing inbound Allow rule and a pre-stage denial if
that executable has a Block rule. Three fresh real-child control-only runs
passed under this selection. A separate Python process's LAN check remains
supplemental evidence, not proof of the physical child path.
`bootstrap.py` fetches that revision into ignored `.agent-coordinator`; imports
fail closed on missing/changed pinned sources. New packages embed the snapshot and
adapter, including the skill entrypoint. Old installed standalone packages remain
valid independently of this checkout.

The current physical adapter uses the existing legacy readiness profile. Four-client
staging now supplies a trusted `ResultClassification` to all three configs, and
the pinned coordinator requires the two endpoint results to agree with it.
One-client staging retains its previous classification. Standalone schema-control
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

### Farm32 deployment envelope and native runtime closure

The hash-pinned qualified role artifact is a deployment envelope. Its
`deployment-sha256.json` is qualification evidence and is not native game
content. Before ARM, source-owned staging derives a new run/role runtime
projection from the exact game content table plus `game.package.json`, checks
equality with the pinned deployment inventory, and verifies every copied byte.
The inventory and projection receipt stay outside the executable's closed
runtime root. Read-only preflight and the endpoint verify the prepared copy;
the endpoint launches that copy. Original envelopes and all eight manifest pins
remain unchanged. Stale or partial projections fail closed and remain evidence;
staging never deletes or retries them. Native package integrity remains enforced.

Projection preparation and prepared-copy validation also enforce the native
package builder's existing 240-character absolute-path limit for every runtime
member and its directory ancestors. The .NET copier's long-path support is not
evidence that the pinned native host can read those paths. Oversized paths are
rejected before any registry, attempt marker, runtime copy, or receipt is
published. Choose compact, fresh run/role registry names; retain full run
identity and exact content hashes. Do not change Windows global long-path
settings or native package contents to accommodate qualifier naming.

Before launching either farm role, the outer controller binds a temporary
worker game-UDP permission to the sealed run's exact projected
`GargantuanServer.exe`, its SHA-256, the verified native closure, and the
preflight projection receipt. The rule allows only inbound UDP `39450` from
`10.253.3.1` to `10.253.3.2` on the currently verified Private fiber interface.
It is separate from the normal-LAN coordinator TCP permission. An original
package-path exception does not authorize a different projected executable.

The rule has a run UUID name and independent ownership marker. Existing names,
changed projection bytes, ambiguous interfaces, non-Private profiles and
potentially applicable explicit Blocks deny launch. Both persistent and
effective rule filters must match before the roles start. Every launch exit
reconciles this exact owned rule; altered ownership or scope denies removal
and fails cleanup. Unrelated rules and firewall profiles remain unchanged.
Rule publication is an infrastructure prerequisite, never application
readiness, packet-delivery evidence or provider acceptance.

The October 4 Local32 startup failure exposed the missing distinction. It is
historical failed infrastructure evidence, not a production F1 measurement or a
provider PASS. Projection preparation does not count as a physical run or
replace native package validation and the canonical provider/evidence gates.

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

### Farm32 helper and staged ownership imports

The fixed worker helper directory contains the endpoint adapter and ACL module;
the per-run stage owns `dependency.py`, `upstream.lock.json` and the exact pinned
Coordinator exports. The endpoint loads its ownership dependency by that verified
stage path, rechecks the dependency/lock bytes against the stage index, and checks
all pinned exports before loading the owned-process modules. A dedicated package
namespace binds relative ownership imports to the same stage; ambient Python
imports and a helper-directory dependency cannot supply this authority.

Owned-child startup failures retain a bounded stage-local diagnostic with phase,
error type and cleanup error. Unknown child ownership remains false and does not
create a successful terminal receipt. The outer controller reports its original
listener/launch refusal together with any subsequent cleanup refusal. These are
qualification infrastructure diagnostics, not provider or Foundation 3L PASS.

### Farm32 launch, sampling and failed-run evidence

The failed Local32 run `68bfec9e-1335-4f45-b7ad-9ea502bd2592`
reached 28 ready clients and stopped before the workload. Repeated synchronous
resource queries on the client launch path delayed subsequent process starts.
The individual blocking operating-system call was not measured. The
[physical receipt](PooledPhysicalQualification3L.md) retains the original
failure, incomplete capture collection and unproven worker tree cleanup.

Resource sampling now runs in a separate bounded, hidden endpoint child.
Startup publishes only the new owner's native process counters and immutable
PID/start identity; it does not query the NIC or resample prior owners. The
sampler takes the existing two-second process/host snapshots and retains the
five-second snapshot-duration, row-count and log bounds. Its seven function
definitions come from the staged endpoint source; configuration, extracted
source, runtime, owner identities and partial evidence are hashed and sealed.
The role prepares the fixed CPU/memory counter and dedicated pipe-reader C#
assembly once in its generation/role-owned registry directory before the run
clock. The separate child loads the bounded, source/runtime/identity-pinned assembly from
verified held bytes instead of compiling those definitions again. Loaded type
origins must match that assembly; ambient or replaced types cannot supply the
counter authority. This is run-owned preparation, not an installed helper or
shared cache. Startup still includes child launch, verification/loading and
the complete actual first baseline within five seconds. Bounded phase records
and the always-retained hosted fixture directory preserve future refusals;
they do not extend any startup, query or stop deadline.
Runtime preparation and sampling directories stay outside the sealed flat
role-evidence root. After joined stop, the role retains the exact bounded
assembly, preparation/source/index receipts and a complete sampler-raw archive
as indexed top-level files. The archive obeys the existing 16-MiB member limit;
overflow is a qualification failure with the original registry raw preserved.
Every archive entry is joined to its raw byte/hash index. This preserves the
existing 128-member complete flat-directory acceptance check and worker
collection instead of exempting sampler directories from coverage.
It is stopped and its streams joined before native owner objects are disposed.
This changes sampling placement, not the bootstrap deadline or resource gates.

The fixed local endpoint adapter uses generic Windows Job containment from
process creation, including nested child launches. Job membership and checked
handle release determine cleanup independently of workload exit status.
Cleanup uses one bounded deadline; output draining has its own bounded phase.
The existing terminal v1 representation remains unchanged. An optional separate
terminal diagnostic records cleanup errors without manufacturing a successful
exit or a tree-reaped result. This local facility adds no wire command or
arbitrary remote process capability.

A failed workload can retain a `FAILED_DIAGNOSTIC` capture export only after
owned capture stop, the existing hook/profile checks and zero-loss validation.
Success binding rejects that classification. Missing or invalid role evidence
remains missing or invalid; diagnostic recovery cannot repair the original
provider verdict. Hard-abort capture cleanup still belongs to the existing
bounded service lease/watchdog. Installed capture services and hooks are not
changed by these source corrections. A fresh candidate must independently pass
source, dependency, hosted CI and endpoint preflight before another provider run.

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
