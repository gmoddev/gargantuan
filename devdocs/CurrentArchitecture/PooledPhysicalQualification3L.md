---
status: incomplete-physical-preflight
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-15
---

# Foundation 3L pooled physical qualification attempt

## Verdict and source

**INCOMPLETE / NOT PHYSICALLY QUALIFIED.** Forward TCP capacity and UDP delivery
were measured on the available Ethernet LAN. Independent reverse capacity,
exact UDP loss and full bidirectional funding remain **not measured**. Local
Windows denied creation of the scoped benchmark receiver firewall rule with
`Access is denied`; the available process token was not an administrator token.
User authorization was already present. This was an OS setup limitation, not
an automatic approval-review rejection or an attributed Engine defect.

No 32-client Gargantuan workload was started. Neither Local nor Node is
physically qualified. These results do **not** establish that the available
1-GbE path is incapable of funding Option C. The older
[full-reservation shortfall](PhysicalDeploymentPreflight3L.md) is historical
and cannot reject the pooled profile.

Production source: `eec0c7123a39698761e49ed276ee37aae673e023` on
`foundation/3l-content-availability`, primary repository `gmoddev/gargantuan`.
The published branch matched this source before the attempt and before receipt
preparation. Work used an isolated detached worktree; the user's dirty checkout
was preserved. This publication changes documentation only: no production,
test, profile, wire, ordering, rate, journal or recovery semantics changed.
No active Foundation 3L checkpoint file was present at this source; the
physical receipt, validation ledger and KI-006 are updated together.

## Profile and hardware

