---
status: stopped-phase1-incomplete-four-grant-proof
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-24
---

# Foundation 3L pooled physical qualification attempt

## Bounded real-GNS Phase 1 stop (2026-09-24)

**STOP / INCOMPLETE FOUR-GRANT PROOF.** The [accepted funding review](PhysicalFundingGateReview3L.md)
permitted actual GameSession/GNS testing without a zero-loss synthetic UDP or
further NDIS prerequisite. The unchanged `POOLED_SERVICE` profile was exercised
with four actual GameSession clients on the dedicated static fiber. The
evidence-complete attempt connected all four clients and applied eight repeated
512-KiB structural waves, but the predeclared analyzer found qualified
simultaneous four-grant overlap in only waves **1, 4 and 6**: respectively
16,607, 16,920 and 18,433 microseconds. The other five waves had no qualifying
common interval (wave 7 qualified only two individual peers). The executable
exited 1. There was **no observed below-floor interval** and no demonstrated
transport-budget failure; the short burst workload did not supply enough
qualified interval evidence to declare Phase 1 healthy. Do not promote three
successful windows to sustained 64-MiB/s service or Phase 1 PASS.

### Source and physical configuration

The base is published `4d27238553fdc27b9f6772b438c49393eac3a8eb` plus a
five-file uncommitted qualification-only source overlay, not a clean-HEAD
binary. Base archive SHA-256 is `252f67d5e76b18f026c1239e1f959e37e973a728dae4f9c2c5bd235fe8d53a18`;
overlay archive SHA-256 is `9228e18366227c0636a92f309179ab1749e5523f068776219650ccfb3e3790e4`.
The overlay adds the manual probe, private bounded diagnostics and CMake target;
the accepted profile values and production admission behavior are unchanged.
Worker build: MSVC 19.51.36260.0 x64 Release, Ninja, CMake 3.31.6-msvc6,
four compile jobs, GNS enabled, precompiled headers disabled to avoid a worker
path-map/PCH compiler error. Declared pinned GNS revision is
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`; the 278-file prepared GNS
tree has SHA-256 manifest `d0e3de240f1a2d008f6e8458379745e62ae22f3cfb7559a1e10b2f1db514efb6`.
Both endpoints ran the same newly built probe executable, SHA-256
`dee724ebda36f869ec66c8807f6108ad7d86f582551549a15d3ea0637d9f88ea`.
The copied client package includes exact DLL/runtime hashes in the archive.
Both worker and local no-socket analyzer self-tests passed before traffic.

The directly connected Mellanox ports were **already** preferred static
`10.253.3.1/30` (workstation Ethernet 3, if50) and `10.253.3.2/30` (worker
Ethernet 4, if19) when this task began. Both negotiated 10 GbE with MTU 1500,
driver 2.53.23539.0; the on-link `/30` routes select the fiber, and each side's
neighbor cache identifies the opposite Mellanox MAC. A scoped single-datagram
reachability check arrived on the worker from `10.253.3.1`. Phase 1 bound the
server to `10.253.3.2:39450` and connected clients to that address. A temporary
worker Public-profile UDP rule was limited to local `10.253.3.2:39450` and
remote `10.253.3.1`; it was removed after each attempt. No Windows security
policy was disabled. No address, route, NIC binding, driver or profile setting
was changed in this task. Static addressing remains a **candidate deployment
requirement**, retained because it was pre-existing; persistence was not tested
since qualification did not pass.

### Measurements and stop reason

The first launch reached its client-ready timeout before demand, then cleanup
cleared all state. A scoped UDP check verified reachability. A prompt-launch
repeat applied all eight waves but the PowerShell wrapper truncated its server
output on nonzero exit. The same bounded workload was repeated once with
separate stdout/stderr and complete CSV preservation; results below are from
that evidence-complete attempt. The earlier failures remain in the archive.

| Property | Completed Phase 1 measurement |
| --- | ---: |
| Actual GameSession clients / applied structural waves | 4 / 8 each |
| Maximum simultaneous drain grants / debt | 4 / 2,097,152 B |
| Structural retirement per wave | 2,097,152 B |
| Accepted / exact attributed retired / terminal released | 16,802,660 / 16,802,660 / 0 B |
| Unique stream first sent / ACKed, sampled cumulative deltas | 16,833,199 / 16,833,199 B |
| Reliable stream retransmitted delta | 0 B |
| Maximum pending / sent-unacked per peer | 361,701 / 303,715 B |
| Maximum GNS queue time | 19.252 ms |
| Configured/effective per-grant GNS send rate | 18,874,368 B/s |
| Maximum sampled GNS wire outbound per peer | 337,388 B/s |
| Worker NIC sent / received during wrapper | 18,677,150 / 564,060 B |
| Worker NIC receive errors / discards delta | 0 / 0 |
| Server process peak RSS / maximum host CPU sample | 27,099,136 B / 16% |
| Largest client process RSS / maximum local host CPU sample | 22,233,088 B / 48% |

Each of the four peers ended with first-sent and ACKed deltas equal, and all
eight 2-MiB aggregate waves retired exactly. The feedback trace has 3,086 rows,
zero invalid, overflow or below-floor classifications, and an ACK-positive
qualified interval for all four peers in each of the three overlapping waves.
The peak sampled RTT is 1 ms. Pending/unacked bytes and debt drained to zero at
cleanup. Retransmission is physical cost, not logical drain; its observed zero
does not convert the short offered workload into a 64-MiB/s physical proof.
No path-loss consequence was demonstrated by this attempt.

The gameplay producer recorded 130 RPCs and 129 Event ACKs. RPC p95/p99/max
were 53.369/76.537/77.006 ms; Event ACK max 77.008 ms, within the existing
150/250/500-ms RPC and 250-ms Event targets during the measured work. Every
client reported eight applied waves and movement; the server terminated the
session after its incomplete-overlap verdict, so all clients exited nonzero.
Action result, due-to-observation Character/root latency and per-core CPU are
**not measured** in this Phase 1 harness. Fixed 20-second service recovery and
workload-derived complete structural convergence were not independently
qualified. The client generator process count peaked at four; worker process
count peaked at one. Host CPU/memory and NIC samples are in the archive; link
rate alone was not used as headroom proof.

All four client processes exited. The worker rule, UDP listener and probe process
are gone. Session cleanup reports empty connections/journal requirements,
zero outstanding debt/grants, and exact accepted=retired+terminal conservation;
client cleanup also reports `good=1`. A full canonical cache/resident lifecycle
gate remains **not measured** because Phase 2 did not run. The pre-existing
static address plan remains preferred; no diagnostic NIC tuning is retained.

**Phase 2 Local: not run. Phase 2 Node: not run. KI-006: OPEN. Foundation 3L:
B — PARTIALLY READY.** No final acceptance sweep is eligible and no 3M work
began. The next permitted physical task is to make the bounded actual-client
probe produce sustained/overlapping qualified backlog windows for all four
grants, with complete host measurements, without changing the accepted profile.
After a true Phase 1 PASS, build and run the exact 32-actual-client canonical
Local and Node matrix. A synthetic zero-loss rerun is not the next gate.

Local untracked evidence archive: `build/physical-gns-evidence-20260924.zip`,
102 files plus manifest,
6,017,209 B, SHA-256
`c567c7d8d5d23e4d36c072862b4b409f5937f20561469dd933e89690318911f1`.
Every entry was decompressed and SHA-256 checked. The same archive is preserved
on the worker under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-gns-physical-20260924` with
a matching download hash. It contains source/base and overlay snapshots,
compiler script, exact binaries, raw per-peer GNS feedback/host CSV and JSON,
failed-attempt logs, analyzer, and cleanup receipts.

