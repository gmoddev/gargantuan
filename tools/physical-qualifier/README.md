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
legacy v1 readiness profile, including early-server-result handling. Current local
stage writes three config files; adapting authenticated assignment pull to the
physical project requires separate bounded qualification before deployment.
POOLED_SERVICE, physical gates, KI-006, Foundation 3L status and 3M are unchanged.
