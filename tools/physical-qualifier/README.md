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
Codex sandbox could not create its staged evidence directory. The fixed-root
[`Provision-ClientEvidenceRoot.ps1`](Provision-ClientEvidenceRoot.ps1) repaired
only that task-owned ACL. The next attempt used
[`run_one_client_lifecycle.py`](run_one_client_lifecycle.py), which runs
[`evidence_preflight.py`](evidence_preflight.py) through the installed client
Codex sandbox before any wake. The restricted client passed; both agents
registered, but the worker's separate service-evidence root denied its sandbox
identity. No capture or probe ran. The failed physical and lifecycle IDs are
consumed; a new attempt requires a bounded worker-root repair and worker
preflight. See the [physical receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
and [protocol record](docs/PROTOCOL.md).
POOLED_SERVICE, physical gates, KI-006, Foundation 3L status and 3M are unchanged.