## Current funding-gate clarification (2026-09-23)

The accepted [funding review](PhysicalFundingGateReview3L.md) supersedes the
historical prerequisite below: **raw capacity is established; proceed to bounded
production-GNS qualification with static dedicated-link addressing**. Synthetic
zero-loss UDP and additional Windows/vendor attribution are not prerequisites.
All pooled service, latency, freshness, fairness/debt and physical actual-client
gates remain unchanged. Nonzero retransmission is evaluated as physical cost,
never counted as unique drain and not an automatic failure.

KI-006 remains OPEN; no actual-client physical PASS or Foundation closure is
implied. Earlier diagnostic measurements and failures below remain historical.

## Historical installed direct fiber update (2026-09-23)

The approved [receive-handoff retry](FiberPhysicalPreflight3L.md) completed
expanded NBL/NDIS/TCPIP/WFP tracing. A 900.018899-Mbps generated /
899.799008-Mbps received trial loses 167 packets after the last observed
filter edge and before TCP/IP. Native drops and metadata do not identify an
owner or correction. Two clean short controls are insufficient repeatability.
The instrumentation stop applies: funding remains unqualified, KI-006 OPEN,
and native indication/return ownership plus queue telemetry is the next task.

The [new 10 GbE fiber preflight](FiberPhysicalPreflight3L.md) supersedes this
receipt's deferred-cable status. The operator confirms a direct Mellanox-to-
Mellanox cable. Explicitly bound TCP delivers 9.246–9.471 Gbit/s. Earlier
UDP failures and clean samples remain historical evidence. WFP now identifies
WSH Default Inbound Block filter 147332 on a local/raw IPv4 receive/accept path.
Matched worker DHCP/static/DHCP profiling gives 140.840/900.001/145.683 Mbit/s;
NETIO sampled share changes 64.646%/2.745%/66.549% and FindCacheMatch
44.356%/0%/46.308%. AFD lifecycle tracing directly identifies DHCP's raw UDP
endpoint. No WSH policy change is needed for the static diagnostic condition.
Elevated receiver traces independently locate 2,244 missing sequences after
the last filter upper edge and before TCP/IP capture; TCP/IP's sequence set
exactly equals application delivery. The receiver's largest gap is blocked
waiting, with only 6 microseconds runnable before scheduling. Npcap unbinding
does not eliminate loss and is reverted. The hidden handoff queue/drop reason
remains unobserved, so the requested stop applies. **Physical funding remains
unqualified** at the unchanged
805.306368-Mbit/s envelope; the final repeated funding matrix was not run.
The larger send buffer remains provisional, not a deployment requirement.
DHCP/address state is restored, NIC settings are unchanged, and task rules,
traces and processes are absent. The receipt records the pre-existing Public
worker profile's inbound management limitation. Next: identify receive-handoff
queue/indication ownership and drop reason, establish a supported correction,
then stable bidirectional funding with an accepted persistent address plan.
No actual clients, Local/Node application
runs or final acceptance sweep occurred. KI-006 remains OPEN and
Foundation 3L B — PARTIALLY READY; no merge or 3M. The September 15 LAN evidence
below remains historical within its original scope.

