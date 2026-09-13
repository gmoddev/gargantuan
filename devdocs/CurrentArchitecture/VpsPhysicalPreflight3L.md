---
status: measured-infrastructure-preflight
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-13
---

# Foundation 3L existing VPS infrastructure preflight

Date: 2026-09-13. Read-only infrastructure assessment; temporary test tools and
listeners removed. The original preflight made no repository, service-contract, firewall, tunnel,
installed-package, or persistent-service changes. This publication records evidence only. No Foundation 3M.

## Verdict

**C — EXISTING VPS INFRASTRUCTURE CANNOT FUND CURRENT PROFILE**, scoped to the
existing tested WireGuard path and current configuration. The observed service
does not conservatively fund either 2.147 Gbps application reservation or
4.295 Gbps aggregate backend capacity. Retain the physical-capacity stop.

This is not proof of a provider's contractual hard cap or of every possible
alternative path. Direct public iperf testing was unavailable without changing
protected firewall rules; raw untunneled capacity and provider guarantees remain
**not measured / unknown**. No engine defect is inferred from this preflight.

## Inventory and provider evidence

| Alias | OS | Assigned CPU | Memory | Available at initial probe |
| --- | --- | --- | --- | --- |
| BigKVM | Ubuntu 24.04.5 LTS, kernel 6.8.0-139-generic | 4 KVM vCPUs; exposed model AMD Ryzen 9 9950X3D | 16,720,662,528 B (15.57 GiB) | 15,569,948,672 B (14.50 GiB) |
| MiniVPS | Ubuntu 24.04.5 LTS, kernel 6.8.0-139-generic | 2 KVM vCPUs; exposed model Intel Xeon Gold 6132 | 4,055,949,312 B (3.78 GiB) | 3,496,165,376 B (3.26 GiB) |

The processor model string describes the exposed host model, not dedicated access
to all its physical cores. Both guests have active ens3 and an existing wg0
tunnel. BigKVM additionally has an active Pelican bridge and veth interfaces;
docker0 has no carrier. ens3 has MTU 1500, wg0 MTU 1420. ens3 reports speed -1,
meaning unavailable. Virtual bridge/veth reports of 10,000 Mbps are not uplink
capacity evidence. IP addresses are deliberately omitted from this report.

Both have fq_codel on ens3 and no configured tc class rate shown. This rules out
neither hypervisor nor provider shaping. Cloud-init reports unknown provider /
nocloud, without region or availability zone. The SSH configuration contains no
bandwidth or traffic-plan metadata. No provider rate, transfer quota, dedicated
CPU entitlement, or network SLA can be established from the inspected metadata.

BigKVM is already hosting panel/Wings/container workloads; MiniVPS runs Caddy
and the existing tunnel. Those workloads were preserved. Initial load averages
were near zero and neither had swap in use.

## Method and topology

Control: existing Windows SSH aliases and keys; no key copying or forwarding.
Data: MiniVPS initiates an iperf3 connection to BigKVM on **wg0, port 45231**.
Normal mode transfers MiniVPS to BigKVM; reverse mode transfers BigKVM to
MiniVPS on the established connection. The encrypted tunnel runs over the
existing public path; it is not a new private provider network.

iperf3 3.16 and its dependencies were downloaded using the existing Ubuntu apt
metadata and extracted under unique /tmp directories. Nothing was installed in
the package database and no daemon service was enabled. Each test listener was
single-use and bounded by a 22–30 second timeout; clients also had timeouts.
No tests ran concurrently. No firewall or tunnel configuration was changed.

Two initial attempts with MiniVPS listening transferred no data because the
existing policy does not permit the test port in that direction. One subsequent
listener encountered that still-active bounded listener. These errors are retained
in the raw receipts and excluded from throughput results. Reversing which host
listened used BigKVM's existing allow rule for the tunnel peer. Public INPUT
policy offers no unused permitted iperf port; existing services were not displaced.

TCP used four streams paced to 500 Mbps aggregate, then 2 Gbps aggregate, for
five seconds each. Confirmation runs used the same 2 Gbps ceiling for eight
measured seconds after two seconds of warm-up. UDP used one stream, 1200-byte
datagrams, three seconds per offered-rate step. Further UDP escalation stopped
in each direction after loss exceeded 1%; that was a test-safety stopping rule,
not a newly selected Gargantuan loss contract.

## TCP receiver throughput

Mbps and Gbps below use decimal units. Reported throughput is received iperf
application data, not wire bytes, GNS capacity, or an interface speed.

| Direction | 500 Mbps cap, 5 s | 2 Gbps cap, 5 s | Repeat: 2 Gbps cap, 8 s after warm-up |
| --- | --- | --- | --- |
| MiniVPS to BigKVM | 495.30 Mbps | 932.49 Mbps | **1,001.26 Mbps** |
| BigKVM to MiniVPS | 150.13 Mbps | 174.04 Mbps | **108.26 Mbps** |

TCP retransmissions reported in the three forward trials were 0, 0, and 32;
reverse trials reported 330, 707, and 64. Retransmission counts are not packet-
loss percentages. Short runs have different sender/receiver measurement windows,
especially across warm-up and drain; the roughly 1 Gbps result is not proof of
a precise 1 Gbps provider policer or a guaranteed sustainable minimum.

## UDP receiver results

These use the receiver's sum_received throughput, not the sender's offered-rate
summary. Loss and jitter are iperf's reported receiver statistics; short reverse
tests have different sender/receiver accounting windows at the end of the run.

