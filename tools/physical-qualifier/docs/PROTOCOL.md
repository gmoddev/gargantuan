---
status: current
owner: qualification-infrastructure
last_verified: 2026-09-27
---

# Physical qualifier control protocol v1

## Extracted infrastructure ownership

The generic control and capture implementation now belongs to
[GantriaEngine Agent Coordinator](https://github.com/GantriaEngine/agent-coordinator),
pinned by [upstream.lock.json](../upstream.lock.json) to commit
`24edb592ace678124731455b841e2623f64ba57a`. Read its
[protocol](https://github.com/GantriaEngine/agent-coordinator/blob/24edb592ace678124731455b841e2623f64ba57a/docs/PROTOCOL.md)
and [security model](https://github.com/GantriaEngine/agent-coordinator/blob/24edb592ace678124731455b841e2623f64ba57a/docs/SECURITY.md).
This document retains Gargantuan's legacy readiness profile, fixed artifact,
capture/evidence policy and acceptance boundaries. Bootstrap the pinned library
before using this checkout; see [migration](../../../devdocs/CurrentArchitecture/PhysicalAgentCoordination.md).
Existing installed copies and the old capture service remain operational until
the new package is separately qualified; no installed tool is replaced here.

> The protocol coordinates capabilities; it does not transmit authority.
>
> Natural-language agent communication does not directly invoke endpoint capabilities.

This is an explicitly accepted qualification-only process boundary. It does not
enter Engine, GameSession, GNS, or production networking. It replaces chat timing
for the existing one-actual-client readiness smoke; it is not a Foundation 3L
acceptance gate or evidence of structural funding. KI-006 stays open. Four-client
readiness, strengthened Phase 1, and the 32-client Local/Node matrix remain separate
tasks. The accepted POOLED_SERVICE contract and probe binary are unchanged.

## Ownership and transport

One coordinator binds an explicit normal-LAN IPv4 address. Each endpoint operator
starts a local helper which binds its configured LAN source address and registers
its role. The helper owns only its child probe, capture, and evidence directory.
The worker operator owns launching the server helper locally, including elevation
for Packet Monitor. SSH is for deployment/integrity checking, never live timing.
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
bounded independently (90-second dumpcap; 64 MiB circular worker Packet Monitor).
Local probe deadline is 25 seconds, server-live detection 6 seconds, hook execution
10 seconds, socket send 1 second. Capture/child failures abort, rather than retry.

## Barrier and states

1. Both helpers verify local source manifest and the exact existing binary before
   `STAGE_READY`. No captures or probes run while waiting for the other endpoint.
2. Coordinator sends `ARM_CAPTURE` to client. Client starts full-header capture
   and reports `CAPTURE_LIVE` only when its capture child is alive and pcap exists.
3. Coordinator sends `START_SERVER`. Worker starts its locally configured capture,
   then its one-client readiness server. Its unbuffered GNS `event=listening` log,
   still-running PID, and PID-owned UDP `10.253.3.2:39450` socket must all agree.
4. Worker sends `SERVER_LIVE` with PID and endpoint. Coordinator immediately sends
   `START_CLIENT` in the same receive handler. Client launches its already-verified
   probe and reports `CLIENT_RUNNING`. No chat or additional source/build scan.
5. Client exits, stops its owned capture, validates that the exact peer/port tuple
   appears in both directions, and sends `CLIENT_DONE` with success, PID, exit code,
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

The stock worker hook `worker/PktMonCapture.ps1 <EvidenceDir> Start|Stop` requires
local elevation, verifies the Mellanox static link, and discovers both its current
Miniport and TCP/IPv4 L2 component by the Mellanox ifIndex. Packet Monitor captures
at those two layers to include both NIC transmit and receive visibility; its UDP
filter still requires both fiber peer addresses and port 39450. The capture is
full-packet, circular, and bounded to 64 MiB.
Stop uses a task-owned marker and requires matching ETL session/filter inventory
before stopping or clearing the sole owned filter. Changed ownership aborts cleanup
and preserves unrelated state for the local worker operator. Conversion packet and
drop totals are recorded. Endpoint cleanup independently parses the resulting pcapng and fails unless the
exact peer/port tuple appears in both directions. The privileged service invokes
this fixed hook unchanged; it does not add or remove packet directions. No security
policy is changed. Do not force-kill helpers: a kill or host crash cannot run finally
cleanup; the worker must inspect the recorded capture ownership marker locally
before recovering a leftover Packet Monitor session.

### Optional Windows capture service

On dockerbox, the existing installed service provides an opt-in wrapper for
the same stock hook. It runs as LocalSystem and exposes only `start`, `stop`, and
`status` over a local named pipe. The pipe DACL grants LocalSystem and the one
worker SID selected during installation. Requests are newline-delimited JSON
bounded to 16 KiB; the service accepts a canonical run UUID, an endpoint-helper
PID running the configured Python runtime, and an evidence directory beneath the
single configured local root. It accepts no executable, command, arbitrary
Packet Monitor arguments, interface, or path outside that root.

The service verifies the installed hook SHA-256 before each invocation, permits
one active capture, persists its owned run before starting, and invokes the
existing exact-interface/ownership hook. It monitors the endpoint helper PID and
process start time; helper exit, service stop, or the 90-second hard deadline
stops only that run. Service restart attempts cleanup only for its protected
active-run record. An ownership mismatch blocks new captures and preserves
unrelated Packet Monitor state. Audit entries are written under the protected
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
This version intentionally refuses non-readiness probe arguments and more than one
client. Extending to four or 32 clients requires a separate bounded harness change
after the relevant prerequisite passes. Evidence remains local, with result.json,
control.jsonl, source manifest, probe logs, captures, and evidence-manifest.json.
The manifest lists every evidence file except itself to avoid a self-hash cycle.

Run control-only tests with:
`python -m unittest discover -s tools/physical-qualifier/tests -v`.
They use loopback and mock protocol participants, never the qualification probe.
