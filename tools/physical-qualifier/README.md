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

The explicit `stage --clients 4 --phase1` path selects the pinned four-client
non-smoke probe, `FOUR_CLIENT_PHASE1_ONLY` result classification, 70-second
client capture finalization and the bounded
[`four-client-phase1-lifecycle.json`](workflows/four-client-phase1-lifecycle.json)
workflow. Its server result requires the probe's strengthened four-grant funding
verdict and metrics. It launches exactly one gameplay producer (the first
client nonce) and three non-producers; the endpoint rejects a staged Phase 1
client config with a different producer flag. Readiness staging keeps all four
producer flags at zero and remains `FOUR_CLIENT_READINESS_ONLY`.
Abort cleanup also waits for the timed client capture to close its final pcapng
block. Worker capture stop waits for the service's bounded completed-export
response before direction validation; failure to acknowledge stays a cleanup
error. The installed endpoint copies are not updated by this source change.
This command writes configs only. The artifact pin, both evidence roots, worker
capture service and hook, fiber/LAN state, lifecycle profiles and endpoint
leases still require independent preflight before a physical attempt.
The lifecycle runner performs its own forward/reverse tunnel proof before
assignment. That worker proof is one-use per stage label: do not call the
tunnel preflight separately with the same label and then invoke the runner.

[upstream.lock.json](upstream.lock.json) pins commit
`9c81cfb16640dc18e29b6253fc0f5463c4c4dd4b` and SHA-256 of the consumed sources.

For an explicitly authorized physical qualification, `stage.json` may carry
`QualificationProfile: PHYSICAL_QUALIFICATION_INTERACTIVE`. The one-client
lifecycle adapter then checks evidence access and normal-LAN control from the
logged-in non-admin users and starts no restricted worker socket broker. Its
fixed SSH forward/reverse and fresh-run gates remain. Both endpoint daemon
configs must independently select a temporary named interactive Codex profile;
the stage flag does not elevate or change a daemon. The default restricted path
remains available and its Foundation 2B qualification is a separate claim.
The September 28 one-client physical retry passed under the temporary dedicated
Codex profile file format. The
[receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
retains its exact run IDs, packet counts and cleanup evidence.
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
legacy barrier without transmitting physical commands. The separately selected
[four-client workflow](workflows/four-client-lifecycle.json) uses the same
barrier and fixed probe with `stage --clients 4`: one server expects four actual
clients, while the client endpoint launches four distinct nonces. Its local
result waits for all four clean closes and both captures must contain four
distinct bidirectional source-port tuples. Default staging remains one client.
The first four-client physical attempt reached all four application readiness
checks but failed the client and worker capture gates. After separate client
finalization, worker four-flow capture and classification proofs, the one fresh
retry passed both raw capture gates and returned
`FOUR_CLIENT_READINESS_ONLY` success from both endpoints and the physical
coordinator. Its outer lifecycle wrapper returned FAIL after a server agent
`NEEDS_USER/MISSING_CAPABILITY` status, despite a successful host result; see
the [receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md).
The one-client catalog's first full run
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
records that historical failure and cleanup. The later one-client PASS and
four-client capture stop are recorded in the current physical receipt.

POOLED_SERVICE, physical gates, KI-006, Foundation 3L status and 3M are unchanged.
