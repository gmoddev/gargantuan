---
status: blocked-physical-capacity-preflight
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-13
---

# Foundation 3L physical deployment preflight

## Decision and source

**STOP: the inspected worker cannot fund the intended 32-client network
profile. No production deployment class is physically qualified by this task.**
Branch `foundation/3l-content-availability`, starting source
`ca176c81609eea83dbef634e37342700a3811114`. This is a documentation-only
preflight, not an actual-client benchmark failure or an engine defect.

The task explicitly requires stopping when available hardware cannot fund even
the intended 32-client profile. No 32-client processes were launched, no harness
or production source was changed, and no simulator evidence was rerun. Existing
[qualified simulator evidence](ContentClientScaleQualification3L.md#recipient-service-qualification-2026-09-13)
and prior official single-client / real-GNS evidence retain their original scope.
KI-006 stays **OPEN** for the intended 32-actual-client gate, not for hypothetical
200/500-client promises. Foundation 3L remains **B — PARTIALLY READY**; no 3M.

## Inspected hardware

Read-only SSH probe of the configured `dockerbox` worker, reporting hostname
`HOSTPC`, captured at `2026-09-13T22:55:39.5140409Z`:

| Resource | Observed inventory; not benchmark throughput |
| --- | --- |
| CPU | AMD Ryzen 9 5900X, 12 physical cores / 24 logical processors |
| OS | Windows 11 Pro |
| OS-visible memory | 33,477,956 KiB, approximately 31.93 GiB |
| Free memory at probe | 17,718,068 KiB, approximately 16.90 GiB; shared-host snapshot, not a reservation |
| Active physical adapter | Realtek PCIe GbE Family Controller, Ethernet 2, negotiated **1 Gbps** |
| Other physical adapter | RZ616 Wi-Fi 6E, disconnected |
| Matching native test/build processes | None at the probe; this does not imply the host has no other workloads |
| Environment | Shared development worker on LAN; loopback bypasses the physical NIC |

The probe used `Get-CimInstance Win32_Processor`, `Win32_OperatingSystem`,
`Get-NetAdapter -Physical`, and a bounded process-name inventory through SSH.
Receipt: `C:\Users\aiden\AppData\Local\Temp\3l-physical-preflight-20260913.json`,
SHA-256 `5F925B031397F23337C0C56079CC5B6DE632372BC8077BAD690CB9BE81DD9614`.
Negotiated link speed is a physical upper bound, not measured usable throughput.
Switch, router, upstream, loss, RTT, wire overhead and client-farm capacity are
**not measured**. No other funded deployment host/path was identified in this
task. No hardware, network settings or unrelated services were modified.

## Capacity arithmetic before execution

The [deployment contract](NetworkingReliableDeploymentContract.md) and
[workload contract](ReliableGameplayWorkloadContract3L.md) require actual funding
for every connected peer, including idle peers. Current workload settings are
R=8,388,608 B/s, backend=16,777,216 B/s per connection, N=32,
A=268,435,456 B/s, structural share 750 permille, gameplay reserve 25%.

| Quantity | Required / available |
| --- | --- |
| N times R | 268,435,456 B/s = 256 MiB/s = **2.147483648 Gbps** |
| A | 268,435,456 B/s; N times R equals A |
| Aggregate backend ceilings | 536,870,912 B/s = 512 MiB/s = **4.294967296 Gbps** |
| Structural refill | 6,291,456 B/s per peer; 201,326,592 B/s aggregate |
| Gameplay reservation | 2,097,152 B/s per peer; 67,108,864 B/s aggregate |
| Inspected link, ideal upper bound | 125,000,000 B/s = 119.21 MiB/s = **1 Gbps** |
| Application reservation shortfall | 143,435,456 B/s before any packet overhead |

The 1 Gbps path cannot fund application reservations alone. Configuring GNS
ceilings above that link does not create capacity. Small observed traffic does
not permit replacing the declared reservation with an average arrival rate.

This conclusion survives rate tuning within the existing 100-ms nonqueue
allowance. Source verification of
[`ReliableServiceProfile`](../../include/gargantuan/network/ReliableServiceProfile.hpp),
its [validator](../../src/network/ReliableServiceProfile.cpp), and
[`ServerHost`](../../src/host/server/ServerHost.cpp) establishes:

- G=524,288 B, E=262,176 B, minimum Qp=G+2E=1,048,640 B.
- Qg=G+2NE=17,303,552 B at N=32; no smaller legal-group exception.
- The 250-ms target minus 100-ms nonqueue allowance leaves 150 ms.
- Minimum integer R is ceil(Qp times 1,000 / 150) = **6,990,934 B/s**.
- N times that minimum R is **1.789679104 Gbps** of application capacity.
  Required backend headroom doubles it to **3.579358208 Gbps**.

Reducing E to the ordinary 20-KiB workload burst would violate the current
validator's legal-frame admission headroom. Changing queue allowances, legal
groups or the selected workload to make a 1 Gbps deployment pass is not an
authorized correction for this physical-capacity stop.

## Canonical offered traffic remains unchanged

All figures below are pre-run bounds from the
[recipient workload](RecipientServiceWorkload3L.md), not measurements on this
physical host. Complete application-message figures include the 32-byte adapter
envelope exactly once; they exclude unknown UDP/IP, encryption, ACK and
retransmission overhead. Backend ceilings are configuration, not captured wire
bytes or exact NIC occupancy.

| Traffic | Bound / interpretation |
| --- | --- |
| Reliable gameplay ingress | 25.5 messages/s, 2,944 B/s |
| Reliable gameplay egress | 41.5 messages/s, 6,336 B/s, including forced Character recipients |
| Ordinary Character/root output | Eight moving root-motion Characters times eight recipients times 20 Hz = 1,280 deliveries/s; conservative 212 B/delivery = 271,360 B/s |
| Continuous semantic promotion | Up to 3,840 deliveries/s, 814,080 B/s; 25,440 B/s to a recipient of two Characters |
| Owner input | Conservative 480 commands/s, 44,160 B/s aggregate; retain accepted input timing rather than multiplying Remote traffic by client count |
| Event / RPC / action | At most 15/s, 10/s and 0.5/s respectively; one ordinary producer, one outstanding RPC |
| Combined qualified reliable burst | 99 messages / 20,864 B global; nine messages / 1,496 B per peer |
| Content acquisition | One shared 512-object / 273,032-B source unit; Local disk or Node provider IO, not 32 provider copies |
| Structural network demand | Load, resident, eviction, reload via normal GRPL. Provider payload size is **not** serialized GRPL size. Exact physical-run bytes/s are **not measured**; admission is bounded by the structural refill and 524,288-B burst above |

Retain 65,536 planning work, exact 8,192 3J selection, acceptance-only Known,
complete-group reservation, finite Qp/Qg and unchanged service targets. Do not
count the ordinary and continuously promoted state bounds as simultaneous
independent streams. The backend headroom must cover realtime and packet work;
the table does not claim that application and backend reservations are additive.

## Candidate for the next hardware gate — not accepted or funded

A conventional dedicated-server class is a plausible **candidate**, not a
purchase recommendation or a newly supported product profile:

| Field | Candidate assumption, pending provisioning and qualification |
| --- | --- |
| Intended maximum | 32 simultaneous clients; no 64/100/200/500 promise |
| CPU | One server on a dedicated 12-core x86-64 host of the inspected 5900X class; CPU adequacy must be measured |
| Memory | At least 32 GiB reserved for that host, with separate capacity for the client load generators; adequacy must be measured |
| Network | Provisioned end-to-end 10 Gbps LAN/datacenter-class path and independent client load generators that can exercise it |
| R / A / backend | 8 MiB/s / 256 MiB/s / 16 MiB/s per connection |
| Shares | 75% structural, at least 25% gameplay |
| Classification | Proposed qualification hardware; **not production-qualified**, not currently funded on the inspected worker |

The aggregate backend ceilings consume 42.95% of a nominal 10 Gbps line rate;
57.05% is arithmetic slack, **not measured headroom**. Ten gigabit is a common
provisionable class, not a demonstrated minimum or proof of guaranteed throughput.
[Hetzner documents a dedicated-server 10G uplink option](https://docs.hetzner.com/robot/dedicated-server/network/10g-uplink/),
including a replacement NIC/uplink and separate outgoing traffic allowance.
This establishes hosting availability only; no server was rented, no tariff
accepted, and no external host was benchmarked. Provider port specifications do
not establish end-to-end reserved service or affordability for this product.

Once a funded host/path is identified, confirm capacity and isolation before
implementing the multi-process extension: one actual Server and 32 actual
GameSession client processes using real GNS and ordinary application paths.
Run Local and Node sequentially with the accepted fanout and total offered mix;
correlate recipient observations with full identities and a justified cross-
process clock mapping. Server send counters are not client observation.
Capture per-peer fairness, resource use, structural identity/convergence and
shutdown ownership. Only passing measured headroom can authorize a scale
extension within a separately funded profile.

## Evidence disposition and validation

| Class | Result |
| --- | --- |
| SIMULATOR QUALIFIED | Existing Local/Node 32 and 200 protocol-peer matrix, unchanged source and original scope |
| PHYSICAL QUALIFIED | **None added**; intended 32-actual-client profile fails preflight, not an executed workload |
| Retained actual-path evidence | Prior official single-client Local/Node and real 32-peer GNS cases, within their documented workload scope |
| NOT MEASURED | New actual-client Local/Node workload; due-service, gameplay, tick, fairness, convergence, server/client CPU/RSS, physical traffic, queue residence, headroom and post-run cleanup |
| Capacity finding | NIC/link reservation shortfall; CPU and memory suitability remain untested |
| Product disposition | No new supported client count; KI-006 OPEN for intended 32-client gate; final acceptance sweep is premature |

Documentation-only validation passed: arithmetic against the unchanged validator,
62 relative file-link targets with none missing, and `git diff --check`. The
isolated Astro build passed 19 pages with Pagefind and sitemap on Node 24.19.0;
all 30 tracked site input files matched HEAD, excluding unrelated morphology
edits. These engineering Markdown documents are checked as repository documents,
not claimed as newly generated site pages. Existing focused
MSVC, Linux sanitizer and simulator receipts are reused, not relabeled as a new
physical run. No native test source changed. CI status is reported at publication;
an in-progress workflow is not a pass. No persistent test processes were started.
