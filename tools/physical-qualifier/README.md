# Gargantuan physical qualification adapter

The development Phase 1 probe and result analyzer implement the
[D01/F1 finite-grant drain contract](../../docs/adr/D01-pooled-service-curve.md):
each accepted maximum-size grant must complete unique native first-send within
its finite rate-latency envelope, pass its independent intra-grant running-rate
check, and participate in a real four-peer first-send overlap. Four native
all-subinterval peer checks imply the 64 MiB/s running-pool bound on that
common interval. ACK, retirement, capture, and gameplay gates remain separate.
Earlier short-window and D01 cross-grant receipts are historical. The installed
physical probe and capture-service pins remain unchanged until the corrected
F1 candidate completes deterministic and hosted CI gates. The old staged D01
candidate remains pinned separately
from readiness: probe SHA-256
`2E543D0983D66895569A0E270905086200C6E478A8C313B206E6BC64C0081ABC`,
native-source archive SHA-256
`FC0D0B5E11D488E091CF4552A3CFC7E9362F1DA4DFA434AB139120A15BFDAA5A`,
base `a998cf98b6a1dad40d414c59e0f4a6d348749e52`, and pinned GNS
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. Both candidate bundles
passed manifest/hash and socket-free self-test preflight. The worker-local
four-process loopback stress case failed D01's charged service-deficit bound;
it did not exercise the physical path.
The D01 manifest and probe hashes are deployment history, not F1 qualification.
The corrected F1 candidate pins native source revision
`ce4733de8d68086667bc8fe5138bcc743f21c54e`, pinned GNS
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`,
F1 native-source archive SHA-256
`18AF89DAEEF3D5DD8E1AC6ED7089EDBEF278990AA211CBB4E54BCF3555D846FB`,
and Windows probe SHA-256
`925DC0684787B1D629901D5047FF9ECC968AFC5F4E579DE576F701322E9BB99F`.
The [F1 manifest](phase1-f1-source-manifest.json) also pins all twelve runtime
assets and the two native DLLs required beside the probe executable; the Phase 1
adapter checks their hashes and rejects the earlier D01
candidate. [Generate the archive](make_f1_source_archive.py) with
`python tools/physical-qualifier/make_f1_source_archive.py <output.zip>` from
the pinned native source. Use `--verify-pinned <archive.zip>` to compare the
archive with the historical source revision; `--verify-tree <archive.zip>
<source-root>` checks a separate build tree. The candidate bundle is staged
separately from installed tools; endpoint artifact, capture, evidence-root and
normal-LAN preflights remain required before an authorized physical attempt.

The corrected four-client capture hook uses a task-owned, non-circular NDIS
trace capped at 256 MiB. Stop rejects a trace at or above 240 MiB or with any
reported lost ETW events, then exports only complete frames from the pinned
fiber miniport. Large traces use an in-process bounded pcapng exporter so the
privileged service can finish within its 30-second Stop limit. A 72,000,000-byte
synthetic fiber payload produced a 94,896,128-byte ETL with zero lost events;
the final hook replay exported 62,300 complete frames in 10.390 seconds. This
is capture-infrastructure preflight, not a physical F1 result. The installed
service hook must be updated and hash-pinned before physical qualification.
The fixed-operation service embeds its pinned hook in a PowerShell
`-EncodedCommand`, so the hook keeps the fast exporter as a compressed embedded
source blob. The readable [C# source](worker/FastNdisExport.cs) is checked
byte-for-byte against that blob, and a regression bounds the full Windows
command line for a 512-character evidence path. No auxiliary runtime file is
needed beside the service-pinned hook.

Generic transport, coordinator, endpoint and privileged capture implementation is
owned by [GantriaEngine Agent Coordinator](https://github.com/GantriaEngine/agent-coordinator).
This directory retains Gargantuan's exact probe/source identities, fixed GNS argv,
physical capture hook, staging, local execution adapter and evidence acceptance.
F1 server readiness verifies that the still-running probe PID owns the exact
UDP `10.253.3.2:39450` socket. The pinned F1 binary has no
`event=listening` log marker, so log text is not a readiness predicate.

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
workflow. Its server result requires the F1 four-grant finite-drain verdict and
metrics. It launches exactly one gameplay producer (the first
client nonce) and three non-producers; the endpoint rejects a staged Phase 1
client config with a different producer flag. Readiness staging keeps all four
producer flags at zero and remains `FOUR_CLIENT_READINESS_ONLY`.
Abort cleanup also waits for the timed client capture to close its final pcapng
block. Worker capture stop waits up to 45 seconds for the service's bounded
completed-export response before direction validation; failure to acknowledge
stays a cleanup error. The generic helper source bounds its hook at 30
seconds. On 2026-09-28 the worker's installed service was updated to that
bound and qualified with a 26.656-second capture export producing a complete
32,233,120-byte pcapng. The installed executable SHA-256 is
`463934849064F7DD51FEAB73B8471FD9DBB6CAF3D9C95EBB38A8D65B2EFCA664`;
the capture hook, service config, authorized SID, evidence root, and Python
lease were unchanged. The current 32-wave qualification workload has **not**
passed its isolated four-peer preflight, so no fresh physical Phase 1 attempt
was launched with it. The [physical receipt](../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
records the blocking observations.
The adapter's candidate provenance is base
`14644a369f9e7bfb9a81c21354adae62902d63d7`, overlay
`2ED31AE67E0F99619940BBB130CD451DB37FEF3A5CEEDAB475E682C3FBEE7003`,
and probe SHA-256
`F130DC868A807FFA4EF10887079162C562230854AE17C013559452791993E969`.
The single diagnostic physical attempt used this probe after both endpoint
preflights and then restored the prior worker probe pin. It found no sustained
four-grant backlog window or terminal scheduler rejection. Its 30 MiB worker
ETL needed 19.262 seconds to export, exposing the installed helper's
15-second stop limit; post-run recovery does not change the failed gate.
This command writes configs only. The artifact pin, both evidence roots, worker
capture service and hook, fiber/LAN state, lifecycle profiles and endpoint
leases still require independent preflight before a physical attempt.
The lifecycle runner performs its own forward/reverse tunnel proof before
assignment. That worker proof is one-use per stage label: do not call the
tunnel preflight separately with the same label and then invoke the runner.

[upstream.lock.json](upstream.lock.json) pins commit
`67b730e6dbf7a0524d4f29d0d9cb19a0abf8a614` and SHA-256 of the consumed sources.
The pinned coordinator and endpoint now share the same listener, source-bind,
connect, and authenticated `STAGE_READY` path in production and the control-only
preflight. The latter records the actual child process and token, completes the
control protocol, and returns `CONTROL_PREFLIGHT_ONLY` without starting Packet
Monitor, capture hooks, GNS, or a probe. Stage it with `--clients 4 --phase1
--control-preflight` and the separate
[`four-client-control-preflight-lifecycle.json`](workflows/four-client-control-preflight-lifecycle.json)
workflow. Its dedicated lifecycle capabilities cannot accept the physical
workflow, and a control-only result is not physical F1 evidence. Repeat it with
fresh identities through the same installed lifecycle child launcher before a
physical run; the interactive host socket check remains supplemental.
The deployed client coordinator runtime must have an applicable inbound TCP
allowance for the normal-LAN control listener. Windows may create an automatic
Block rule for a new versioned Python executable even when the same binary is
allowed at another path. The qualified deployment uses the existing installed
Python 3.12.6 image (SHA-256
`737A7E3B71E3578F8432ACC7DD88C452E593622C544BC13DA4789D69C63DA5AE`)
for the child process and keeps the versioned, hash-verified qualifier source.
The stage helper rejects a Block rule or missing Allow rule for that image.

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
new package only, including the same project-owned capture hook at both
`tool/worker/PktMonCapture.ps1` and `skill/scripts/PktMonCapture.ps1`. Keep old
installed bundles/services as rollback. Follow the
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

At the time of that ordering fix, POOLED_SERVICE, physical gates, KI-006,
Foundation 3L status and 3M were unchanged. D01 later superseded the
short-window acceptance rules; F1 now supersedes D01's cross-grant service
deficit for future qualification while those receipts remain historical.

The Farm32 outer controller is `farm_outer_campaign.py`. Its `prepare` operation
creates one-use ticket identity, generates a deployment-hash-checked scale
manifest on the worker from both packaged roles, retains the exact same
manifest bytes for the client, performs fresh role-local inventory (including
the 8 GiB client free-memory floor), and seals `farm_ticket_staging.py`'s fixed
copy plan. `stage` copies only those planned files plus the pinned coordinator
runtime into private endpoint roots, verifies every copied member locally at
each endpoint, and rechecks the worker Python/helper hash pins. The worker owns
the normal-LAN coordinator listener on `192.168.0.108:39451`, while the client
connects outbound; a nonce-bound control-only socket probe and the same-run
`LISTENING_UNQUALIFIED` marker precede the two role launches. The fixed runner
and endpoint helper use bounded hidden processes. `collect` copies every worker
index member with a fresh SHA-256 check, binds the coordinator and both role
results, and invokes the existing offline capture reconciler. For Node, it
mirrors the bounded Node receipts/logs at their original one-run absolute path
so the receipt's immutable path and hash pins remain verifiable. These
operations record execution and sealed evidence only. Capture directions, real
Node TLS, workload
behavior, and all Foundation 3L acceptance gates remain independently checked
by the offline reconciliation and acceptance tools; no outer stage result is a
physical or provider PASS. This path has source/mock coverage but requires an
official workflow-dispatch package, installed side-by-side Farm32 service,
endpoint runtime staging, and actual control-only qualification before use.

The fixed baseline/recovery frame ceilings cover their existing 300/420-second
role-local runtime budgets at the Player's 16,667-us network cadence, including
its first unslept frame: 18,001/25,201 client frames and the existing extra 1,000
server ticks. These are process-lifetime ceilings, not workload duration or
service allowances. Recovery convergence and ordinary-service deadlines remain
unchanged; supervisors still enforce their original wall-clock budgets.

Recovery collection preserves each `recovery-gameplay.tsv`,
`recovery-structural.tsv`, and `recovery-mixed.tsv` server member up to its
canonical 32 MiB limit and verifies its sealed hash. Offline recovery analysis
runs from the repository `tests` directory: when staging those analysis scripts
separately, keep `RecoveryCausalEvidence.ps1` beside
`PhysicalGameSessionFarm.ps1`, `PhysicalGameSessionFarmReconcile.ps1`, and
`PhysicalGameSessionFarmAcceptance.ps1`, together with their existing helpers.
Keep `PhysicalFarmClockEvidence.ps1` beside the reconciliation/acceptance scripts;
it independently joins the hash-indexed 32-client native calibration logs using
`farm_clock_exchange.py`. Its 640 probe intervals do not synchronize whole phases
or qualify one-way latency, and missing historical clock evidence remains unmeasured.
The role-local endpoint supervisor does not import these analysis scripts.