## Historical verdict and source (2026-09-15)

**INCONCLUSIVE / NOT PHYSICALLY QUALIFIED.** Independent TCP and UDP evidence is
now available in both directions. Clean trials exceed the accepted envelope,
but reverse UDP loss, pacing variation and a ctsTraffic playback-window failure
remain unattributed on the shared Windows hosts. The conservative preflight
gate is **not passed**. This is a qualification-tool / host-timing attribution
stop, not proof of a hard 1-GbE capacity ceiling or an Engine defect.

The operator corrected the topology: **no direct cable is installed**. The
planned **25 Gb fiber cable is deferred until delivery in a few days**. Current
LAN measurements do not qualify that future path, its NICs or negotiated speed.
Intermediate network hardware models remain unidentified.

No actual Gargantuan server or clients were launched; Local and Node physical
qualification are **not measured**. KI-006 remains **OPEN**, Foundation 3L
**B — PARTIALLY READY / NOT READY FOR FINAL ACCEPTANCE**, and 3M **BLOCKED / NOT
STARTED**. No merge was performed.

Production source remains `eec0c7123a39698761e49ed276ee37aae673e023`.
This documentation-only resumption starts from published evidence
`669949b0dac722dd896e9dcf36ebaeb677c6bd96` on
`foundation/3l-content-availability`, primary repository `gmoddev/gargantuan`.
An isolated detached worktree preserves the user's dirty checkout. No production,
test, profile, rates, limits, wire, ordering or recovery semantics changed.
No active Foundation 3L checkpoint exists at this source.

