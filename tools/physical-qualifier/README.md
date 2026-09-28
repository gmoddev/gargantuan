# Gargantuan physical qualification adapter

Generic transport, coordinator, endpoint and privileged capture implementation is
owned by [GantriaEngine Agent Coordinator](https://github.com/GantriaEngine/agent-coordinator).
This directory retains Gargantuan's exact probe/source identities, fixed GNS argv,
physical capture hook, staging, local execution adapter and evidence acceptance.

```text
python tools/physical-qualifier/bootstrap.py
python tools/physical-qualifier/bootstrap.py --check
python -m unittest discover -s tools/physical-qualifier/tests -v
python tools/physical-qualifier/qualifier.py stage --help
```

[upstream.lock.json](upstream.lock.json) pins commit
`24edb592ace678124731455b841e2623f64ba57a` and SHA-256 of the consumed sources.
Bootstrap downloads to an ignored local directory; import fails closed if sources
are absent/changed. It does not install services, change network policy, replace
physical-PC tooling, or run captures/probes. Existing installed standalone copies
remain the operational baseline until the new bundle is qualified on both PCs.

[package.py](package.py) includes the pinned snapshot plus the project adapter in
a fresh bundle; all files are hashed. Skill entrypoints are replaced inside the
new package only. Keep old installed bundles/services as rollback. Follow the
[migration receipt](../../devdocs/CurrentArchitecture/PhysicalAgentCoordination.md)
and [physical protocol](docs/PROTOCOL.md) before authorized use.

The generic schema example is not a Foundation 3L workflow. This adapter keeps the
legacy v1 readiness profile, including early-server-result handling. The local
stage writes three config files; the project-specific Foundation 2B lifecycle
catalog and [one-client workflow](workflows/one-client-lifecycle.json) wrap that
legacy barrier without transmitting physical commands. Its first full run
registered both agents but aborted before capture or probe because the client
Codex sandbox could not create its staged evidence directory. The next attempt
proved the client fix but found the worker's separate evidence ACL missing its
restricted SID. The fixed-root
[`Ensure-QualificationEvidenceRoot.ps1`](Ensure-QualificationEvidenceRoot.ps1)
now selects only the approved client or worker root by endpoint kind and
preserves the worker capture service's SYSTEM/installer access. The
[`run_one_client_lifecycle.py`](run_one_client_lifecycle.py) `preflight` mode
requires both exact restricted-identity proofs before daemon start; its `run`
mode refuses wake without fresh matching proofs. The
[`evidence_preflight.py`](evidence_preflight.py) worker path removes its
temporary run directory so the qualifier can create it normally. Both real
sandbox preflights passed in the next single attempt. That attempt stopped on
worker socket `WinError 10013` before capture or GNS; it was not retried. All
failed physical and lifecycle IDs are consumed. See the
[physical receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
and [protocol record](docs/PROTOCOL.md).

The worker `WinError 10013` was reproduced at the LAN TCP `connect` and traced
to the offline Codex SID's non-loopback outbound block. The worker now invokes
a one-run, pinned local broker through fixed `PREFLIGHT` and `START` requests;
the limited interactive host broker owns the normal-LAN socket and launches
only the exact staged qualifier endpoint. The restricted profile and firewall
rule stay in place. The real restricted-identity broker preflight passed, along
with both evidence preflights, for physical run
`722dfaa2-6ac6-4ae4-a67b-c226944c2528`. Its sole lifecycle attempt
`0cf08bfe-62f7-423e-a4b5-33636e0f2a1b` then failed before the worker
registered because the SSH tunnel lacked the worker-to-host reverse loopback
forward for port 49961. The host-owned `run` adapter now starts one fixed SSH
session with both client-to-worker daemon
`-L 127.0.0.1:49964:127.0.0.1:49963` and worker-to-host coordinator
`-R 127.0.0.1:49961:127.0.0.1:49961`. It authenticates the worker daemon
through the forward and requires a run-bound nonce exchange from the worker's
actual restricted SID through the reverse before creating a lifecycle assignment.
The SSH process and both listeners are checked again during cleanup. A consumed
physical label cannot reuse an old tunnel session. No physical endpoint, capture or GNS
started in that historical run, which was not retried. A later fresh run
passed both tunnel handshakes and registration, then failed on the physical
client-to-worker GNS connection with zero qualified packets in either capture.
Its [receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
records the one-run failure and cleanup. Four-client readiness remains gated
on a later one-client PASS.

POOLED_SERVICE, physical gates, KI-006, Foundation 3L status and 3M are unchanged.