The accepted [Option C proof](PooledReliableServiceProof3L.md#selected-32-peer-candidate)
requires a usable envelope of **100,663,296 B/s = 96 MiB/s = 805.306368 Mbit/s**.
Fixed modeled commitments are 84 MiB/s: structural 64, gameplay 8,
control/realtime 4 and transport reserve 8; another 12 MiB/s remains inside
the envelope. All bounds, four concurrent grants and the 16-MiB/s active-grant
peer drain floor remain unchanged. A throughput sample is not proof of those
application guarantees or reserved physical headroom.

| Resource | Intended client generator | Intended server / trusted dockerbox |
| --- | --- | --- |
| Host | DESKTOP-B8V8NAN | HOSTPC |
| CPU | Ryzen 9 7950X3D, 16 cores / 32 logical | Ryzen 9 5900X, 12 cores / 24 logical |
| OS | Windows 11 Pro, build 26200 | Windows 11 Pro, build 26200 |
| OS-visible RAM | 33,157,488,640 bytes | 34,281,426,944 bytes |
| Free memory at inventory | 8,741,300 KiB | 17,192,488 KiB |
| Inventory UTC | 2026-09-15 10:58:08 | 2026-09-15 10:59:18 |
| Physical NIC | Realtek Gaming 2.5GbE Family Controller | Realtek PCIe GbE Family Controller |
| Negotiated speed | 2.5 Gbit/s | 1 Gbit/s |
| Driver version / date | 10.54.1111.2021 / 2021-11-11 | 10.79.50.1003 / 2025-10-03 |
| IPv4 / interface | 192.168.0.68/24 / Ethernet | 192.168.0.108/24 / Ethernet 2 |
| MTU | 1500 | 1500 |

Both machines were shared, not reserved. The worker's existing `drycreek-bot`
and `directus-db` containers remained running. Free memory is a snapshot, not
a supported client count or a reservation. No 10/25-GbE adapter was visible in
the physical-adapter inventory; the disabled/unidentified PCI inspection did
not identify an available high-speed NIC. No NIC, route, driver or MTU was tuned.

Observed topology is client Ethernet to worker Ethernet on the on-link
`192.168.0.0/24` LAN. `Find-NetRoute` selected Ethernet and source
`192.168.0.68`, not the client's active Wi-Fi or VPN. The benchmark sender was
explicitly bound to that address. Intermediate switch and cable models and
direct-cable status are **not established**; do not call this a direct link.
Worker Wi-Fi was disconnected. WSL virtual interfaces were outside this path.

## Baseline latency and MTU

Each direction used 100 sequential ICMP echoes, 1,200-byte payload, a 500-ms
timeout and 50-ms spacing. All 100 succeeded in each direction: observed
baseline ICMP loss was **0/100** in each direction, not a general loss guarantee.
Percentiles use nearest rank. `.NET Ping` reports integer milliseconds; a zero
sample means below its millisecond resolution, not zero physical latency.

| ICMP direction | Mean RTT ms | p50 / p95 / p99 / max ms | Mean / max absolute successive RTT delta ms |
| --- | ---: | --- | --- |
| Client to worker | 0.18 | 0 / 0 / 2 / 15 | 0.343 / 15 |
| Worker to client | 0.28 | 0 / 2 / 6 / 11 | 0.566 / 11 |

The last column is an explicitly defined RTT-variation statistic, not one-way
packet jitter. DF probes succeeded at 1,472 payload bytes and returned
`PacketTooBig` at 1,473 in both directions, consistent with the 1,500-byte MTU.
One-way jitter, latency under the Gargantuan workload and shaping/asymmetry
across independently saturated directions are **not measured**.

## Throughput method and results

Used the Microsoft-signed [NTTTCP 5.40 release](https://github.com/microsoft/ntttcp/releases/tag/v5.40).
Authenticode was valid for Microsoft Corporation; both executable copies had
SHA-256 `F66561D09AF91305412FD60CA4B28D57C7B650035D3C1EDCC00A57B079E2247E`.
Tool locations were the isolated worktree's `build/physical/Tools/ntttcp.exe`
and worker `C:\Sandbox\Codex\Tools\ntttcp-5.40\ntttcp.exe`.

Each valid trial used four synchronous connections/threads, 15 seconds of
measurement, two seconds warmup and one second cooldown. Three successful TCP
trials and three UDP envelope trials provide bounded repeatability; one UDP
trial each sampled the 64-MiB/s structural and 84-MiB/s fixed-commitment rates.
No specific physical repeat count was found in the canonical plan. These are
short capacity preflight samples, not endurance or application qualification.

Common arguments, with paired XML output and a hidden bounded process wrapper:

```text
receiver: -r -m 4,*,192.168.0.108 -t 15 -wu 2 -cd 1 -p 55201 -to 10000 -nsb -xml <receipt>
sender:   -s -m 4,*,192.168.0.108 -t 15 -wu 2 -cd 1 -p 55201 -to 10000 -nsb -xml <receipt> -nic 192.168.0.68
UDP:      add -u -l 1400 to both; sender adds -thr 16384, 21504 or 24576
```

`-thr` is KiB/s per thread. NTTTCP converts it to integer bytes/ms, so the
nominal 96-MiB/s setting is paced at 100,660,000 B/s before timer effects.
TCP data used 65,536-byte application buffers; UDP used 1,400-byte payloads.
Data ports were 55201–55204, with TCP synchronization ports 56201–56204.
The SSH receiver session stayed open until its child finished; wrapper timeout
was 35 seconds. Trials below all recorded child exit zero and no wrapper timeout.

| Trial label | Sender Mbit/s | Worker delivered Mbit/s | Sender / worker reported CPU % |
| --- | ---: | ---: | --- |
| tcp-to-server-4 | 945.954 | 945.864 | 22.225 / 7.686 |
| tcp-to-server-5 | 946.270 | 945.808 | 23.912 / 7.996 |
| tcp-to-server-6 | 944.942 | 944.961 | 24.063 / 8.136 |
| udp-64-to-server-1 | 536.791 | 536.830 | 23.207 / 9.776 |
| udp-84-to-server-1 | 704.646 | 704.387 | 35.292 / 9.605 |
| udp-96-to-server-1 | 805.351 | 805.252 | 26.234 / 10.212 |
| udp-96-to-server-2 | 805.326 | 804.724 | 26.594 / 9.817 |
| udp-96-to-server-3 | 804.748 | 804.902 | 23.885 / 10.740 |

Forward TCP exceeds the envelope in all three samples. The UDP envelope
samples straddle approximately 805 Mbit/s; they do not prove an exact
805.306368-Mbit/s loss-qualified usable minimum. Do not retune the profile or
classify the tiny differences as an attributed network-capacity failure.

Sender/receiver windows differ slightly, and their buffer totals are not
sequence-matched. **Exact UDP packet loss is not measured.** All five UDP trials
reported zero host UDP error-counter deltas on both machines. These counters
cannot detect every path drop and are not proof of zero loss. TCP host counter
deltas at the client were respectively 22/22/32 retransmissions and 6/8/10
errors, with zero reported by the worker. NTTTCP reads host-wide IP Helper
counters, so unrelated shared-host traffic may contribute; these are not
per-flow Engine errors. Reported CPU is the tool's host aggregate; no aggregate
saturation was observed, but individual-core saturation, process CPU/RSS and
32-client generator adequacy are **not measured**.

The first three TCP setup attempts produced no valid capacity evidence: the
first lost its remote receiver with SSH lifetime; later attempts omitted the
tool's extra synchronization ports. They were bounded/stopped and excluded.
A `-rt -wa` TCP round-trip diagnostic returned zero measured application bytes
despite exit zero and was also excluded. It does not substitute for independent
reverse throughput. A batch-wrapper stale exit-status check was corrected in
ignored preflight tooling; it did not invalidate the completed third TCP
trial's paired child receipts. No Gargantuan correction followed.

## Actual-client and provider gate

| Required physical evidence | Local | Node |
| --- | --- | --- |
| Actual GameSession clients / server launched | 0 / 0 | 0 / 0 |
| Connection establishment and health of 32 clients | not measured | not measured |
| Server tick p50/p95/p99/max, CPU/RSS/network | not measured | not measured |
| Character/root cadence, due-to-observation latency, publication gaps | not measured | not measured |
| RPC RTT/handler/response queue, timeouts/errors | not measured | not measured |
| Event ACK/service and action result latency/rejections | not measured | not measured |
| Load/evict/reload and structural convergence | not measured | not measured |
| Fixed 20-second service recovery | not measured | not measured |
| Exact debt retirement, grants/journal high-water, fairness/backlog | not measured | not measured |
| Client process CPU/RSS, callback intervals, missed observations/disconnects | not measured | not measured |
| Debt/readers/admission owners/content/generations after application shutdown | not measured | not measured |

The canonical [recipient workload](RecipientServiceWorkload3L.md) is unchanged:
eight moving/root Characters with eight recipients each, one qualified gameplay
producer and the shared 512-object / 273,032-byte provider unit through
baseline/load/resident/evict/reload and the accepted overload/recovery cases.
Its offered arithmetic cannot be multiplied accidentally by launching 32
independent producers, or reduced to make the physical run pass.

The existing [`GameSessionBenchmark.cpp`](../../tests/GameSessionBenchmark.cpp)
creates an actual `GameSession`/runtime for content peer index zero and protocol
observers for other peers on `SimulatedNetwork`.
[`DueServiceFixture.hpp`](../../tests/DueServiceFixture.hpp) explicitly states
its one-actual-gameplay-client scope. The historical physical plan calls for a
multi-process extension after funding; that canonical 32-actual-client runner
was not found ready to execute. Merely launching 32 players does not supply
the required measured workload, identity correlation and lifecycle accounting.
No new easier workload or production instrumentation was introduced here.

Keep the [fixed service-recovery gate](PooledReliableServiceRecoveryContract3L.md)
independent of workload-derived structural convergence; retained structural
work need not all finish within 20 seconds. Prior Engine/loopback measurements
retain their original scope, not a physical equivalent. The stricter 200-peer
load/reload tick diagnostic remains separate performance debt.

## Cleanup, evidence and validation

Worker cleanup at `2026-09-15T11:22:10Z` and client cleanup at `11:22:14Z`
found no NTTTCP processes and no remaining task firewall rules. Only worker
rules `Codex-Gargantuan-Physical-20260915-TCP` and
`Codex-Gargantuan-Physical-20260915-UDP` were added and removed. They were scoped
to the exact benchmark program, worker address, client address and ports above.
The client rule attempt created no rule. No UAC prompt, persistent elevation,
firewall-wide relaxation or unrelated service change was made. Existing worker
containers remained up eight days. Retained files are tools/logs, not leaked
processes. Application cleanup metrics remain unmeasured because no application
trial ran; RSS alone is not logical-retention evidence.

Raw inventory, ICMP samples, paired XML/JSON/logs, scripts, cleanup receipts and
a per-file SHA-256 manifest are archived as
`pooled-physical-preflight-20260915.zip`, SHA-256
`6F14F8115C3D08096B490FA7FEBBD71ED9F9C80EA178D13691D8520F1334AED4`:

- Local: `C:\Users\aiden\AppData\Local\Temp\gargantuan-3l-physical-20260915\build\physical\`.
- Worker: `C:\Sandbox\Codex\Logs\gargantuan-3l-physical-20260915\`.

The archive excludes the third-party executable/source; its verified executable
hash and upstream version are recorded above. Local Temp storage is not a
permanent artifact service; the worker copy is retained for resumption.

Existing [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/34900780304)
and [GNS CI](https://github.com/gmoddev/gargantuan/actions/runs/34900779755) were
rechecked as completed/success at exact source `eec0c7123`. No expensive native
suite was rerun for this documentation-only attempt. Documentation validation
passed: 88 relative file targets, eight table rows against paired XML/JSON
receipts, all 109 archived files against the manifest, both cleanup receipts,
matching worker archive SHA-256 and `git diff --check`. No site input changed;
no new application or hosted qualification is claimed by those checks.

## Exact next task

Provision the scoped benchmark receiver on the client with an actual Windows
administrator token, or provide another authorized receiver host. Existing
user permission does not provide that OS token. For this same path/tool, the
local inbound rules must restrict the program to the verified local NTTTCP
executable, local address `192.168.0.68`, remote address `192.168.0.108`, TCP
data/sync ports `55201-55204,56201-56204` and UDP data ports `55201-55204`.
Recreate equivalent temporary worker rules only during trials, then remove
both sides' task rules. Do not disable the firewall or rely on an interactive
firewall/UAC prompt during unattended work.

Complete independent TCP directions, sequence-accounted UDP throughput/loss,
under-load latency and CPU/headroom preflight. If that establishes funding,
complete the planned bounded 32-actual-GameSession-client harness using the
unchanged canonical producer mix, real transport and full observation/cleanup
accounting, then run Local and Node with bounded repeats. If capacity fails,
classify that measured environment without changing Option C.

**Supported physical profile: none established. KI-006 OPEN. Foundation 3L
B — PARTIALLY READY, not ready to close. No merge; 3M BLOCKED / NOT STARTED.**