| Direction | Offered | Received | Loss | Jitter |
| --- | --- | --- | --- | --- |
| MiniVPS to BigKVM | 100 Mbps | 98.65 Mbps | 0% | 0.05271 ms |
| MiniVPS to BigKVM | 250 Mbps | 246.02 Mbps | 0.2573% | 0.02050 ms |
| MiniVPS to BigKVM | 500 Mbps | 479.39 Mbps | **2.8211%** | 0.01198 ms |
| BigKVM to MiniVPS | 100 Mbps | 99.97 Mbps | 0.0221% | 0.02265 ms |
| BigKVM to MiniVPS | 250 Mbps | 249.71 Mbps | 0.1440% | 0.00916 ms |
| BigKVM to MiniVPS | 500 Mbps | 499.12 Mbps | 0.1869% | 0.00967 ms |
| BigKVM to MiniVPS | 1,000 Mbps | 889.55 Mbps | **10.8357%** | 0.00898 ms |

Low reported jitter does not cancel packet loss or establish a latency service
guarantee. No multi-gigabit UDP saturation was attempted after these failures.

## RTT, CPU and memory

Twelve pings per direction/path, 200-ms interval:

| Path/direction | RTT min / mean / max / mdev, ms | Loss |
| --- | --- | --- |
| Public, BigKVM to MiniVPS | 39.298 / 47.368 / 61.275 / 7.831 | 0/12 |
| Public, MiniVPS to BigKVM | 39.283 / 42.934 / 55.449 / 4.732 | 0/12 |
| WireGuard, BigKVM to MiniVPS | 39.475 / 41.794 / 45.580 / 2.370 | 0/12 |
| WireGuard, MiniVPS to BigKVM | 39.537 / 43.789 / 50.805 / 3.736 | 0/12 |

Post-test five-ping samples in each tunnel direction had no loss and mean RTT
39.646 / 39.990 ms. Ping mdev is RTT variability, not UDP jitter.

During TCP repeats, vmstat aggregate busy maxima were BigKVM 23% and MiniVPS
34%; maximum sampled steal was 3% and 0%, respectively. No swap-in/out was
observed. This does not establish CPU headroom for Gargantuan or rule out a
single-core/packet-processing bottleneck. iperf pacing itself used substantial
user CPU in some capped trials. Tunnel/kernel/provider attribution is unresolved.

BigKVM is the stronger candidate for one Server by CPU generation and available
memory. MiniVPS's two shared vCPUs / roughly 3.26 GiB initially available RAM do
not establish capacity for 32 actual GameSession processes. Per-client CPU/RSS,
server tick service, and gameplay under either placement are **not measured**.
Splitting clients across both guests does not prove the missing network capacity;
placing them with the Server would also substitute local traffic for remote
physical-path qualification. No 32-client placement is recommended as funded.

## Capacity comparison and contract realism assessment

The accepted N=32 profile requires R=8 MiB/s per connection, A=256 MiB/s, and
backend ceilings of 16 MiB/s per connection: **2.147483648 / 4.294967296 Gbps**.
The best measured TCP direction, roughly 1.001 Gbps, is only about **46.6% / 23.3%**
of those respective numbers, before reserving any operational headroom. The
preferred BigKVM-Server to MiniVPS-client direction was substantially slower.
UDP loss becomes substantial below the requested multi-gigabit envelope.

The existing physical-capacity stop therefore stands. This is a demonstrated
failure to fund the current profile on the tested path, not evidence that the
ordinary canonical workload itself needs 4.295 Gbps. Its reliable gameplay
offering is only 2,944 B/s ingress / 6,336 B/s egress, with conservatively promoted
Character output up to 814,080 B/s, excluding structural work and wire overhead.
Those small averages cannot replace the contract's per-peer reservations.

The large reservation follows the unchanged legal atomic-group and FIFO service
requirements: G=524,288 B; E=262,176 B; minimum peer backlog G+2E=1,048,640 B;
150 ms of queue allowance after the 100-ms nonqueue allowance. Even the minimum
compatible R=6,990,934 B/s requires 1.790 Gbps aggregate application capacity and
3.579 Gbps aggregate backend funding at 32 peers. Reducing R alone cannot keep
all those promises on the measured path. Roughly 40-ms RTT also consumes part
of the nonqueue allowance; changing it without evidence would hide a trade-off.

The next product/architecture assessment should compare, without implementation:

1. Retain the current full envelope and provision an independently funded path.
2. Select a smaller supported peer-count/profile, subject to fresh actual-client
   qualification; do not extrapolate a supported count from these short tests.
3. Explicitly reconsider which admitted legal structural/gameplay bursts carry
   the ordinary latency promise on commodity hosting, documenting any changed
   service scope or deadline while preserving protocol compatibility.

No option was adopted, and no new lane, buffer, rate, wire change, or service
contract was introduced. Exact next task: prepare an approval-ready deployment-
contract realism assessment anchored to this measured infrastructure, quantifying
the capacity/latency/supported-scale trade-offs before any contract modification.

## Receipts and cleanup

Raw JSON and vmstat receipts remain local under
`C:\Users\aiden\AppData\Local\Temp\3l-vps-preflight-20260913`, in its BigKVM
and MiniVPS subdirectories. They are local evidence only and may contain test addresses;
they were not committed or published. The report intentionally omits addresses.
Both remote task directories and extracted tools were removed after copying;
port 45231 had no remaining listeners. iperf3/libiperf0/libsctp1 remain uninstalled.
Existing firewall, WireGuard, panel, Wings, containers, Caddy and SSH were not
reconfigured or restarted. The inspected Gargantuan source was
`4966f223467b12fc396f47ccb7736679c5dcbbc0`; the infrastructure preflight
preserved the pre-existing morphology edits. No production build or CI rerun is needed
for this infrastructure-only inspection.

The subsequent [service-coverage assessment](../FutureArchitecture/Foundation3LServiceCoverageDecision.md)
recommends Option C as a proposal only. It does not revise the accepted contract
or turn these measurements into actual-client qualification.
