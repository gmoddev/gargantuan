---
status: current-infrastructure-extraction
owner: qualification-tooling
last_verified: 2026-09-27
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