The [morning attempt receipt](https://github.com/gmoddev/gargantuan/blob/669949b0dac722dd896e9dcf36ebaeb677c6bd96/devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
retains its original measurements and local administrator-token blocker.
Client-initiated ctsTraffic pull connections allowed independent reverse testing
without adding a local inbound firewall rule. Local administrator access was
not acquired and was not necessary for that method.

## Profile, topology and placement

The accepted [Option C profile](PooledReliableServiceProof3L.md#selected-32-peer-candidate)
requires **96 MiB/s = 100,663,296 B/s = 805.306368 Mbit/s** of usable envelope.
Fixed commitments remain 84 MiB/s: structural 64, gameplay 8,
control/realtime 4 and transport reserve 8; another 12 MiB/s is inside the
envelope. Four concurrent grants and the 16-MiB/s active-grant peer drain floor
are unchanged. The extra 104-MiB/s UDP probes test physical headroom only;
they do not change Gargantuan's profile or create a new acceptance threshold.

| Resource | Workstation / intended client generator | Dockerbox / intended server |
| --- | --- | --- |
| Host | DESKTOP-B8V8NAN | HOSTPC |
| CPU | Ryzen 9 7950X3D, 16 cores / 32 logical | Ryzen 9 5900X, 12 cores / 24 logical |
| OS | Windows 11 Pro, build 26200 | Windows 11 Pro, build 26200 |
| OS-visible RAM bytes | 33,157,488,640 | 34,281,426,944 |
| Free RAM KiB at inventory | 9,748,956 | 17,111,844 |
| Inventory UTC, 2026-09-15 | 20:56:29 | 21:19:12 |
| NIC | Realtek Gaming 2.5GbE Family Controller | Realtek PCIe GbE Family Controller |
| Negotiated speed | 2.5 Gbit/s | 1 Gbit/s |
| Driver version / date | 10.54.1111.2021 / 2021-11-11 | 10.79.50.1003 / 2025-10-03 |
| IPv4 / interface | 192.168.0.68/24 / Ethernet | 192.168.0.108/24 / Ethernet 2 |
| MTU | 1500 | 1500 |

The path traverses the existing LAN, not a direct cable. Exact intermediate
switch/router and cable models are **not established**. Both routes are on-link
`192.168.0.0/24`; workstation traffic is explicitly bound to `192.168.0.68`.
Its active Wi-Fi and VPN are not the selected peer route. Worker Wi-Fi is
disconnected; WSL virtual interfaces are outside this path. No available
high-speed physical NIC was established. No driver, NIC, MTU or route was tuned.

Hosts are shared, not reserved. Existing worker containers `drycreek-bot` and
`directus-db` remained running. Benchmark placement was workstation sender /
worker receiver for forward traffic, worker sender / workstation receiver for
reverse traffic. Trials were sequential, without overlapping evidence transfers.
TCP used four connections in one tool process per host. Forward UDP used one
process per host; reverse UDP used one worker server and either one or four
workstation receivers. Four receiver wrappers also sampled host CPU, so
measurement overhead is part of this environment and is not isolated.

## Independent TCP capacity

[Microsoft ctsTraffic](https://github.com/microsoft/ctsTraffic) 2.0.3.9 x64 was
obtained from its official release directory. Both executable copies match
SHA-256 `0548089E59C872306CE2C98E7163E2A717119756010CF64D3CB3DA2854F632CF`;
Authenticode reports **NotSigned**. This is distinct from signed NTTTCP below.

Each trial transfers 536,870,912 application bytes per connection over four
connections, 2,147,483,648 bytes total, with `-Verify:data`.
Forward uses `-Pattern:push`; reverse uses `-Pattern:pull` on independently
established client connections. Common settings are TCP port 55301,
`-Connections:4 -Iterations:1` on the client and `-ServerExitLimit:4` on the
worker. All six trials record four successes, zero network/protocol errors,
exit zero and no wrapper timeout.

Received throughput below divides verified application bytes by the interval
from the receiver's first connection-established timestamp to its last
connection-success timestamp. It excludes worker listener startup wait and
tool control bytes. Timestamps have millisecond resolution. CPU is the mean
of sampled host aggregate CPU percentages over each wrapper's lifetime,
including startup; it is not dedicated Engine CPU.

| Trial | Received Mbit/s | Workstation / worker mean CPU % |
| --- | ---: | --- |
| tcp-forward-1 | 944.520 | 16.43 / 7.75 |
| tcp-forward-2 | 939.047 | 14.86 / 8.53 |
| tcp-forward-3 | 947.019 | 16.86 / 8.56 |
| tcp-reverse-1 | 941.982 | 11.60 / 8.09 |
| tcp-reverse-2 | 941.569 | 13.00 / 8.31 |
| tcp-reverse-3 | 941.466 | 20.43 / 7.94 |

TCP alone funds the throughput comparison in every trial. Across these samples,
maximum observed benchmark RSS is 13,455,360 bytes workstation and 12,353,536
bytes worker. Maximum sampled core utilization is 93% and 82%, respectively.
These are tool measurements, not a 32-client resource envelope.

## UDP packet accounting, throughput and jitter

Forward uses Microsoft-signed [NTTTCP 5.40](https://github.com/microsoft/ntttcp/releases/tag/v5.40),
verified SHA-256
`F66561D09AF91305412FD60CA4B28D57C7B650035D3C1EDCC00A57B079E2247E`.
One sender/receiver connection uses 1,400-byte datagrams, 15 seconds measurement,
two seconds warmup, one second cooldown, base port 55201 and sequence/QPC
logging with `-jm`. Sender `-thr 98304` or `106496` is KiB/s for the single
thread; NTTTCP pacing truncates to integer bytes/ms. Loss is counted from
unique sequence numbers in the observed interior after warmup, not inferred
from mismatched sender/receiver byte totals. Packets beyond the first/last
observed sequence are excluded; these are exact **bounded-interval** loss counts.

Reverse uses ctsTraffic UDP at nominal 96 or 104 MiB/s, 15 seconds configured
stream length, `-DatagramByteSize:1400`, default one-second playback buffer,
port 55201, and per-receiver jitter CSV. Aggregate frame rate is 72,000 or
78,000/s, divided equally across four flows when applicable. Integer frame
sizing produces **1,398 bytes per frame, one UDP datagram per frame**, including
the tool header. Expected sequence counts are 1,080,000 or 1,170,000.
Zero-error frame runs allow missing-sequence packet accounting.

For both tools, received payload rate uses observed receive timestamps, not
nominal duration. Four-flow rates use the common earliest-to-latest receiver
timestamp span, not the sum of separate per-flow rates. This includes flow
startup skew. Sender pacing sometimes exceeded the configured 15-second span.

| Direction / trial | Nominal MiB/s | Expected / received unique packets | Lost / loss % | Received payload Mbit/s |
| --- | ---: | --- | --- | ---: |
| Forward udp-forward-seq-1 | 96 | 1,078,564 / 1,078,429 | 135 / 0.012517% | 805.220 |
| Forward udp-forward-headroom-1 | 104 | 1,168,287 / 1,168,017 | 270 / 0.023111% | 872.311 |
| Reverse udp-reverse-2, one flow | 96 | 1,080,000 / 1,074,989 | 5,011 / 0.463981% | 778.113 |
| Reverse udp-reverse-group-1, four flows | 96 | 1,080,000 / 1,069,075 | 10,925 / 1.011574% | 795.020 |
| Reverse udp-reverse-headroom-1, four flows | 104 | exact unique receive count not recoverable | exact packet loss not measured | not attributable as packet goodput |
| Reverse udp-reverse-headroom-2, four flows | 104 | 1,170,000 / 1,170,000 | 0 / 0% | 859.196 |
| Reverse udp-reverse-headroom-3, four flows | 104 | 1,170,000 / 1,170,000 | 0 / 0% | 852.324 |

The first headroom trial received 1,617,364,374 bytes, equivalent to 1,156,913
datagram-sized receives, but recorded only 1,060,648 completed frames,
109,352 missing completed frames and 96,265 error frames. Error frames include
received packets outside the playback sequence window. **109,352 is not a
network packet-loss count.** The byte-counter deficit of 13,087 is not an exact
unique-loss count either, because those error frames lack complete identity
accounting. Completed-frame goodput was 743.835 Mbit/s; that metric cannot
stand in for raw datagram capacity. The retained analyzer returns null packet
loss for error-frame trials rather than silently treating missed playback as loss.

Jitter below is absolute change in relative transit time,
`abs(delta receive time - delta send time)`, and its RFC-3550-style 1/16 EWMA.
It needs no cross-host clock offset synchronization. NTTTCP CSV is in receive
order; ctsTraffic CSV is completed-sequence order, so the latter is explicitly
a **sequence-adjacent** statistic, not a proven arrival-order jitter measure.
Grouped values are the largest per-flow p99 / EWMA maximum, not pooled percentiles.

| Trial | Transit variation p99 ms | Maximum EWMA jitter ms |
| --- | ---: | ---: |
| udp-forward-seq-1 | 0.224 | 1.303 |
| udp-forward-headroom-1 | 0.226 | 1.221 |
| udp-reverse-2 | 0.548 | 1.799 |
| udp-reverse-group-1 | 0.599 | 2.356 |
| udp-reverse-headroom-1, completed frames only | 0.631 | 3.255 |
| udp-reverse-headroom-2 | 0.611 | 0.864 |
| udp-reverse-headroom-3 | 0.613 | 0.974 |

All retained measurable runs have zero logged duplicate frames/packets;
NTTTCP observed no reordered arrivals in the analyzed intervals. ctsTraffic
sequence ordering cannot independently establish absence of packet reordering.
An initial reverse UDP attempt on 55301 failed with Windows bind error 10013:
that port lies in an excluded UDP range. It is a setup failure, excluded from
capacity/loss evidence; the subsequent trials used available port 55201.

During the anomalous first reverse headroom trial, the worker had a sampled
core at 100%, while mean host CPU was 14.21%; clean repeats had maximum cores
47% / 56% and host means 10.23% / 9.29%. This is correlation, not attribution
to a process or proof of a server bottleneck. Workstation wrapper samples showed
mean host CPU 17.17–17.67% during that anomalous trial. Per-receiver RSS stayed
at or below 15,396,864 bytes across the four-flow probes; worker tool RSS was
at or below 11,591,680 bytes. Short sampling can miss scheduling stalls.
No driver trace, packet capture or isolated-host reproduction establishes
the cause. Clean repeats are retained alongside, not substituted for, failures.
Forward NTTTCP reports workstation / worker host CPU 16.461% / 8.445% at
nominal 96 MiB/s and 14.972% / 9.320% at 104 MiB/s. Forward UDP process RSS
was not sampled. None of these measurements qualifies application resource use.

The evidence therefore does not establish a conservative usable UDP minimum
under the canonical deployment contract. No arbitrary acceptable loss percentage
has been added. Neither a permanent physical capacity failure nor a qualified
1-GbE profile follows from these mixed results.

## Baseline ICMP and MTU

Fresh samples at 21:19 UTC use 100 sequential 1,200-byte echoes per direction,
500-ms timeout and 50-ms spacing. All succeeded. Percentiles are nearest rank;
integer-millisecond zero means below the API's resolution.

| Direction | Loss | Mean RTT ms | p50 / p95 / p99 / max ms | Mean / max absolute successive RTT delta ms |
| --- | --- | ---: | --- | --- |
| Workstation to worker | 0 / 100 | 0.11 | 0 / 0 / 1 / 10 | 0.222 / 10 |
| Worker to workstation | 0 / 100 | 0.04 | 0 / 0 / 1 / 2 | 0.081 / 2 |

DF probes at 1,472 payload bytes succeeded and 1,473 returned `PacketTooBig`
both ways, consistent with MTU 1500. These were baseline samples after throughput
testing. ICMP under saturation and one-way absolute latency are **not measured**.

## Actual-client and provider gate

| Required physical evidence | Local | Node |
| --- | --- | --- |
| Actual GameSession clients / server launched | 0 / 0 | 0 / 0 |
| Connection establishment and health of 32 clients | not measured | not measured |
| Server tick/service, CPU/RSS/network | not measured | not measured |
| Character/root cadence, latency and publication gaps | not measured | not measured |
| RPC RTT, handler/response queue, timeouts/errors | not measured | not measured |
| Event ACK/service and action latency/rejections | not measured | not measured |
| Structural load/evict/reload and convergence | not measured | not measured |
| Fixed 20-second service recovery | not measured | not measured |
| Exact retirement/debt, grants/journal, fairness/backlog | not measured | not measured |
| Client CPU/RSS, callback intervals and missed observations | not measured | not measured |
| Logical lifecycle/debt/readers/content cleanup | not measured | not measured |

The [canonical workload](RecipientServiceWorkload3L.md) remains eight moving/root
Characters with eight recipients each, **one** qualified gameplay producer,
and the shared 512-object / 273,032-byte provider unit through
baseline/load/resident/evict/reload and accepted overload/recovery cases.
Launching 32 independent producers would incorrectly multiply offered demand.

The existing [GameSession benchmark](../../tests/GameSessionBenchmark.cpp)
uses an actual content client at peer index zero and protocol observers for
other peers on SimulatedNetwork. [DueServiceFixture](../../tests/DueServiceFixture.hpp)
likewise has one actual gameplay client. They are not a ready physical client
farm. The task authorizes the smallest missing actual-client harness **after
preflight passes**; that condition has not been established, so no harness
implementation or application run was started.

The [recovery contract](PooledReliableServiceRecoveryContract3L.md) keeps
fixed 20-second service recovery separate from retained-work-derived structural
convergence. Unaffected Engine/loopback results retain their original scope;
the stricter 200-peer tick diagnostic remains separate performance debt.

## Cleanup, artifacts and validation

Four temporary inbound worker rules allowed only the relevant executable,
local `192.168.0.108` and remote `192.168.0.68`, on all profiles:

| Rule name | Executable | Protocol / configured ports |
| --- | --- | --- |
| Codex-Gargantuan-PhysicalResume-20260915-TCP | ctsTraffic 2.0.3.9 | TCP 55301 |
| Codex-Gargantuan-PhysicalResume-20260915-UDP | ctsTraffic 2.0.3.9 | UDP 55201, after excluded-port setup correction |
| Codex-Gargantuan-PhysicalResume-Ntttcp-20260915-TCP | NTTTCP 5.40 | TCP 55201,56201 |
| Codex-Gargantuan-PhysicalResume-Ntttcp-20260915-UDP | NTTTCP 5.40 | UDP 55201 |

Worker cleanup at **2026-09-15T21:20:09Z** removed those four rules and verified
none remained, with no ctsTraffic/NTTTCP processes. Client cleanup at
**21:20:10Z** also found no task rules or benchmark processes. No local rule was
added; the optional local receiver setup script was prepared but not executed.
Both protected worker containers remained up eight days. No global firewall
disable, interactive UAC prompt, persistent elevation, driver update or unrelated
service change occurred. Tool files and logs are retained intentionally.

The resumption archive `pooled-physical-resume-20260915.zip` contains **329**
manifest-verified files: inventories, sequence/timing CSV, paired tool receipts,
analysis scripts/results, topology clarification and cleanup. SHA-256:

`43573204bfb3a2986efe396d64948deb98bc673a21ddb3f5d144efd47808c1a0`

- Local evidence: `C:\Users\aiden\AppData\Local\Temp\gargantuan-3l-physical-20260915\build\physical-resume\`.
- Retained worker archive: `C:\Sandbox\Codex\Logs\gargantuan-3l-physical-resume-20260915\`.
- Executables: local `build/physical-resume/Tools/ctsTraffic.exe` and
  `build/physical/Tools/ntttcp.exe`; worker
  `C:\Sandbox\Codex\Tools\ctsTraffic-2.0.3.9\ctsTraffic.exe` and
  `C:\Sandbox\Codex\Tools\ntttcp-5.40\ntttcp.exe`.

The archive excludes third-party executable/source directories. It preserves
both successful and invalid/anomalous trials. Local Temp is not permanent
artifact hosting. The earlier morning archive and receipt remain separate.

Existing terminal-success [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/34900780304)
and [GNS CI](https://github.com/gmoddev/gargantuan/actions/runs/34900779755) at exact
production source `eec0c7123` are reused. No native suites were rerun for this
documentation-only resumption. Validation passes 53 relative file targets,
six TCP and seven UDP receipt rows, both cleanup receipts, all 329 archived
files against their SHA-256 manifest, matching worker archive hash, and
`git diff --check`. These checks do not imply application qualification.

## Exact next task

When the planned fiber path is physically installed, identify both NICs,
drivers, negotiated speed, MTU, selected route and any intermediate hardware.
Do not assume a 25-Gb cable alone establishes a 25-Gbit/s path. Repeat the
bounded independent TCP and sequence-accounted UDP preflight, resolving pacing
and playback-window attribution before declaring a usable envelope. If work
resumes on the current LAN instead, those unresolved reverse UDP results remain
a prerequisite; a clean subset alone is not the retained verdict.

If preflight passes, continue in the same task with the smallest canonical
32-actual-GameSession-client harness and unchanged Local and Node workloads,
including every service, resource and cleanup metric above. Do not reduce the
profile to obtain a pass.

**Supported physical profile: none established. KI-006 OPEN. Foundation 3L
B — PARTIALLY READY; not ready for final acceptance. No merge; no 3M.**
