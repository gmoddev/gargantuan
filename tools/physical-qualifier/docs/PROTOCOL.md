---
status: current
owner: qualification-infrastructure
last_verified: 2026-09-29
---

# Physical qualifier control protocol v1

## Extracted infrastructure ownership

The generic control and capture implementation now belongs to
[GantriaEngine Agent Coordinator](https://github.com/GantriaEngine/agent-coordinator),
pinned by [upstream.lock.json](../upstream.lock.json) to commit
`9c81cfb16640dc18e29b6253fc0f5463c4c4dd4b`. Read its
[protocol](https://github.com/GantriaEngine/agent-coordinator/blob/9c81cfb16640dc18e29b6253fc0f5463c4c4dd4b/docs/PROTOCOL.md)
and [security model](https://github.com/GantriaEngine/agent-coordinator/blob/9c81cfb16640dc18e29b6253fc0f5463c4c4dd4b/docs/SECURITY.md).
This document retains Gargantuan's legacy readiness profile, fixed artifact,
capture/evidence policy and acceptance boundaries. Bootstrap the pinned library
before using this checkout; see [migration](../../../devdocs/CurrentArchitecture/PhysicalAgentCoordination.md).
The existing fixed-operation capture service is retained. Its worker-local hook
was updated after separate capture qualification; the service binary and probe
are unchanged.

> The protocol coordinates capabilities; it does not transmit authority.
>
> Natural-language agent communication does not directly invoke endpoint capabilities.

This is an explicitly accepted qualification-only process boundary. It does not
enter Engine, GameSession, GNS, or production networking. It replaces chat timing
for the qualified one-actual-client and four-client readiness smokes. Physical
readiness evidence is not final Foundation 3L acceptance or structural funding
evidence. KI-006 stays open; strengthened Phase 1 and the 32-client Local/Node
matrix have separate gates.
The readiness protocol and its installed probe remain unchanged. Strengthened
Phase 1 uses the [D01 service curve](../../../docs/adr/D01-pooled-service-curve.md);
the older short-window contract and its probe pin are historical. A new D01
binary must be staged and verified before a physical Phase 1 attempt.

## Ownership and transport

One coordinator binds an explicit normal-LAN IPv4 address. Each endpoint operator
starts a local helper which binds its configured LAN source address and registers
its role. The helper owns only its child probe, capture, and evidence directory.
The worker operator owns launching the server helper locally, including elevation
for the Windows trace hook. SSH is for deployment/integrity checking, never live timing.
No file transfer, network reconfiguration, firewall operation, or remote arbitrary
command is exposed. Configured process argv and capture hooks are local operator
authority, never received over the wire.

Use a fresh run UUID and 256-bit random token per attempt. Peer source IPv4,
role, exact artifact hash, endpoint, token, and run must match. Tokens are passed
in configuration files and redacted from evidence. TCP v1 is unencrypted and is
restricted to this trusted LAN; peer-IP checking is not cryptographic machine
identity. Do not expose the port outside the trusted LAN. Do not copy a token to
chat. Loopback is allowed for control-only tests. Qualification fiber addresses
and wildcard binding are rejected as control addresses.

## Representation and bounds

UTF-8 newline-delimited JSON, at most 16,384 bytes including newline per envelope:

```json
{"Version":1,"RunId":"UUID","Sequence":1,"TimestampUnixMs":0,"Token":"64 hex characters","Type":"STAGE_READY","Role":"CLIENT","ArtifactSHA256":"SHA256","Endpoint":"10.253.3.2:39450"}
```

Sequences start at 1 independently in each direction on each connection and must
advance exactly by 1. No reconnect/resume, duplicate execution, or cross-run reuse.
Wall-clock timestamps are evidence only; local monotonic deadlines enforce timeout.
Malformed, oversized, unauthenticated, out-of-order, or out-of-state messages abort.
Two peer sockets maximum; unexpected peer IPs abort. TCP_NODELAY is enabled.
Control and child text logs are capped at 16 MiB each. Stage/run timeouts are
configurable within 1–600 seconds; defaults are 300/60 seconds. Capture storage is
bounded independently (40-second dumpcap for four clients, 90 seconds for one;
64 MiB circular worker NDIS trace).
Local probe deadline is 25 seconds, server-live detection 6 seconds, hook execution
10 seconds, socket send 1 second. Capture/child failures abort, rather than retry.

## Barrier and states

1. Both helpers verify local source manifest and the exact existing binary before
   `STAGE_READY`. No captures or probes run while waiting for the other endpoint.
2. Coordinator sends `ARM_CAPTURE` to client. Client starts full-header capture
   and reports `CAPTURE_LIVE` only when its capture child is alive and pcap exists.
3. Coordinator sends `START_SERVER`. Worker starts its locally configured capture,
   then its readiness server for the selected client count. Its unbuffered GNS `event=listening` log,
   still-running PID, and PID-owned UDP `10.253.3.2:39450` socket must all agree.
4. Worker sends `SERVER_LIVE` with PID and endpoint. Coordinator immediately sends
   `START_CLIENT` in the same receive handler. Client launches one or four
   already-verified probe processes and reports `CLIENT_RUNNING` with the first
   PID. No chat or additional source/build scan.
5. After the client probe or four-process group exits, the endpoint stops its
   owned capture, validates the selected number of bidirectional peer/port tuples, and sends
   `CLIENT_DONE` with success, PID, exit code,
   local evidence path, and bounded GNS log detail. The server performs the same
   bidirectional validation after stopping its owned capture. The worker may finish
   and send `SERVER_DONE` before `CLIENT_DONE` reaches the coordinator. Since
   `FINALIZE` can already be in flight in that ordering, a worker that has sent
   `SERVER_DONE` accepts one delayed `FINALIZE` in `RESULT_READY` without repeating
   cleanup or changing its result. A live worker accepts `FINALIZE` only in
   `RUNNING`, then moves to `FINALIZING`; both helpers accept `RUN_DONE` only after
   their local result and cleanup are complete. Duplicate or out-of-state
   finalization still aborts. Both helpers remain connected until `RUN_DONE`.
6. Any timeout, disconnect, failure, or user interruption sends `ABORT`/`FAILED`
   where possible. Helpers stop only their owned children/hooks and hash evidence.
   A new attempt needs a new run/config/output directory, with no automatic retry.

Local success requires exit 0, probe readiness PASS and probe cleanup `good=1`.
In the pinned readiness probe, client `clean_remote_shutdown=1` means the client
reached `GameSessionStatus::Ready` and then observed the GameSession failure text
`Game server connection closed`. The server reports zero for that field by
construction; it is not a missing server-side client-close acknowledgement. The
server checks that the ready-peer count stays stable for one second, then closes
its session. Require the client's post-Ready remote-close condition and both
endpoints' cleanup; do not promote the server diagnostic into a new acceptance
gate. These are qualification-probe semantics, not production networking changes.

Overall readiness requires both endpoint results. Packet tuples and simultaneous
live-listener timing still require capture/log reconciliation; coordinator success
does not replace the canonical one-client gate or imply structural service PASS.

## Operations

Requires Python 3.12+ standard library and the hash-verified upstream snapshot.
Run `python tools/physical-qualifier/bootstrap.py` once for this checkout.
There is no pip runtime dependency or probe rebuild.
Use the installed skill's `scripts/qualifier.py`, or this repository copy:

```text
python qualifier.py stage --help
python qualifier.py coordinator <coordinator.json>
python qualifier.py endpoint <client.json>
python qualifier.py endpoint <server.json>
```

The `stage` command creates all three run configurations; transfer only server.json
to the worker over normal LAN before either helper registers. Start coordinator
first, then local endpoint helpers in hidden/background processes with their own
console logs. Each helper blocks on protocol messages. Registration of both helpers
authorizes one attempt within the previously authorized experiment; do not arm the
helpers just to inspect or deploy them. No GO/SERVER LIVE chat relay is needed.

The worker hook `worker/PktMonCapture.ps1 <EvidenceDir> Start|Stop` requires
local elevation, verifies the static Mellanox link and interface index 19, and
starts a Windows NDIS physical-interface trace scoped to that interface,
IPv4, UDP and the fixed client address. It does not filter by UDP port: the
pcap validator requires the exact two-peer UDP tuple and port 39450.
Capture is full-packet, circular and bounded to 64 MiB. Stop verifies the
task-owned trace path, then converts only complete NDIS packet events from the
fiber miniport into an Ethernet pcapng; an incomplete export is not published.
The worker idle preflight requires both Packet Monitor and Windows trace to be
stopped before lifecycle assignment.
An elevated worker-local test may select fixed synthetic port 39452 as a third
hook argument; the service always uses the default GNS port 39450.

The 2026-09-28 application-only diagnostic on UDP 39452 showed why the prior
synthetic gate `82b2a37d-f998-4062-92a9-f33a41eca9f8` did not qualify capture:
the worker Python listener had no matching inbound firewall allowance. The
earlier ETL contains one inbound Ethernet snapshot at aggregate component 75,
three IP-layer drop reports at component 89, and no flow at the named IPv4 L2
secondary component 81. Its component-81 pcap export was empty because that
component saw no flow; the drop reports are not three lost fiber packets. A
temporary allowance limited to the Python runtime, UDP 39452, the two fiber
addresses and `Ethernet 4` let the next application-only exchange deliver five
requests and five replies. That rule was removed after the test.

Topology run `941a40a5-bbe1-408f-9bae-3a32a07c50e4` used the same bounded
application exchange and selected the Mellanox binding stack. Miniport 13 and
aggregate TCP/IP 75 each observed exactly five Rx and five Tx packets, one copy
of each synthetic payload. IPv4 L2 secondary component 81 observed zero, while
the two edges of WFP Native Filter 30 observed each packet twice. Converting all
selected components yielded 40 frames for ten datagrams; component-13-only
conversion yielded exactly ten and zero Packet Monitor drop reports. Component
IDs can change, so the hook resolves the miniport ID at each start rather than
pinning `13`. The corrected hook passed the bounded direct candidate run
`965f65e9-da22-4fea-becf-da8356f0e60c`: all five application requests and
five replies were delivered, and the miniport-only export contained each
direction exactly five times with no unexplained duplicates or capture drops.
The worker service remains the same fixed-operation, 90-second-lease binary.
The earlier synthetic-qualified hook and service pin were
`73A840FCA570F676B06D76457FF719301BBB4C93F9865A25B19EE1671EE63E3E`.

The later live-GNS ingress investigation showed that the synthetic observation
filter did not generalize to GNS. In the preserved failed-run ETL, miniport 13
contained 62 outbound payloads and no inbound payloads; all 62 exported to
pcapng. A bounded live-GNS sweep with the same UDP/IP/port filter again saw no
inbound payload on miniport 13, WFP Native Filter 30, or TCPIP 75. Removing the
IP predicate, or selecting every Packet Monitor component, did not restore
ingress. Matching only the two fiber MACs and IPv4 exposed 150 inbound and 68
outbound miniport frames, with 150 inbound and 64 outbound on the GNS tuple.
Adding `-t UDP` to that MAC filter again suppressed every inbound payload;
matching the same MACs, IPv4 and port 39450 without `-t UDP` captured 127
inbound and 61 outbound GNS packets at miniport 13. The 188 miniport payloads
all exported to pcapng with zero reported drops. WFP Native Filter 30 records
two edge snapshots of each packet, so the hook keeps miniport-only export and
the existing exact-UDP pcap qualification gate. No NIC offload setting changed.
The hook and service pin for the preceding one-client proof were
`2BC2E1430A29ACDE61DCCD35B943998FDB45D8E81E3E2A206D66158F334634D9`.
The installed-hook capture-only proof `e60416771cd34df0` saw 135 inbound and
32 outbound exact-tuple GNS packets in both ETL and pcapng, with no lost events
or reported drops. The subsequent fresh one-client lifecycle stopped before
physical capture when both Codex agents failed at startup; it did not qualify
one-client readiness.
The 2026-09-28 four-flow attribution reproduced the earlier two-ingress-flow
pattern with Packet Monitor's port-only filter. Removing that predicate or
selecting other Packet Monitor components did not reliably show all received
packets, even though the worker application received 20 requests per flow.
A bounded Windows NDIS physical-interface trace recorded 20 ingress and 20
egress packets for each of the four fixed-fiber synthetic flows. The staged
hook exported valid pcapng with those same counts on both diagnostic port
39452 and readiness port 39450. After promotion, the installed fixed-operation
service repeated 20/20 per flow on port 39450. The installed hook and service
pin are `231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`;
the previous hook and config remain in the protected worker-local backup.

The fresh four-client physical run `acd22294-75d5-4457-b69e-73bb6a00d8af`
then recorded all four tuples in both directions: client 603 outbound/402
inbound, worker 402 outbound/603 inbound. The client dumpcap closed its
pcapng by its own 40-second duration stop with zero reported drops; the worker
service stopped its owned trace and exported 1,323 complete fiber frames. Both
raw evidence manifests verify. The physical coordinator and both endpoint
results succeeded with `FOUR_CLIENT_READINESS_ONLY`. The outer lifecycle host
also recorded both capability results as successful, but its final wrapper
status was FAIL because the server agent ended `NEEDS_USER/MISSING_CAPABILITY`
after the physical protocol completed; that status is not a packet or
GameSession failure. See the [physical receipt](../../../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md).

Stop uses a task-owned marker and requires the matching Windows trace path
before stopping it. Changed ownership aborts cleanup and preserves unrelated
state. Endpoint cleanup independently parses the pcapng and fails unless the
exact peer/port tuple appears in both directions. Do not force-kill helpers:
a kill or host crash cannot run finally cleanup; the worker must inspect its
recorded capture ownership marker before recovering a leftover trace.

The locally installed Foundation 2B lifecycle catalog is
`physical_qualifier_lifecycle.py`, with the ordered workflow in
`workflows/one-client-lifecycle.json`. The client starts its pinned legacy
coordinator and endpoint, the worker runs its locally approved endpoint, and
the client collects both results. Agent Coordinator carries only versioned
capability names and result metadata; the physical commands and run files stay
endpoint-local.

The physical lifecycle adapter owns one SSH process per fresh physical label.
The main host listens at `127.0.0.1:49964` through SSH `-L` to the worker's
`127.0.0.1:49963` lifecycle daemon; the main coordinator consumes this leg.
SSH `-R` listens on the worker's `127.0.0.1:49961` and forwards to the main
host's `127.0.0.1:49961` coordinator; the restricted worker endpoint consumes
this leg. Both listeners bind loopback only. The host controls SSH setup with
`ExitOnForwardFailure=yes`, one fixed destination per leg, and no peer-supplied
forwarding options. The worker daemon owns the forward target; the main
coordinator owns the reverse target. Before creating an assignment or waking
agents, the adapter requires both restricted evidence proofs, the fixed worker
socket broker proof, an authenticated `OFFLINE` presence response through the
forward, and a two-way nonce exchange through the reverse from the worker's
non-admin sandbox SID. A listener or SSH process alone is not readiness.
The temporary reverse challenge listener closes before the lifecycle host binds
the same port. Session identity, SSH PID, ports, owners and timestamps are
retained locally. Normal exit, preflight failure and lifecycle abort terminate
the owned SSH process and require both listeners to clear. An existing listener
or consumed label blocks a new session; the restricted agent cannot launch SSH
or redirect either leg.

The sole fresh lifecycle attempt prepared physical run
`58f15af7-46b7-462b-af78-df88a8db428d` and used Agent Coordinator run
`8267f8a4-7e78-4407-a2ad-9583fbfc46ac`. Both Codex agents woke, pulled
fresh assignments and registered. The first client capability failed before
the legacy coordinator could listen: its child reported `WinError 5` while
creating the client `coordinator-evidence` directory beneath
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-0e2f3637519c4425`.
The Codex endpoint sandbox could read the staged local config but lacked write
access to that evidence parent. The host aborted; neither probe nor capture
started. There was no GNS or GameSession observation and no packet count to
qualify. The agents and daemons stopped, the SSH tunnel was closed, Packet
Monitor had no session or filters, and the service remained running and idle.
This attempt is **FAIL** and must not be retried with its used identity.
The next task must stage client evidence in a directory writable by the
bounded Codex endpoint, verify that write access before wake, and then make
one entirely fresh lifecycle attempt. KI-006 remains OPEN and Foundation 3L B
remains PARTIALLY READY.

### Optional Windows capture service

On dockerbox, the existing installed service provides an opt-in wrapper for
the same stock hook. It runs as LocalSystem and exposes only `start`, `stop`, and
`status` over a local named pipe. The pipe DACL grants LocalSystem and the one
worker SID selected during installation. Requests are newline-delimited JSON
bounded to 16 KiB; the service accepts a canonical run UUID, an endpoint-helper
PID running the configured Python runtime, and an evidence directory beneath the
single configured local root. It accepts no executable, command, arbitrary
capture arguments, interface, or path outside that root.

The service verifies the installed hook SHA-256 before each invocation, permits
one active capture, persists its owned run before starting, and invokes the
existing exact-interface/ownership hook. It monitors the endpoint helper PID and
process start time; helper exit, service stop, or the 90-second hard deadline
stops only that run. Service restart attempts cleanup only for its protected
active-run record. An ownership mismatch blocks new captures and preserves
unrelated Windows trace state. Audit entries are written under the protected
ProgramData service directory. Installation is a one-time local Administrator
action. It does not change UAC, firewall, execution policy, NIC state, or Windows
security policy.

New source builds/installations use the pinned standalone helper with the
project-owned `worker/PktMonCapture.ps1` adapter. The new service/pipe is named
`GantriaAgentCoordinatorCapture` / `GantriaAgentCoordinatorCapture-v1`; its binary
is `AgentCoordinator.CaptureService.exe`. The old installed
`GargantuanPhysicalQualifierCapture` service is preserved as the current baseline.
Do not uninstall or replace it merely to adopt the extraction.

To opt in during staging, use `--server-capture-client` with the installed
`PhysicalQualifier.CaptureService.exe`. The qualifier passes its endpoint process
ID to the local service client to provide the bounded capture lease. The
coordinator protocol, probe, and evidence ownership remain unchanged. Keep the
direct PowerShell hook option for hosts that do not install this service.

PowerShell capture hooks are hashed at staging and checked again before execution.
The helper executes their verified local source as an inline `-EncodedCommand`,
with quoted local arguments and no execution-policy settings. This supports the
worker's existing script-file restriction while preserving its Windows policy.

Configuration files are per-run trusted local inputs, not production settings.
This version refuses non-readiness probe arguments and supports only the qualified
one-client default or a separately selected four-client readiness mode. In the
four-client mode, the same barrier launches four actual client probe processes
with unique consecutive nonces; the server expects four simultaneously Ready and
active GameSessions for the probe's canonical one-second interval. The client
result waits for all four processes, requires each canonical clean close, and
both endpoint captures require four distinct source-port tuples with traffic in
both directions. Both endpoint and coordinator results are classified
`FOUR_CLIENT_READINESS_ONLY`; the coordinator rejects a configured/result
classification mismatch. The four-client physical readiness evidence passed
on the single fresh attempt; the outer lifecycle wrapper status requires
separate reconciliation. This mode is not strengthened Phase 1 or
32-client funding evidence. Evidence remains local, with result.json,
control.jsonl, source manifest, probe logs, captures, and evidence-manifest.json.
The manifest lists every evidence file except itself to avoid a self-hash cycle.

Run control-only tests with:
`python -m unittest discover -s tools/physical-qualifier/tests -v`.
They use loopback and mock protocol participants, never the qualification probe.
