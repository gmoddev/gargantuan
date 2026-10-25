---
status: stopped-physical-funding-not-established
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-23
---

# Foundation 3L direct 10 GbE fiber preflight

## Current disposition: accepted funding review

The [approved physical funding review](PhysicalFundingGateReview3L.md) supersedes
this receipt's historical next-task/prerequisite statements. Raw capacity is
established. Proceed to a bounded actual GameSession/GNS pooled-service probe
under static dedicated-link addressing; zero unexplained synthetic UDP loss
and further NDIS attribution are not prerequisites. All recorded diagnostic
measurements, limitations, unsuccessful trials and cleanup remain preserved.
Actual application results belong in the
[physical qualification receipt](PooledPhysicalQualification3L.md). KI-006 remains
OPEN pending the full 32-actual-client Local/Node matrix; no merge or 3M.
The 2026-09-24 bounded GNS probe reached eight waves but stopped because only
three waves gave simultaneous qualified four-grant evidence. It showed no
retransmission increase or below-floor interval; see that application receipt
for the exact stop and unchanged static `/30` address disposition. Further NDIS
attribution remains outside the current acceptance prerequisite.

## Historical approved receive-handoff retry — instrumentation stop (2026-09-23)

**STOP / PHYSICAL FUNDING NOT ESTABLISHED; KI-006 OPEN; 3L B — PARTIALLY READY.**
The operator requested another elevation attempt and approved Windows UAC.
The expanded Packet Monitor / NDIS / TCPIP / WFP collection completed. The
first divergent observed receive boundary remains the last observed filter
upper edge → TCP/IP. Available telemetry does not identify the owning callback,
driver or exact drop reason. No evidence-supported correction was selected.
This supersedes the earlier elevation-pending disposition preserved below.

### Source, path and repeatability

The isolated worktree remains at `4d27238553fdc27b9f6772b438c49393eac3a8eb`;
production Engine source is unchanged from `eec0c7123`. Probe v11 remains
SHA-256 `8ef1e2120b029e125378ef907236f688f6eb8ef2520d8477a7d905e223568dee`.
The direct 10-GbE/MTU-1500 path is HOSTPC Ethernet 4
`169.254.173.205/16` → DESKTOP-B8V8NAN Ethernet 3 `169.254.117.214/16`.
Each completed trial used the temporary same-address static **sender** window,
eight sequence-accounted flows, 1,300-byte payloads, batch eight, 64-KiB send
and 4-MiB receive buffers. The receiver remained on its existing address.
Static is still a candidate physical deployment requirement, used only for
qualification diagnostics here; no permanent address plan was adopted.

| Trial | Duration | Generated Mbps | Received Mbps | Sent / received | Lost |
| --- | ---: | ---: | ---: | ---: | ---: |
| handoff-3 | 8 s | 900.018899 | 899.799008 | 692,304 / 692,137 | 167 |
| handoff-4 | 3 s | 900.080531 | 900.080531 | 259,608 / 259,608 | 0 |
| handoff-5 | 3 s | 900.079571 | 900.079571 | 259,608 / 259,608 | 0 |

All three have zero duplicates, reordering, invalid datagrams and sender
would-block retries. Maximum EWMA jitter is respectively 0.817658, 0.101452
and 0.298715 ms; maximum receive-service gap is 13.7321, 2.2433 and 5.0167 ms.
Timed sender kernel totals are 3.593750, 1.406250 and 1.765625 s; timed user
totals are 59.609375, 22.546875 and 21.375000 s across eight processes.
Receiver process totals are 1.828125, 0.781250 and 0.656250 s.
Whole-wrapper per-core samples average CPU/DPC/ISR percentages of
10.933/1.868/0.821, 12.071/1.196/0.879 and 14.758/0.957/1.156 respectively;
these include background work and setup/teardown and cannot attribute the burst.
No new NETIO stack percentage was measured. The established matched
DHCP/static/DHCP profile remains the sender attribution evidence.

Two clean short controls without a correction do not establish sustained
repeatability. Observed service in this retry ranges 900.018899–900.080531 Mbps
generated and 899.799008–900.080531 Mbps received, in one direction only.
There is still no certified repeatable bidirectional floor.

### Boundary and metadata reconciliation

Actual receive ordering is determined from `LowerifIndex` linkage, not sorted
component IDs: miniport 2/if50 → WFP Native 24/if51 → Npcap 159/if52 → QoS
22/if53 → WFP 802.3 21/if54 → TCPIP 65. The binding inventory below remains
valid. WFP 802.3 reports an optional datapath; its absent packet snapshots do
not prove actual bypass or callback completion. TCPIP 65's aggregate secondary
components must not be confused with its observed NDIS protocol edge.

Packet Monitor snapshots establish observation at a component boundary, not
successful completion of all subsequent receive indications or buffer returns.
The five observed pre-TCPIP edges are miniport upper, both WFP Native edges,
and both Npcap edges. QoS/WFP 802.3 above them have no matching data snapshots.
Thus the evidence cannot name Npcap, QoS, WFP, NDIS or TCPIP as the drop owner.
The earlier reversible Npcap exclusion still demonstrates that Npcap is not
sufficient to explain the loss. No additional filter exclusion was justified.

`handoff-3` records all **167** missing sequences at all five pre-TCPIP edges
and none at TCPIP. Their appearances occupy
**22:05:43.9263375–22:05:43.9279887 UTC**. The memory capture overwrote early
events despite reporting “No events lost.” Partial initial per-CPU buffers also
leave holes, so whole-trial ETL counts must not be used. Payload/token/sequence
analysis verifies a complete retained suffix separately for every flow:
**323,058** data packets at each pre-TCPIP edge and **322,891** at TCPIP,
exactly matching application delivery in those same suffixes. The 64 sequence
neighbors on each side of each missing range have complete non-loss coverage.
Every missing sequence lies well inside that verified suffix.

`handoff-4` independently verifies full capture coverage: all **259,608**
packets appear at every one of the six observed edges and at the application.
The second short control's application accounting is clean; its raw ETL is
preserved without claiming a second complete per-boundary decode.

The expanded `0x3f` capture includes NBL metadata: all **835** appearances of
the 167 missing packets have metadata. Their opaque values are `Checksum=56`,
`HashInfo=257`, `Rsc=0`, with the same per-flow RSS hash as delivered packets.
There is no distinct metadata signature explaining loss. Four unrelated
appearances at the retention boundary lack metadata. Packet Monitor group IDs
remain unsuitable for reconstructing cross-edge NBL ownership; a native
indication/return identity join is still absent. Neither metadata equality nor
an upper-edge snapshot proves synchronous versus deferred callback completion.

### Native drops, RSS and Mellanox findings

The retained native trace contains 1,311 TCPIP event 1214, 157 event 1215 and
six event 1423 records. None joins the qualification UDP tuple. Eight records
containing the fiber endpoint addresses are TCP control teardown, reason 20,
after the trial. Other fiber-interface records include broadcast/IPv6 traffic;
interface membership alone is insufficient attribution. No Packet Monitor
drop event accounts for the missing sequences and no WFP provider event was
recorded in the retained window. Some remote manifest-based NDIS rundown
decoding reports schema warnings; raw ETLs are preserved and malformed
rundown payloads are not used as attribution evidence.

Four NDIS event 10204 records on the receiver interface show temporary
receive-limit changes 1,024 → 512 → 1,024 on CPUs 12 and 4. The latter
reduction reports 15 ms processing duration; restoration completes at
22:05:43.8057220, **120.6155 ms before** the earliest missing-packet appearance.
These records contain no qualification sequence/queue/drop identity. They
demonstrate receive throttling activity but do not explain the later loss burst
or justify an RSS, affinity, moderation or buffer change.

Elevated RSS inventory now exposes a 128-entry table: group 0 processors
4, 6, 8, 10, 12, 14, 16 and 18 each have 16 entries, with eight configured
receive queues. This is configuration, not a live hardware queue ownership
trace. No queue-specific missed-packet, descriptor-exhaustion or allocation-
failure counter is exposed by the installed driver. Built-in per-processor
NIC counters remain unchanged even for received packets during traffic, so
their zero low-resource deltas are **not usable evidence against pressure**.
Generic NIC receive discard/error deltas are zero in all three trials;
machine-wide IP discard changes do not join the missing sequences.

Fresh elevated MFT queries identify **both** adapters as MCX4121A-ACA_Ax,
PSID MT_2420110034, firmware **14.29.1016**, driver **2.53.23539.0**.
Port queries show no physical errors in the queried counters, but do not expose
the missing indication/queue ownership. No vendor miniport defect is established,
so no speculative driver or firmware update, binding change or tuning is selected.

### Funding, cleanup and next task

| Unchanged funding matrix | Worker → workstation | Workstation → worker |
| --- | --- | --- |
| 805.306368 Mbps | Not rerun: correction/repeatability prerequisite unmet | Not rerun |
| 900 Mbps headroom | Diagnostic trials above; no funding verdict | Not rerun |

The available supported instrumentation cannot distinguish the owner further;
the requested instrumentation stop condition applies. Physical funding remains
**NOT ESTABLISHED**, KI-006 remains **OPEN**, and the accepted POOLED_SERVICE
envelope is unchanged. No application clients, merge or 3M work occurred.

Both endpoints are restored to DHCP with their original preferred /16 addresses,
10-GbE link and MTU 1500. All task rules, captures and helper/probe processes
are closed; bindings and driver settings are unchanged. The existing System32
PktMon duplicate was left untouched. Capture output for this retry is under
the isolated build directory. Setup failures are preserved: handoff-1's broad
counter startup timed out before traffic; handoff-2 sent no test data because
the temporary address was still tentative. Bounded address-readiness waiting
corrected the harness before handoff-3. These are not qualification trials.

Exact next task: obtain Windows/NDIS or vendor-assisted receive indication and
return ownership tracing for this tuple/sequence burst, with live queue/resource
telemetry and preserved buffer identity across the unobserved upper filters and
protocol handoff. Require a specific ownership/drop explanation before selecting
a correction, then repeat the unchanged 900-Mbps tool trials and bidirectional
805.306368/900-Mbps funding matrix. Broad tuning or security-policy weakening
does not follow from this evidence.

Evidence: `build/fiber-handoff/live-summary.json`, native-event and NBL metadata
joins, both boundary decodes, four raw captures and cleanup receipts. The new
`build/fiber-handoff-live-20260923.zip` preserves the approved retry separately
from the immutable prior elevated and offline-continuation archives.
It contains **482 evidence files plus manifest**, **251,831,869 bytes**,
SHA-256 `e0987bfd2581ebabc57ebd268f1f15806b737df87eb197e5862397195232728a`.
Every worker ZIP entry was decompressed and SHA-256 checked against the manifest;
the downloaded local archive hash matches. Worker copy:
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-handoff-live-20260923.zip`.
Earlier archive hashes remain `d7877ad800b3e6c2804d9a8752b0f2d521f0083bd266af801c639a4fd5b986d1`
(elevated) and `a96a3b13ab966bac9759003d2f2e6145e67d7d467962c0157946f90b47198b37`
(pre-approval offline continuation); neither was replaced.

## Historical pre-approval continuation — capture semantics and inventory (2026-09-23)

**Historical state, superseded by the approved retry above: receiver elevation pending. Physical funding remained unqualified;
KI-006 OPEN; 3L B — PARTIALLY READY.** The follow-up preserves the established
sender result and targets the remaining receive handoff. Windows canceled the
new receiver helper's elevation request. No helper, new capture, traffic trial,
temporary rule, static window or binding experiment started. A retry requires
the operator's direction; no alternative elevation path was attempted.
This is an access limitation for the new trace, not proof that supported
instrumentation cannot further distinguish the boundary.

Source remains the isolated `4d27238553fdc27b9f6772b438c49393eac3a8eb` worktree.
The preceding 677-file / 32-ETL archive's SHA-256 was rechecked unchanged.
No Engine or accepted profile change, application-client run, merge or 3M occurs.

### Boundary semantics and actual binding inventory

The precise established interval is **after the last observed receive-filter
upper edge and before the TCP/IP capture point**. "Last observed" does not
mean that every configured filter has reported or successfully completed its
receive callback. Microsoft's [Packet Monitor semantics](https://learn.microsoft.com/en-us/windows-server/networking/technologies/pktmon/pktmon-syntax)
define snapshots at component-boundary crossings. An upper-edge receive
snapshot is more specific than adapter visibility, but it does not acknowledge
protocol acceptance, callback completion or eventual buffer ownership.

The preserved elevated fiber binding inventory, in receive-stack display order,
is Mellanox `mlx5.sys` → WFP Native `wfplwfs.sys` → Npcap `npcap.sys` → QoS
Packet Scheduler `pacer.sys` → WFP 802.3 `wfplwfs.sys` → protocols. The observed
data snapshots stop at Npcap before TCP/IP in the lossy run; QoS and WFP 802.3
are configured above it but have no matching data snapshots in that capture.
Microsoft documents [optional receive-handler bypass](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/data-bypass-mode).
Bypass is a possible explanation for absent snapshots, not a verified handler
state here. Neither those modules nor NDIS itself can be assigned the drop
solely from their position. The previous Npcap exclusion result remains valid.

Fresh non-elevated binding inventory confirms IPv4/IPv6, QoS, Npcap, both WFP
bindings, NDIS capture, LLDP, topology discovery, NDISUIO and RDMA-NDK enabled.
Hyper-V extensible-switch and multiplexor bindings are disabled on the fiber
interface. Nested-network-virtualization is enabled in binding configuration
but not separately exposed as a module in the preserved Packet Monitor stack.
Installed/enabled binding inventory and active callback observations are distinct.
Protocol bindings also include SMB/client services, NetBIOS/WINS and PPPoE;
the Packet Monitor protocol list reports TCPIP/TCPIP6, RSPNDR, LLDP, NDISUIO
and LLTDIO. No binding was disabled in this continuation.

Offline re-decoding checks **4,152,826 events** and all **11,220 observations**
of the 2,244 missing sequences across the five NIC/filter edges. Each missing
sequence has five different Packet Monitor group IDs, all with appearance zero.
Thus payload tuple/token/sequence remains the reliable cross-edge identity;
group IDs cannot reconstruct an NBL lifetime here. This does not itself prove
cloning, corruption or a driver defect. No Packet Monitor drop event or NDIS
provider event is present in that original ETL, which omitted NBL metadata flags.

Native Packet Monitor decoding recovers emission CPUs that EventLogReader's
XML omits. Missing-packet CPU distributions agree across all five observed edges:

| Native CPU label | NIC data packets | Missing application sequences |
| --- | ---: | ---: |
| 08 | 86,538 | 284 |
| 10 | 173,076 | 569 |
| 12 | 259,614 | 835 |
| 16 | 86,538 | 271 |
| 18 | 86,538 | 285 |

All 692,304 NIC packets and 2,244 missing sequences reconcile. Loss occurs on
all five observed receive CPUs, not just one emission CPU. This does not map
hardware queue IDs or prove balanced queue service; no affinity/RSS correction
follows from it. Native text uses the decoding worker's local timezone, while
the XML UTC timestamps are retained for correlation.

### Queue, miniport and firmware limits

Both endpoints retain driver **2.53.23539.0**, eight configured receive queues,
maximum eight RSS processors, 512 receive buffers, MTU 1500 and 10-GbE link.
The workstation's detailed RSS processor array/indirection fields remain null
under this token. A configured queue count does not establish the queue used by
a given missing packet. The adapter registry exposes `RecvCompletionMethod=1`;
no explicit `AsyncReceiveIndicate` override is present. Neither value alone
proves synchronous versus deferred ownership through the complete filter chain.

Neither inventory query exposes the NVIDIA/Mellanox WinOF-2 counter sets.
Windows per-processor NIC activity counters exist, including low-resource
receive indicators, but an idle snapshot cannot explain historical trial loss.
NVIDIA's [counter reference](https://networking-docs.nvidia.com/winof2driverum/24150000/adapter-cards-counters)
describes additional vendor diagnostics; their absence here must not be
interpreted as zero queue overruns, allocation failures or descriptor pressure.
No receive-buffer, RSS, moderation, affinity or priority adjustment is justified.

Read-only worker MFT rechecks ConnectX-4 Lx MCX4121A-ACA_Ax, PSID MT_2420110034,
firmware **14.29.1016**, 10-GbE link and zero physical error/link-down counters
since their last reset. Those are worker physical-link observations outside a
new traffic trial, not workstation receive-queue evidence. Current workstation
firmware could not be freshly queried without elevation. No matching miniport
defect is established; no driver/firmware update or diagnostic mode was applied.

### Pending trace and gate

The prepared bounded helper adds Packet Monitor NBL address/metadata flags,
native NDIS events (including receive throttling and queued indication events),
full TCPIP/WFP events, and before/after per-processor NIC counters to the trusted
eight-flow probe. It preserves existing capture sessions and restores its own
filters. It has not run or supplied evidence yet. Packet metadata can expose
RSS hash information, but a hardware queue ID or complete NBL lifetime is not
assumed available before examining the resulting events.

No new correction, repeatability proof or bidirectional funding matrix exists.
The last measured static eight-flow range remains 899.944–899.998 Mbit/s
generated and 897.078–899.944 Mbit/s received, with unexplained intermittent loss.
Static remains a reverted diagnostic condition and candidate deployment
requirement, not an accepted persistent address plan. DHCP/APIPA remains active.
The old `C:\Windows\System32\PktMon.etl` duplicate is left untouched.

**Next:** obtain the receiver's elevated token, run the bounded metadata/NDIS
trace, join missing sequences to NBL identity, processor and native drop events,
then select a correction only if ownership or pressure is established. Funding
and the 32-client matrix remain gated. Continuation evidence and prepared
diagnostic helpers are under `build/fiber-handoff`; production code is unchanged.

The continuation archive `build/fiber-handoff-20260923.zip` contains **43 files
plus manifest**, including the native text decode and offline analyses, at
**49,831,710 bytes**. Every entry was decompressed and SHA-256 checked, and the
downloaded archive verified against
`a96a3b13ab966bac9759003d2f2e6145e67d7d467962c0157946f90b47198b37`.
Original ETLs remain in the unchanged preceding archive rather than being
duplicated. Both host closeout checks pass; no task rules or processes remain.

## Established elevated attribution (earlier 2026-09-23)

**STOP / PHYSICAL FUNDING NOT QUALIFIED. KI-006 OPEN; 3L B — PARTIALLY READY.**
Matched profiling now confirms that removing the worker DHCP/APIPA condition
removes the pathological NETIO/`FindCacheMatch` cost. DHCP raw UDP socket
creation is directly observed. Elevated receiver tracing localizes natural
loss to the handoff **after the last receive filter's upper capture edge and
before TCP/IP's capture point**. Receiver threads wait for delivery during the
long gaps; they are not spending those gaps runnable and unscheduled. The
specific queue/drop reason inside that handoff remains unobserved, so no
reliable loss bound or correction is established. The final funding matrix
was not run. No application clients, Engine changes, merge or 3M occurred.

### Source, instrumentation and matched sender result

The isolated worktree remains at `4d27238553fdc27b9f6772b438c49393eac3a8eb`,
with unchanged production Engine relative to `eec0c7123a39698761e49ed276ee37aae673e023`.
The prior WFP archive hash is verified unchanged. The same v11 executable
(`8ef1e2120b029e125378ef907236f688f6eb8ef2520d8477a7d905e223568dee`) runs on
both endpoints. The workstation token was initially filtered; an authorized,
hidden Windows-elevated helper enabled WFP inventory, Packet Monitor and WPR.
The helper limits each trace to 90 seconds and restores its diagnostic binding
change on exit. Worker control remains through LAN SSH; data stays bound to
the direct Mellanox fiber addresses.

All three sender states use **one flow, batch one, 900 Mbit/s, 1,300-byte
payload, QPC pacing, 64-KiB send and 4-MiB receive buffers**, with an eight-second
target and the unchanged ten-second generation bound. CPU sampling and WFP
tracing use the same settings and executable in every state. Static preserves
the worker's `169.254.173.205/16`, gateway absent, using only `store=active`.

| State / trial | Generated Mbit/s | Process kernel CPU | Send median / p99 µs | NETIO sampled share | FindCacheMatch sampled share | Filter 147332 drops |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| DHCP/APIPA, `aba-dhcp-1` | 140.840 | 9.813 s | 67.8 / 135.9 | 64.646% | 44.356% | 135,424 |
| Static same address, `aba-static-1` | 900.001 | 2.891 s | 3.7 / 11.7 | 2.745% | 0% | 0 |
| Restored DHCP/APIPA, `aba-restored-1` | 145.683 | 9.531 s | 63.2 / 137.7 | 66.549% | 46.308% | 140,080 |

Percentages are exclusive sample weights divided by sampled probe-process CPU,
not total host CPU. Complete distributions, user/timed-kernel CPU and sample
weights are in `trial-summary.json` and the matched profile CSV/stack reports.
The two slow runs submit/receive 135,423 and 140,080 packets without data loss;
the WSH counts above are the observed counts, not inferred from packet totals.
The raw-copy classification cost disappears with static addressing and returns
with DHCP, beyond throughput-only evidence. No cache hit/miss counter is exposed;
the cost is measured directly, but the internal cache algorithm is not diagnosed.

The filter remains **WSH Default Inbound Block**, provider
`FWPM_PROVIDER_MPSSVC_WSH`, sublayer `FWPM_SUBLAYER_MPSSVC_WSH`, at
`FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4` (44), effective weight 274877906944.
The earlier complete provider/callout inventory remains applicable; a new full
workstation WFP inventory is archived. No security provider, WSH filter or
firewall policy was disabled or weakened.

### DHCP/raw-copy ownership and static-address disposition

A separate AFD/DHCP lifecycle trace around a bounded active-address transition
records DHCP service **PID 2984** creating an **AF_INET / SOCK_RAW / IPPROTO_UDP**
endpoint (`0xffffab0aeaaf2dd0`) successfully, alongside a datagram endpoint.
The raw endpoint attempts an unspecified-address bind with port 68, completes
with port zero, and sends DHCP requests to `255.255.255.255:67`. Its creation,
bind and send events are preserved in `dhcp-afd-lifecycle.json` and the raw ETL.
This supplies direct raw-socket ownership evidence missing from the prior task.

Windows documents copying matching IP datagrams to raw sockets according to
protocol and address, which supports this interpretation of the observed raw
receive path ([Microsoft raw-socket semantics](https://learn.microsoft.com/en-us/windows/win32/winsock/tcp-ip-raw-sockets-2)).
Combined with the prior `IppLbIndicatePackets → RawReceiveDatagrams →
WfpAleAuthorizeReceive → FindCacheMatch` stack, WSH events naming LocalService
`svchost.exe`, and the matched address-state experiment, DHCP's raw endpoint
explains the service-copy candidate. WFP drops do not carry the AFD endpoint
pointer, so an event-by-event join to that exact endpoint is not claimed.
There is no evidence that WSH's default block is erroneous; permitting it is
not a proposed correction. Static removes the DHCP-dependent condition without
changing security policy. APIPA changing IP routing semantics is not separately
proved.

Explicit static addressing is a **candidate physical-link configuration**, but
is **not accepted or retained as a deployment requirement** in this task.
The same-address static window is diagnostic only; a permanent address plan,
user/network expectations and persistence remain to be confirmed after receive
attribution. No reboot or static-address persistence claim is made.

### First missing receive boundary

The instrumented static single-flow trial receives 690,145 of 692,307 packets.
**271** packets are absent at the NIC/Npcap capture points and exactly match the
NIC receive-discard increment. The other **1,891** missing sequences appear at
the NIC, both WFP Native Filter edges and both Npcap edges, but are absent at
TCP/IP component **65**. Its 690,145 data sequences exactly match application
delivery. This distinguishes the instrumented trial's additional NIC loss from
the post-adapter deficit established previously.

The final eight-flow trace, `rx-schedule-1`, provides the decisive comparison:

- All **692,304** submitted sequences are present at Mellanox component **2**,
  both WFP Native Filter **24** edges, and both Npcap **159** edges.
- TCP/IP component **65** contains **690,060** unique sequences: precisely the
  application's set. All **2,244** missing application sequences are already
  absent there. No captured TCP/IP data sequence is missing from the application.
- NIC errors/discards, receiver UDP InErrors and reported matching AFD drops
  do not account for this deficit. Packet Monitor reports no matching data-packet
  drop event; per-packet sequence accounting is authoritative, rather than its
  inflated aggregate filter counters.

The first missing observable point is therefore **TCP/IP after the last filter
upper edge**. This does not identify the precise NDIS/stack queue or imply that
Npcap itself dropped the packet. A focused Npcap binding A/B/A experiment uses
Packet Monitor throughout and no active Npcap capture in any of its three runs:

| Eight-flow diagnostic | Generated / received Mbit/s | Missing | Npcap binding |
| --- | ---: | ---: | --- |
| `rx-bound-1` | 899.966 / 897.162 | 2,146 | Original enabled |
| `rx-unbound-1` | 899.991 / 898.527 | 1,126 | Temporarily disabled on fiber only |
| `rx-restored-1` | 899.944 / 899.944 | 0 | Restored |
| `rx-schedule-1` | 899.998 / 897.078 | 2,244 | Restored; larger scheduling trace buffer |

In the unbound run, every submitted sequence reaches the NIC and both WFP
Native Filter edges; the 1,126 missing packets again disappear before TCP/IP,
whose set equals application delivery. Thus removing Npcap is not a demonstrated
fix. That unbound trace separately records 1,925 AFD drops with 1,328-byte raw-IP
buffers and source port zero on another endpoint; they do not account for the
1,126 missing 1,300-byte probe datagrams. The final trace has no AFD drop records.
The restored clean run is an intermittent clean sample. Npcap was re-enabled;
no binding change is retained. All four eight-flow runs have zero duplicates,
reordering, invalid packets, WouldBlock retries and NIC discard/error deltas.
The separate static single-flow run has 118 reorderings, explicitly retained.

### Scheduling, queue coverage and packet timing

Initial 256-buffer memory traces overwrite workload events on busy CPUs despite
zero lost-event counters. Those traces are preserved but excluded from scheduling
causality. After removing context-switch/ready stack collection and increasing
the bounded kernel buffer count, the final trace covers the entire receive
window on **all 32 CPUs**. It preserves 13,240–20,343 ready events per receiver
thread. Offline decoder warnings about other event schemas are retained; the
CSwitch/ReadyThread records and coverage are checked independently.

The largest per-flow gaps are **15.196–29.657 ms**. In each largest gap, the
receiver leaves the CPU in **Waiting / UserRequest**, and another task or Idle
runs. For the 29.657-ms gap, **29.644 ms is blocked**, followed by **6 µs runnable
before scheduling**. Across the eight largest gaps, ready-to-running delay is
3–6 µs. Thread migrations occur on wake; they do not turn those blocked intervals
into runnable starvation. This is consistent with the probe's synchronous
`Socket.Receive` waiting for upstream delivery. No receiver CLR suspension occurs
during the receive window; recorded suspensions are after it. QPC/precise-UTC
calibration checks differ by about 4.75 µs across the observation interval.

Surviving packets take up to **28.957 ms from NIC capture to TCP/IP**, almost all
after the last filter edge (maximum **28.949 ms**). This directly correlates
upstream delivery delay with the receiver waits. It does not identify which
upstream thread/queue caused the delay or prove an upstream DPC/ISR starvation
mechanism. No receiver priority/affinity change is justified or retained.
The final trial's sampled receiver DPC/ISR maxima are 37% / 6%, receiver-process
CPU totals 1.65625 seconds across the eight flows, and worst reported jitter is
1.810668 ms. These aggregate measurements do not identify the handoff's owner.

NIC capture shows the expected tight eight-packet batches: per-flow median
inter-arrival 0.3–0.4 µs, p99 737.5–744.9 µs, and maximum gaps 2.134–9.119 ms.
The aggregate maximum is **264 packets in a sliding 1-ms interval** (240 in
fixed bins); per-flow fixed-bin maxima are 40–96 packets. Among delivered
packets, sender QPC stamps show up to **184 packets per sliding millisecond**,
with pacing lag up to 7.668 ms. The populations differ by the missing packets;
these are diagnostic timestamps, not hardware wire timing. Sender catch-up
bursts and host batching both remain relevant, distinct from the measured
last-filter-to-TCP/IP delay.

At 112.5 Mbit/s per flow, a 29.657-ms interval represents about **417,052 bytes
(0.398 MiB)** of payload, below each 4-MiB socket buffer. More importantly,
the missing sequences do not reach the TCP/IP capture point. Socket occupancy
was not directly measured here, and a buffer increase is not justified by this
boundary. The preceding positive queue-overflow control remains established
evidence. No new buffer sweep or affinity experiment was run.

### Stop, cleanup and remaining gate

Seven completed trials contain **35 paired flows**. Sender generation under
temporary static addressing is repeatable near 900 Mbit/s, including four
eight-flow runs at 899.944–899.998 Mbit/s. Their received rates range from
897.078–899.944 Mbit/s, but intermittent unreported loss has no established
causal bound. **No repeatable bidirectional service floor is certified.**
The canonical **805.306368-Mbit/s** requirement is unchanged. The final repeated
805.306368/900-Mbit/s funding matrix remains deferred.

Elevated tools now expose the first missing boundary, but the handoff's internal
drop reason remains unobserved. The requested stop applies at that remaining
instrumentation boundary. No changes to production Engine, accepted service
profile or security policy are proposed to obtain a pass.

Cleanup verifies original preferred APIPA addresses and DHCP on both hosts,
MTU 1500 and 10-GbE links, exact original advanced/RSS/binding settings, no
task probes/listeners/controllers/firewall rules, and stopped WPR/ETW/Packet
Monitor sessions. Worker-to-workstation bound ICMP passes 3/3; LAN SSH remains
functional. The pre-existing Public worker profile's inbound management
restriction remains documented below. Static windows and the fiber-only Npcap
unbind are **C — diagnostic/reverted**. Probe settings remain **A — tool
configuration**; no **B — deployment requirement** or **D — unrelated correction**
is accepted. The prior larger-send-buffer result remains provisional.

One default-output duplicate, `C:\Windows\System32\PktMon.etl` (78,162,009 bytes),
remains because Windows elevation for its deletion was canceled. It matches
the preserved `rx-schedule-1-pktmon.etl`; no capture session remains active.
No further deletion/elevation attempt was made after cancellation.

**Exact next task:** instrument the Windows receive handoff between the last
filter upper edge and TCP/IP's first indication, including queue/indication
ownership and drop reason, using the preserved missing sequence sets and timing
windows. Establish a supported correction and repeatability before accepting a
persistent static address plan and rerunning the unchanged bidirectional funding
matrix. The 32-actual-client Local/Node matrix remains a later task; no 3M.

Evidence is under `build/fiber-elevated`, with worker originals under
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-elevated-20260923`.
`Validate.py` checks sequence balances, exact capture boundaries, matched sample
weights, complete final scheduling coverage, restoration and prior archive hash.
Flow attribution uses the UDP tuple plus token and sequence; simultaneous
processes can share a timestamp-derived token. The four documentation files
remain uncommitted; no production test suite was rerun for diagnostic-only work.

The completed `build/fiber-elevated-20260923.zip` contains **677 evidence files
plus its manifest**, including **32 ETLs**, and is **1,054,412,883 bytes**.
Every evidence entry was decompressed and SHA-256 checked on the worker; the
downloaded archive's SHA-256 also matches:
`d7877ad800b3e6c2804d9a8752b0f2d521f0083bd266af801c639a4fd5b986d1`.
The archive receipt and standalone manifest are saved beside it as
`build/fiber-elevated-archive-receipt.json` and
`build/fiber-elevated-manifest.json`. Large decoded scheduling/capture files
remain in the worker evidence root and are included in the archive.

## Historical WFP condition and receive boundary (earlier 2026-09-23)

The following section preserves the prior task's evidence. Its unresolved raw
ownership and non-elevated tracing limit are superseded by the section above.

**STOP / PHYSICAL FUNDING NOT QUALIFIED. KI-006 OPEN; 3L B — PARTIALLY READY.**
The worker sender collapse is now associated with a specific Windows Service
Hardening filter and a reversible **DHCP/APIPA versus static-address condition
on the fiber interface**. Independently, natural reverse receive deficits are
localized after the workstation's Npcap adapter capture and before application
delivery. Their precise stack/drop mechanism remains unresolved. The final
funding matrix was **not run**, because both attribution prerequisites are not
closed. No actual application clients, Engine changes, merge or 3M occurred.

Source remains the isolated `4d27238553fdc27b9f6772b438c49393eac3a8eb` checkout,
production-equivalent to `eec0c7123a39698761e49ed276ee37aae673e023`. The direct
7950X3D `169.254.117.214` ↔ 5900X `169.254.173.205` Mellanox path, 10 GbE,
MTU 1500, driver, firmware, offloads, RSS and moderation are unchanged.
The earlier 418-file diagnostic and 1,476-file profiling archives were reused
and their hashes remain unchanged. Their experiments are not rerun as sweeps.

### WFP ownership and the sender condition

Supported [WFP inspection](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh-wfp)
on the worker enumerated **16 providers, 28 sublayers, 93 callouts and 3,051
filters**, before this task's two temporary probe rules. Windows Firewall owns
2,835 filters, Windows Service Hardening 162, app isolation 24, Codex sandbox
12, NDU six, and other/unnamed owners 12. Registered Tessera and Codex sandbox
providers are inventory facts; no event attribution or removal implicates them.
The installed security inventory reports Windows Defender on both hosts.
The workstation also has Npcap bound to its fiber adapter. Hyper-V/container
components are inventoried; their presence alone is not causal evidence.

Relevant IPv4 layers include ALE resource assignment, receive/accept, connect,
flow establishment, datagram data, and transport-fast NDU inspection. The
`windefend_datagram_v4` callout filter is present at DATAGRAM_DATA_V4. Complete
conditions, provider/sublayer keys, actions, effective weights and scoped
inbound/outbound candidate lists are retained. The verbose candidate lists
include filters that need not actually match this flow.

The pathological single-flow trace identifies **filter 147332**, named
**WSH Default Inbound Block**, provider `FWPM_PROVIDER_MPSSVC_WSH`, sublayer
`FWPM_SUBLAYER_MPSSVC_WSH` (weight **3**), at
`FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4` (**layer 44**). Its effective filter weight
is **274877906944**. It blocks matching service security identities through an
`ALE_USER_ID` security descriptor; it is not a Mellanox callout.

The worker generated 136,456 datagrams and recorded **136,457 WFP event-1001
drops** at that filter: one for each data packet plus the UDP handshake.
These events name `svchost.exe`, LocalService, interface scope 19 and
`Loopback=true`, with `ReauthReason=0`. All submitted data reached the peer.
Thus these drop events describe another local/raw receive path; they are not
loss of the probe's transmitted packets. This agrees with the earlier failing
stack's `IppLbIndicatePackets → RawReceiveDatagrams → RawDeliverDatagrams →
ProcessAleForNonTcpIn → WfpAleAuthorizeReceive → KfdClassify →
CompareSecurityContexts → FindCacheMatch` path.

DHCP tracing records failed receives/retries on the exact worker fiber GUID
and interface 19, from the running DHCP service PID 2984. Both fiber endpoints
use DHCP-enabled APIPA addressing without a DHCP server. A single bounded
experiment changed **only the worker interface's active address configuration**:
the same `169.254.173.205/16`, no gateway, temporarily static using
`netsh interface ipv4 set address ... store=active`. A `finally` block restored
DHCP, the same preferred APIPA address and its original origins. No service or
security filter was disabled, excluded or permitted around.

| Condition / single-flow trial | Generated Mbit/s / datagrams/s | Kernel CPU | Send-call median | Matching WSH drops |
| --- | --- | ---: | ---: | ---: |
| DHCP/APIPA, `attribution-single-3` | 141.911 / 13,645 | 9.688 s | 66.7 µs | 136,457 |
| Temporary static, `static-single-1` | 900.001 / 86,539 | 2.297 s | 3.2 µs | 0 |
| Temporary static, `static-single-2` | 900.001 / 86,539 | 2.391 s | 3.2 µs | 0 |
| Restored DHCP/APIPA, `restored-single-1` | 152.472 / 14,661 | 9.797 s | 66.5 µs | 146,609 |

All use a 900-Mbit/s request, 1,300-byte payload, eight-second target and
64-KiB send / 4-MiB receive buffers. The slow runs reach the ten-second
generation bound. Both static runs deliver all 692,307 packets, conservatively
899.985/899.984 Mbit/s. The restored run submits/receives 146,608 packets and
again has exactly one WSH drop per packet plus handshake. The two static runs
and restored run use the same v11 executable; v10 differs only in receiver
instrumentation, not sender behavior.

This A/B/A comparison supports the **interface DHCP/APIPA condition as a trigger
for repeated service-hardening classification**, beyond a throughput-only
improvement. It does not yet separate DHCP retry/socket lifetime from address
reconfiguration's cache invalidation, identify the exact raw endpoint object,
or count cache hits/misses. DHCP is the supported service candidate, not a claim
that a new trace directly resolved the raw socket's owning handle. WFP
`FindCacheMatch` activity alone is not a cache-miss counter. The earlier
same-binary NETIO CPU/stack comparison remains the sampled cost evidence;
fresh matched NETIO sample percentages were not obtained for this A/B/A pair.
An additional lifecycle WPR trace records DHCP transitions but supplies no
usable AFD raw-socket ownership or WFP event coverage.

### Independent receive localization

The probe records per-sequence receive and sender QPC values in preallocated
arrays, written only after reception. Existing Npcap captures only the fiber
peer's test UDP ports, with 96-byte snapshots and a bounded 22-second capture.
The decoded token and sequence account for **every submitted datagram** in all
ten completed trials. Capture reports zero drops and duplicates. Capture disk
writes are diagnostic overhead; these instrumented runs are not a disk-free
funding certification. Payload generation/receive loops still use memory only.

Four eight-flow reverse diagnostic repeats use the unchanged eight-datagram
batching and QPC phase pacing, 4-MiB receive / 64-KiB send buffers:

| Trial | Generated Mbit/s | Conservative received Mbit/s | Missing datagrams | Interpretation |
| --- | ---: | ---: | ---: | --- |
| `attribution-flows-1` | 899.985 | 899.732 | 183 | All missing sequences captured at receiver adapter |
| `attribution-flows-2` | 899.992 | 899.987 | 0 | Clean bounded sample |
| `attribution-flows-3` | 793.499 | 793.499 | 0 | Sender undergenerated; excluded from receive-capacity analysis |
| `attribution-flows-4` | 900.003 | 896.536 | 2,657 | All missing sequences captured at receiver adapter |

Each submits 692,304 packets. Actual datagram rates, jitter, user/kernel CPU,
per-core DPC/ISR samples, service gaps and NIC deltas are in `trial-summary.json`.
The last trial generates 86,538.760 datagrams/s and receives 86,205.420;
maximum EWMA jitter is 1.719 ms, sender kernel CPU totals 2.844 s, and sampled
receiver per-core DPC/ISR peaks are 29%/6%. The largest reported service gap
across its eight flows is 28.13 ms. Samples alone cannot assign that gap's cause.
At 1,300 bytes, the unchanged 805.306368-Mbit/s envelope requires
77,433.305 datagrams/s; 900 Mbit/s requires 86,538.462. All current trials have
zero WouldBlock retries, duplicates and invalid packets. The earlier new
single-flow clean-rate capture has **34 reordered packets** and 159 missing;
the other nine completed trials have no reordering. Service-gap analysis sorts
actual receive QPC values, rather than assuming sequence order equals time order.

In the last natural-loss trial, **all 692,304 packets are independently captured**,
but the application receives only 689,647. NIC TX/RX error/discard deltas and
aggregate UDP InErrors do not explain the deficit. The trace has **no AFD
datagram-drop event**, no matching UDP WFP drop and no matching UDP TCP/IP
drop. Eight fiber TCP/IP transport-drop records concern TCP control teardown,
not the UDP workload. Trace statistics report zero lost events/buffers.
This narrows the boundary to **after Npcap's adapter capture and before
application socket delivery**, without proving whether an NDIS filter, stack
queue/coalescing path, socket behavior or unreported drop is responsible.

The same last trial shows receive-service gaps of up to 27.64 ms on slot zero;
capture's largest inter-arrival gap for that flow is only 1.06 ms. Its eight-
packet batches have at most 16 packets in a fixed 1-ms bin on that flow.
These are capture timestamps, not hardware wire timestamps. The gaps recur
across receiver flows while the sender continues at target rate. This supports
a receive-service correlation, but cannot identify thread descheduling versus
DPC/ISR, classification or other host activity without receiver kernel tracing.

A separate **deliberate receiver-pause control**, excluded from qualification,
requested a 100-ms pause and measured 115.025 ms. `Socket.Available` then reads
4,194,304 bytes. It loses **6,729 sequences**, all captured, with exactly
**6,729 AFD event-1033 drops (reason 2, 1,300-byte payload) on the matching probe
socket**. A further 77 AFD drops have 1,328-byte raw-packet buffers on another
endpoint and are not counted as probe loss. This validates observation of
AFD queue exhaustion; it does **not** explain away natural losses that produce
no corresponding AFD records. The receiver's natural 2,657-packet deficit and
the sender's WSH raw-copy loop remain separate findings.

### Stop boundary, disposition and next task

The workstation permits scoped Npcap and user-provider ETW capture, but returns
access denied for full WFP state export, Packet Monitor, and both kernel and
event-only WPR profiles under the current token. No elevation prompt or bypass
was attempted. Available instrumentation cannot resolve the remaining natural
receive-drop layer or scheduling cause, so the requested stop condition applies.

There is **no certified repeatable bidirectional service floor**. The highest
repeatable conditional worker single-flow generation is 900.001 Mbit/s in two
temporary-static runs, receiving 899.984–899.985 Mbit/s loss-free. That is neither
an eight-flow deployment qualification nor proof for the other direction.
The unchanged DHCP baseline still reproduces collapse; one eight-flow run falls
below the envelope. The 805.306368/900-Mbit/s final funding matrix is deferred.

- **A — qualification-tool configuration:** sequence/QPC accounting, batching,
  socket buffers and pacing. The prior larger-send-buffer result remains a
  provisional backpressure mitigation/incidental improvement, not a causal fix
  or deployment requirement. The corrected-condition samples use 64 KiB.
- **B — physical deployment requirement:** none accepted. Static addressing on
  an isolated fiber link is a candidate requiring controlled confirmation on
  both hosts, not a retained change or an Engine requirement.
- **C — diagnostic only, reverted/stopped:** worker active-store static-address
  window, two exact-peer executable/port-scoped rules, ETW/WPR, Npcap captures,
  deliberate receiver pause and probe processes.
- **D — unrelated system issue corrected:** none permanently corrected.

Cleanup verifies original preferred addresses, DHCP enabled, MTU 1500,
10-GbE links, exact NIC advanced/RSS equality, no test listeners/processes/rules,
and stopped trace sessions. Worker-to-workstation bound ICMP passes 3/3,
demonstrating the two-way packet path, and LAN control SSH works. Inbound ICMP
and SSH to the worker fiber address are blocked after test-rule removal. The
worker's network-profile log already records **Public at 08:31:37Z**, before
this task, and Public again after restoration; its existing OpenSSH rule is
Private-only. This is recorded as a pre-existing management-profile limitation,
not silently changed to obtain a connectivity pass.

**Exact next task:** obtain authorized elevated receiver-side Packet Monitor
and kernel scheduling/drop evidence, identify the natural 2,657-packet deficit's
first missing stack boundary, and confirm the DHCP/raw-service condition with
matched NETIO profiles and tightly controlled address configuration on both
hosts. Then establish stable bidirectional generation/reception and rerun the
unchanged funding matrix. The 32-actual-client Local/Node application matrix
remains a later gate; no 3M.

Evidence is in `build/fiber-wfp`, with worker originals under
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-wfp-20260923`. There are **38 completed
paired flow trials across ten trials**, including one deliberate pause control,
plus one failed startup pair retained as unsuccessful evidence. v10/v11 sources,
binaries, per-sequence captures, ETLs, WFP inventories, parsed results and cleanup
are preserved. Earlier broad TCPIP tracing is a bounded circular diagnostic;
absence of events there is not used as complete-window proof. The later narrow
traces and positive control support the specific negative-event findings above.
ETL XML rendered on the other host may display the decoding host's computer
name; source artifact names, wrapper host records, interface IDs and original
trace headers establish provenance. Native/application tests were not rerun
because no production code changed.

`Validate.py` passes all 38 sequence balances, all ten independent capture
comparisons, zero current WouldBlock retries, unchanged receiver UDP InErrors,
the WSH A/B/A counts, positive-control AFD accounting, trace-loss checks,
baseline configuration/cleanup checks and prior archive hashes. `git diff
--check` passes. Probe executable SHA-256 identities are
`d6b3448ed9025f9876d895e06bb1c9c30b77e0f9b746241f8ca4e6ec9040c929` (v10) and
`8ef1e2120b029e125378ef907236f688f6eb8ef2520d8477a7d905e223568dee` (v11).

The new `build/fiber-wfp-20260923.zip` contains **570 manifest-listed evidence
files plus the manifest**, including **27 ETLs**, and is **214,997,066 bytes**.
Every entry was decompressed and SHA-256 checked on the worker; the downloaded
archive hash is independently checked locally:
`235094bc5681a391aa4a3e0961ecc4e50828be14ad9e10729f565393470ac1e4`.
Failed/partial collection attempts and both names for the copied raw-endpoint
ETL are preserved as evidence; their presence is not an additional successful
trial. No raw evidence was discarded. These artifacts remain ignored/local;
the four documentation updates are uncommitted.

## Historical sender profiling verdict (earlier 2026-09-23)

The section below preserves the preceding task's measurements and scope. Its
then-unattributed WFP ownership and next-task statement are superseded above.

**INCONCLUSIVE / PHYSICAL FUNDING NOT QUALIFIED.** The failing worker sender's
CPU cost is now attributed to **Windows Filtering Platform classification/cache
matching in NETIO.SYS**. A bounded eight-flow, eight-datagram-batch method
improves generation substantially, but does not establish a stable bidirectional
generator/receiver across the longer repeats. No Engine defect or physical
fiber ceiling is established.

The final six trials all exceed the **805.306368-Mbit/s arithmetic envelope in
raw received bytes**. That is an arithmetic observation, not a passed methodology
prerequisite: one reverse sender ensemble achieves only 827.352 Mbit/s against
900 requested, and reverse losses range from 0.106167% to 5.934676%. A subsequent
larger-send-buffer comparison still loses data in one of two runs. No acceptable
loss threshold, lower requested rate, revised profile or permanent tuning is
introduced to turn these results into a qualification pass.

**KI-006 OPEN; Foundation 3L B — PARTIALLY READY.** No actual application clients,
production networking edits, merge or Foundation 3M work occurred. Published
HEAD remains `4d27238553fdc27b9f6772b438c49393eac3a8eb`, production-equivalent
to `eec0c7123a39698761e49ed276ee37aae673e023`. The existing isolated worktree
continues to preserve unrelated edits in the original checkout. The prior
418-file diagnostic archive was reused and its SHA-256 verified unchanged.

### Unchanged path and instrumentation

All traffic explicitly binds the direct Mellanox ConnectX-4 Lx fiber:
**7950X3D / Ethernet 3 / 169.254.117.214 ↔
5900X / Ethernet 4 / 169.254.173.205**, 10 GbE, MTU 1500. The earlier TCP,
packet-size, RSS and moderation investigations were not repeated.
Driver 2.53.23539.0, firmware, offloads, RSS topology, flow control, moderation,
MTU and Engine profile were unchanged. Worker temperature was 55°C before and
54°C at both cleanup readings.
Local temperature remained unavailable without elevation.

Existing Windows tools were used: WPR **10.0.26100.9444** and xperf
**10.0.26100.7705**, with WPA-compatible ETLs. CPU sampling, a scheduling-only
profile, and a combined CPU/CSwitch/ReadyThread/DPC/ISR profile were collected
in memory. Trace files were flushed after each captured trial. Initial built-in
CPU traces do not contain DPC/ISR events; the custom profiles do. The clean and
failing comparison traces report **zero lost ETW events/buffers**.
OS symbols came from Microsoft's symbol service and are cached under
`C:\Sandbox\Codex\Cache`. A broad online symbol pass was stopped after the
required symbols arrived, then decoding completed from the local cache.

### Clean versus failing sender

The decisive comparison uses the **same v3 binary**, one connected nonblocking
UDP socket, 1,300-byte payload, 64-KiB send / 256-KiB receive buffers,
900-Mbit/s request and eight-second target. Periodic profiling did not reliably
reproduce the failure at startup, so a combined recording was attached after
sender kernel CPU had already accumulated. That captured the slow path; the
earlier hypothesis that profiling necessarily removes the problem is rejected.

| Measurement | Clean `profile-reverse-2` | Failing `attach-reverse-1` |
| --- | ---: | ---: |
| Actual send Mbit/s / datagrams/s | 900.000 / 86,539 | 152.052 / 14,620 |
| Received Mbit/s / datagrams/s | 899.987 / 86,537 | 152.050 / 14,620 |
| Submitted / received datagrams | 692,307 / 692,307 | 146,205 / 146,205 |
| Send interval | 8.000 s | 10.000 s; generation bound |
| Time inside send calls | 2.674 s | 9.990 s |
| Mean send-call duration | 3.863 µs | 68.328 µs |
| Process user / kernel CPU, including startup | 4.906 / 2.688 s | 0.156 / 9.469 s |
| WouldBlock retries / fatal send errors | 0 / 0 | 0 / 0 |
| NETIO share of sampled process CPU | 2.856% | 66.398% |
| Mellanox driver share of sampled process CPU | 2.523% | 0.756% |

The failing sender spends only about **0.010 s outside send calls** during its
10-second send interval. This is CPU-heavy synchronous work inside a
nonblocking send operation, rather than an intentional pacing sleep or an
application blocked waiting for socket space. CSwitch accounting records
7.665 s on the main sender thread during the partial late-attach trace;
scheduler waits/DPC/ISR cannot explain away the independently measured 9.469 s
of whole-process kernel CPU.

Resolved failing samples are dominated by **NETIO!FindCacheMatch**
(3.815 sampled CPU seconds, 45.787% of process sample weight), followed by
FilterMatchEx, memcmp, CompareSecurityContexts, MatchValues, FindMatchingEntries
and IndexTrieClassify. The stack report and flat samples include Winsock send,
AFD, UDP/IP and WFP paths. NETIO file version is **10.0.26100.9444**.
This locates the expensive stage; it does **not** identify the triggering filter,
callout, cache state or OS defect. The post-cleanup, read-only WFP filter snapshot
is context, not proof of which rule caused the hot path. No firewall protection
or security software was disabled.

The other single-flow trials remain in `single-summary.json`, with both
actual Mbit/s and datagrams/s. Twelve paired trials include the startup CPU
captures, scheduling trace, three combined repeats, unprofiled failure and batch
pilots. The unprofiled v3 repeat produces only 171.925 Mbit/s / 16,531 datagrams/s.
Instrumentation v4 adds per-call latency distributions; clean calls have
approximately 3–4-µs median and 4.6–9.7-µs p99 in those captured runs.
Two otherwise target-rate traces lose 256 and 43 datagrams; they are not silently
classified as clean receiver evidence.

### Qualification-only generator and receiver

The retained purpose-specific C# probe adds normal supported
[UDP send segmentation](https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-wsasetudpsendmessagesize)
through the documented UDP_SEND_MSG_SIZE socket option. It submits up to eight
separately numbered **1,300-byte datagrams** per send call. On-wire datagram size
remains MTU-compatible; no NIC offload property or jumbo-frame setting changes.

Batching alone is insufficient: its single-flow reverse pilot delivers
899.990 Mbit/s / 86,537 datagrams/s without loss, while forward generates only
216.828 Mbit/s / 20,849 datagrams/s. The latter's batch-send median/p99 are
373.5/479.5 µs, with 5.984 kernel CPU seconds in a six-second bound.
That local failure was not independently kernel-profiled; its specific WFP
attribution must not be inferred solely from the worker trace.

The next bounded candidate uses **eight independent flow processes per endpoint**,
eight datagrams per send, per-flow QPC pacing and staggered phase starts.
Each receiver has a **4-MiB effective receive buffer**. Send buffers initially
remain **64 KiB**. Eight flows reduce each flow's packet/call rate and distribute
send-path CPU work; the flows are benchmark workers, not application peers.
A 900-Mbit/s aggregate request is **86,538.462 datagrams/s**, approximately
10,817.308 per flow and 10,817.308 successful batch sends/s in aggregate.
At 750 and 805.306368 Mbit/s the corresponding packet rates are
72,115.385 and 77,433.305 datagrams/s.

Each packet contains its flow token, sequence and sender timestamp. Reliable
TCP metadata reports exact submitted counts; the receiver records unique
sequences, missing positions, duplicates, reordering and adjacent transit
variation. All completed pairs reconcile counts. Final rates use the full union
of sender QPC intervals and receiver QPC intervals, independently on each host;
conservative received rate uses the largest of those spans and the requested
duration. Clock synchronization is not required. Aggregate Mbps is not a sum
of potentially non-overlapping per-flow rates.

Payloads and receive buffers are generated/handled in memory. The packet
generation and receive loops perform **no file playback or disk I/O**.
Per-flow partial/final JSON and console output occur after that flow's measured
send/receive loop and can overlap another flow's tail/cleanup; no claim of zero
whole-process disk activity is made. Disk is not the payload source, and storage
was not benchmarked. The final runs contain no concurrent trace decoding,
symbol download, file transfer or profiling.

### Repeatability before the longer gate

Each cell gives the two four-second repeats. Values are measured aggregate
generation; exact per-flow receive counts/rates, CPU, counters and jitter are
retained in `flow-summary.json` and paired results. These early versions did not
retain absolute receiver timestamps across flows, so aggregate receive-union
goodput is deliberately unavailable for them. The final version adds those
timestamps without changing the packet-generation method.

| Request / direction | Actual generated Mbit/s | Actual datagrams/s | Missing datagrams |
| --- | --- | --- | --- |
| 750 / forward | 750.001 / 750.018 | 72,115 / 72,117 | 0 / 0 |
| 750 / reverse | 750.016 / 747.274 | 72,117 / 71,853 | 76 / 0 |
| 805.306368 / forward | 802.495 / 805.196 | 77,163 / 77,423 | 0 / 0 |
| 805.306368 / reverse | 805.383 / 805.282 | 77,441 / 77,431 | 0 / 0 |
| 900 / forward | 899.887 / 896.930 | 86,528 / 86,243 | 0 / 0 |
| 900 / reverse | 900.099 / 900.092 | 86,548 / 86,547 | 0 / 0 |

All flows complete intended packet counts in these short repeats. Aggregate
start skew lowers some measured rates; nominal 805.306368 is not asserted where
802.495 was actually generated. The 76 missing packets occur around sequence
2515–2527 across flows, not exclusively at startup/end. An earlier unstaggered
reverse pilot misses 143 packets; both pilots remain in the archive.
The successful short headroom repeats justified a separate longer test, not an
acceptance claim.

### Final eight-second funding repeats

Same method both directions; 900 Mbit/s requested, 1,300-byte payload,
eight flows, eight-datagram batches, 4-MiB receive / 64-KiB send buffers.
Each trial submits **692,304 datagrams / 899,995,200 bytes**, in **86,544
successful send calls**. Jitter is the largest per-flow maximum EWMA adjacent
transit variation, not absolute one-way delay.

| Trial | Generated Mbit/s / datagrams/s | Received Mbit/s / datagrams/s | Missing / loss | Max EWMA jitter |
| --- | --- | --- | --- | --- |
| Forward 1 | 900.009 / 86,539 | 899.995 / 86,538 | 0 / 0% | 0.071 ms |
| Forward 2 | 899.996 / 86,538 | 899.995 / 86,538 | 0 / 0% | 0.055 ms |
| Forward 3 | 900.005 / 86,539 | 899.995 / 86,538 | 0 / 0% | 0.041 ms |
| Reverse 1 | 827.352 / 79,553 | 826.471 / 79,468 | 735 / 0.106167% | 1.103 ms |
| Reverse 2 | 899.999 / 86,538 | 846.523 / 81,396 | 41,086 / 5.934676% | 2.639 ms |
| Reverse 3 | 899.989 / 86,537 | 898.097 / 86,356 | 1,274 / 0.184023% | 1.157 ms |

All six have zero invalid/duplicate/reordered datagrams and zero NIC
transmit/receive error/discard deltas. Each forward trial has 692,384 NIC TX and
RX unicast packets; each reverse trial has 692,368, matching data plus control
traffic. Reverse 2 receives only **651,218 application datagrams** despite
those NIC counts. Global UDP InErrors also stays unchanged across every final
flow; overlapping global counters must not be summed. The receive deficit is
above, or unaccounted for by, the NIC counters; its precise WFP/socket/tool layer
is **unresolved**, and no physical wire-loss claim follows.

Reverse 1's eight flows take approximately 8.65–8.70 seconds to generate their
eight-second target counts, spending **69.398 aggregate seconds in send calls**
and **68.469 aggregate kernel CPU seconds**. Batch-send median is 792–804 µs;
worst per-flow p99 is 967 µs. Thus simply reaching the intended count does not
mean the target rate was generated.

Reverse 2 adds a distinct backpressure condition: **44,459 WouldBlock retries**,
in addition to 86,544 successful batch sends. These retries are recorded,
not counted as transmitted bytes. It has 61.063 aggregate kernel CPU seconds.
Reverse 3 reaches target with no retries and only 0.496 aggregate seconds inside
send calls, yet still loses packets. Neither sender kernel cost nor send-buffer
pressure alone explains all receiver deficits.

Final whole-host wrapper CPU means are 23.53–25.75% local / 7.53–10.47% worker
forward, and 10.23–13.79% local / 24.63–28.19% worker reverse. Sender cores reach
100%. Receiver maximum sampled core DPC is 18–30% on the worker and 24–30% on
the workstation; ISR maxima are 6–12% on the worker and 11–12% on the workstation.
These sparse samples include startup/drain and do not prove unlimited headroom.
The benchmark uses up to eight pacing CPU cores; this is not an Engine CPU
budget or evidence for eight-way application send concurrency.

### Send-buffer follow-up and disposition

The observed WouldBlock condition justifies one process-local send-buffer
comparison: **4 MiB effective send and receive**, otherwise the same eight-flow
method, two reverse eight-second 900-Mbit/s trials. They generate
**899.987 / 899.993 Mbit/s** (86,537 / 86,538 datagrams/s), receive
**897.960 / 899.978 Mbit/s** (86,342 / 86,536 datagrams/s), with
**1,267 / 0 missing datagrams** (0.183012% / 0%). Both have zero WouldBlock
retries. This does not prove a general buffer fix: one still loses data,
and intermittent WFP behavior was not held constant across the comparison.
There was no second funding promotion from the clean sample.

Configuration classifications for this task:

- **A — qualification-tool configuration only:** flow count, UDP segmentation
  request, pacing phases, socket buffers and accounting. These are retained as
  candidate diagnostic source, **not a qualified deployment configuration**.
- **B — required physical deployment configuration:** none established.
- **C — diagnostic only:** WPR/ETW recordings, symbol decoding and two exact-IP,
  executable/port-scoped worker firewall rules. Recordings stopped and rules
  removed. No NIC, firmware, offload, RSS, moderation, MTU or OS policy change
  needs restoration because none was made.

Baseline/cleanup comparisons verify exact advanced-property and available
RSS-field equality, intended Preferred addresses and on-link routes, 10-GbE
Up state, MTU 1500, no test listeners/endpoints or remaining probe/decoder
processes. WPR reports not recording. All test processes ran hidden.
The final worker cleanup receipt is timestamped **2026-09-23T09:24:24Z**.
Qualification source and evidence remain in ignored build artifacts; no
unrelated workloads were stopped.

The unchanged arithmetic remains **96 MiB/s = 805.306368 Mbit/s**, including
all transport reserve and slack, four grants and the accepted per-grant drain
floor. Although raw final goodput exceeds that number, the task's stable
generator/receiver prerequisite failed longer repetition. No stable usable
service floor or physical deployment profile is certified. Highest measured
generation in this task is about 900.1 Mbit/s; highest conservatively measured
final received rate is **899.995 Mbit/s**. Earlier approximately 1-Gbit/s samples
retain their historical short-trial scope.

**Exact next task:** identify the WFP filter/cache condition driving the hot
send-classification path and localize the reverse receive deficits with scoped
WFP/drop evidence, then establish a stable method and repeat the unchanged gate.
The 32-actual-client Local/Node matrix remains outstanding and must not start
from this receipt. No production Engine workaround or 3M.

Evidence lives in `build/fiber-profiling`; worker originals are under
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-profiling-20260923`.
The source/versions, 12 single-flow paired trials, 22 eight-flow trials
(**188 paired flow trials** total), ETLs, decoded profiles/stacks, exact
counters and cleanup are retained. v3/v4/v6/v7/v8 and final v9 source snapshots
are available; the intermediate v5 batch pilot has its binary plus the next
source snapshot, not a contemporaneous v5 source snapshot. Final v8 binary SHA-256:
`2b13914dce84becd931b409730cccd9f263c81079771e6b6ceb03c9ef570ffa4`;
v9 send-buffer experiment:
`048f035382ef3a9f81211621bfb6020eedae234c4a14fbd9c96d105334349275`.
Validation reconciles counts, rates, exact final losses/retries and baseline
cleanup, and preserves the prior archive. Native/application tests were not
rerun: no production source changed.

The separate `build/fiber-profiling-20260923.zip` archive contains **1,476
manifest-verified files**, including **six ETLs**, and is **140,413,892 bytes**.
Every entry was decompressed and SHA-256 verified on the worker; the downloaded
local archive matches SHA-256
`a49d51d931be1c1df56658782f65933ab97f3bbc388b1dab257eb8fb2f6618b8`.
NGENPDB directories and shared symbol caches remain on the worker outside this
archive; resolved stack reports and module identities are included. The archive
receipt is `build/fiber-profiling-archive-receipt.json`. The earlier 418-file
diagnostic archive remains unchanged.

## Historical diagnostic resumption (earlier 2026-09-23)

This section and the initial receipt below retain their original measurements.
Their then-current verdict/next-task statements are superseded by the profiling
receipt above.

**STOP / PHYSICAL FUNDING NOT ESTABLISHED.** A bounded rate sweep delivered
approximately 1 Gbit/s without missing datagrams in each direction, proving that
the earlier 651.7-Mbit/s result is not an established fiber ceiling. However,
the final repeat preflight did not reproduce stable bidirectional generation
and delivery. Reverse send calls consumed nearly all of a 10-second generation
bound at only 172.6–179.2 Mbit/s; one forward repeat lost 0.738690%.
No trustworthy sustained minimum of **805.306368 Mbit/s** is established.

The investigation establishes multiple contributors: ctsTraffic frame scheduling
and playback sensitivity, burst/socket-buffer sensitivity, high receive DPC
load, intermittent expensive sender socket calls, and receive discards under
the aggressive moderation experiment. It does **not** prove a single underlying
driver/firmware/Windows defect or identify the exact drop layer for every trial.
There is no evidence sufficient to label this physical fiber loss or an
application-owned defect. The stop condition is inability to establish
repeatable target-rate UDP generation with the available bounded methods.

**KI-006 stays OPEN; Foundation 3L stays B — PARTIALLY READY.** Application
qualification remains unmeasured. No production code, service contract, accepted
margin, application binary, merge or Foundation 3M work changed or ran.

### Baseline and exact test path

Source remains published HEAD **4d27238553fdc27b9f6772b438c49393eac3a8eb**,
production-equivalent to **eec0c7123a39698761e49ed276ee37aae673e023**.
The isolated worktree and preserved unrelated checkout are described below.
The same direct Mellanox ConnectX-4 Lx path was used:
DESKTOP-B8V8NAN / Ethernet 3 / 169.254.117.214 to
HOSTPC / Ethernet 4 / 169.254.173.205. Both directions explicitly bind these
addresses, use the on-link route, negotiate **10 GbE**, and retain **MTU 1500**.
No switch, jumbo-frame change or LAN data-path substitution was introduced.

Both drivers report **2.53.23539.0 (2015-04-13)**. Read-only worker MFT reports
firmware **14.29.1016**, release 2020-12-31, PSID MT_2420110034; worker
temperature was **55°C before / 53°C after**. Local firmware/temperature
inspection was unavailable without an administrative token; no UAC was invoked.
Short before/after samples do not establish sustained thermal behavior.

RSS is enabled, with **8 queues / maximum 8 processors**. Worker baseline is
base processor 0, maximum 22, effective indirection over processors 0–14,
ClosestProcessor profile, group/NUMA 0. The worker reports UDP port-based RSS
hash capability false; IP hashing remains exposed, so this does not mean RSS
is disabled. A single flow is not a demonstration of eight-queue balancing.
Local queue count/base are exposed; some detailed processor fields are null
without administration. Raw RSS inventories retain that limitation.

Interrupt moderation is enabled, adaptive RX, **Moderate RX/TX**. Flow control
and IP/TCP/UDP checksum offloads are RX/TX enabled. LSO v2, USO and RSC are
enabled where exposed. NIC receive buffers are 512, send buffers 2048.
Fresh UDP sockets on both hosts read back **65,536 bytes send and receive**.
These socket buffers are distinct from NIC descriptor counts. Complete advanced
properties, routes, CPU topology and counter baselines are in the diagnostic
archive. No offload toggle was justified by checksum/error evidence.

### Tool validation and measurement boundaries

The original tools were **NTTTCP 5.40** and **ctsTraffic 2.0.3.9**, not iperf3.
No iperf3 installation/build was involved. Existing binaries were hash-verified.
NTTTCP was synchronous, one flow, explicit 1,400-byte datagrams and
98,304 KiB/s throttle for the envelope, with 1-second warmup, 5-second measure,
1-second cooldown in this diagnostic. Default-buffer repeat generated only
186.890 Mbit/s; receiver XML reports 187.145 over its different measurement
window. With a requested 4-MiB receive buffer the next run generated 806.010 and
received 805.691 Mbit/s. Both interior CSV sequence intervals have zero missing
packets (83,453 and 359,511 observed respectively). These are observed interior
ranges, not exact whole-trial loss. The changing offered rate prevents attributing
that entire difference solely to the buffer.

A second task-only C#/.NET Framework **UdpProbe** uses connected, explicitly bound
IPv4 UDP sockets, per-datagram QPC pacing/timestamps, unique trial token and
sequence, reliable TCP completion counts, and a bounded receive drain. It checks
length/token/sequence/trailing marker, counts duplicates/reordering, and computes
goodput over the larger sender/receiver span. All completed pairs reconcile
sent = unique received + lost, with no invalid or duplicate datagrams.
CPU/counter wrappers sample both endpoints; no per-packet disk write occurs
during the custom measurement. This is an independent diagnostic, not GNS or
Engine service qualification.

Version 1 produced the early probes; its recorded executable SHA-256 is
`987672f5249d50f2a7365c9823b773ca70eae8988f9309bd170ef122686c963b`,
but its original source and binary snapshots were not preserved.
Version 2 adds a generation deadline and send-call CPU/time accounting; both
source and executable are retained. Version 3 adds optional nonblocking send;
its SHA-256 is
`dfd0ab26e3acdf2b94a6dd358b0dcba89b16de045fa37c75f3eddcd04cc65960`.
Final funding uses v3. The v2/v3 source and executable hashes are archived.
Nonblocking does not imply negligible kernel work: the final reverse send calls
were slow without returning WouldBlock.

Jitter below is maximum 1/16-EWMA adjacent transit variation, measured using
sender/receiver QPC deltas. It is not synchronized absolute one-way delay.
CPU maxima are sparse host-wide core/DPC samples, not per-flow profiling.
Global UDP counters include unrelated host traffic and cannot localize every
drop. UDP payload rates include the diagnostic header, with no IP/Ethernet
overhead; they do not certify application payload throughput.

### Bounded rate and packet-size sweeps

Four-second, 1,400-byte per-packet-paced trials, default 64-KiB receive buffer.
All seven completed rows in each direction have zero missing datagrams;
the failed earlier forward-500 attempt is retained separately.

| Requested Mbit/s | Forward received | Reverse received |
| --- | ---: | ---: |
| 250 | 250.002 | 250.001 |
| 500 | 500.002 | 499.997 |
| 650 | 650.002 | 650.001 |
| 750 | 750.002 | 749.999 |
| 805.306368 | 805.309 | 805.306 |
| 900 | 900.002 | 899.987 |
| 1000 | 1000.001 | 999.994 |

Highest clean **single-trial** delivery is approximately 1 Gbit/s each way
(89,286 / 89,285 packets/s). No loss onset appeared in this rate sweep, but the
later repeats invalidate any claim of a stable 1-Gbit/s floor. Maximum EWMA
jitter in these rate rows spans 0.0158–0.1320 ms forward and
0.0501–0.6257 ms reverse.

Packet-size sweep requests 805.306368 Mbit/s, default buffers, four seconds.
Rates use actual spans, including undergeneration. Near-MTU rows above supply
the 1,400-byte comparison.

| Direction / bytes | Actual send Mbit/s | Received Mbit/s | Lost / loss % | Received packets/s | Max EWMA ms | Receiver max core DPC % |
| --- | ---: | ---: | --- | ---: | ---: | ---: |
| Forward / 256 | 535.944 | 529.941 | 17,585 / 1.119958 | 258,760 | 0.0716 | 98 |
| Forward / 512 | 805.307 | 798.990 | 6,168 / 0.784302 | 195,066 | 0.1270 | 85 |
| Forward / 1024 | 805.307 | 805.307 | 0 / 0 | 98,304 | 0.0180 | 86 |
| Forward / 1400 | 805.309 | 805.309 | 0 / 0 | 71,903 | 0.1320 | See raw samples |
| Reverse / 256 | 674.928 | 674.917 | 20 / 0.001272 | 329,549 | 0.1121 | 53 |
| Reverse / 512 | 805.307 | 805.296 | 0 / 0 | 196,606 | 0.0552 | 41 |
| Reverse / 1024 | 805.308 | 804.072 | 602 / 0.153097 | 98,153 | 0.9624 | 49 |
| Reverse / 1400 | 805.309 | 805.306 | 0 / 0 | 71,902 | 0.6257 | See raw samples |

Both 256-byte trials fail to generate the requested rate: forward hits the
six-second cap; reverse takes 4.773 seconds to send the four-second target count.
Neither is an 805-Mbit/s capacity test. Forward DPC pressure correlates with
packet rate, but reverse 1024 loss versus clean 512 shows timing variation too;
there is no universal monotonic packet-size threshold. Size-sweep NIC
discard/error and global UDP InErrors deltas are zero. Receiver NIC packet counts
are sent counts plus 8–10 control packets while application sequences are
missing, narrowing the deficit to host delivery/observation without proving
socket overflow or excluding a counter-accounting blind spot.

### Controlled experiments and reverse failure attribution

- **Socket burst sensitivity:** at 1,400 bytes / approximately 805 Mbit/s,
  1-ms bursts with 64 KiB receive buffer lose **826 packets (0.287195%)**,
  receiving 803.120 Mbit/s. A paired 4-MiB receive-buffer trial reads back
  4,194,304 bytes and loses zero, receiving 805.453 Mbit/s. Both reach their
  target rate. This supports buffer/burst sensitivity; it does not prove that
  all historical loss had this cause or that a larger buffer alone qualifies
  sustained service. Per-packet 64-KiB and 4-MiB probes both also passed earlier.
- **RSS placement:** high worker DPC samples justify one bounded change from
  base processor 0 to 8. After address readiness, the 256-byte / 500-Mbit/s /
  4-MiB trial generates only **33.763 Mbit/s**, compared with a prior baseline
  500.000-Mbit/s clean trial. This is not a controlled receive-rate comparison
  and shows no demonstrated improvement. Base 0 was restored.
- **Interrupt moderation:** at the same 256-byte / 500-Mbit/s / 4-MiB target,
  nonblocking sender, Moderate repeat generates only **34.365 Mbit/s**.
  Aggressive generates **490.114**, receives **355.712**, and loses
  **267,508 packets (27.392833%)** with **267,511 NIC receive discards**.
  Unequal generated rates prevent isolating the setting's causal effect,
  but the aggressive configuration demonstrably fails delivery and was reverted.
  This is host/NIC receive pressure, not evidence of damaged fiber.
- **ctsTraffic reverse:** the original 72,000-frames/s invocation combined
  approximately one datagram per media frame with playback deadlines.
  Reducing frame scheduling to **1,000 frames/s**, retaining the nominal
  805.306368-Mbit/s offer and 1,400-byte maximum datagrams, produces approximately
  805 Mbit/s and normal completion without error frames or timeout. Each media
  frame is split over multiple UDP datagrams. Default-buffer client completes
  **3,977/4,000 frames**; 4-MiB receive buffer completes **3,983/4,000**.
  Missing frames are not exact packet-loss counts. Thus invocation/frame
  scheduling accounts for a reproducible portion of the reverse tool failure;
  it does not establish reliable delivery. The custom reverse tests also
  undergenerate intermittently, so blaming ctsTraffic alone would be incorrect.
- **Other experiments/failures retained:** the v1 forward-500 run lacks complete
  sender/receiver metadata and is invalid. First RSS trial failed to bind after
  adapter restart; a preferred-address guard was added. First moderation trial
  timed out because wrapper startup was serialized; the paired retries above
  launch concurrently. A counters-only Pktmon burst trial undergenerated at
  194.667 Mbit/s and its counter window ended before trial completion; no capacity
  or comprehensive drop-localization claim uses it. Pktmon is now stopped with
  no filters. Nonblocking/blocking near-MTU comparison produced 805.308 clean
  versus 805.297 with four lost packets, insufficient to prove a mode fix.

The ctsTraffic interpretation is informed by Microsoft's
[tool source](https://github.com/microsoft/ctsTraffic) and
[media-stream implementation](https://github.com/microsoft/ctsTraffic/blob/master/ctsTraffic/ctsIOPatternMediaStream.cpp).
Current upstream source explains frame/playback mechanisms; it is not claimed
to be an exact source reconstruction of the installed 2.0.3.9 binary.
The controlled installed-binary results above are the direct evidence.
Wrong binding, persistent firewall rejection and stale listeners are not supported
as the cause: explicit endpoint receipts, completed client-initiated control/data
handshakes, counter movement and clean repeats verify the tested path.

### Final bounded funding repeat

All NIC settings were restored before these four sequential trials, with no
Pktmon, firmware query or file transfer during measurement. Each requested
**900 Mbit/s for 8 seconds**, v3 nonblocking sender, per-packet QPC pacing,
**1,300-byte UDP payload**, **262,144-byte effective receive buffer**,
**65,536-byte effective send buffer**, and a 10-second generation cap.
The pinned GNS source uses a 1,300-byte default packet limit, nonblocking sockets
and 256-KiB socket buffers. This informed the receive-buffer choice; the probe
keeps its smaller default send buffer and is explicitly not an exact GNS replica.
No conclusion about production GNS behavior follows from it.

| Trial | Actual send Mbit/s | Received Mbit/s | Sent / received packets | Loss % | Jitter p99 / max EWMA ms |
| --- | ---: | ---: | --- | ---: | --- |
| Forward 1 | 900.001 | 900.001 | 692,307 / 692,307 | 0 | 0.0240 / 0.0862 |
| Reverse 1 | 179.164 | 179.164 | 172,273 / 172,273 | 0 | 0.2878 / 0.4693 |
| Forward 2 | 900.001 | 893.353 | 692,307 / 687,193 | 0.738690 | 0.0198 / 0.7611 |
| Reverse 2 | 172.637 | 172.635 | 165,998 / 165,998 | 0 | 0.1857 / 0.4493 |

Both reverse trials hit the generation bound, with **9.991 seconds in send
calls**, approximately **9.875 kernel CPU seconds**, and zero WouldBlock retries.
That locates the immediate rate limit inside sender socket-call execution,
rather than receiver loss or an intended pacing wait. It does not distinguish
Winsock, filtering, driver, virtualization or other kernel cost without a
proper profiler. Zero loss at the undergenerated rate is not an envelope pass.

Forward 2's NIC receives **692,317 unicast packets** versus **692,307 sent data
packets** and **687,193 application receipts**; NIC discards/errors and UDP InErrors
are zero. Global UDP InDatagrams increases 693,207, including other traffic.
Receiver DPC maximum reaches **97% on a sampled core**, with a 6.147-ms maximum
application receive gap. Forward 1 also reaches 94% DPC and is clean.
This supports transient host receive-service pressure but does not alone
identify the precise loss layer. All final NIC transmit/receive error and
discard deltas are zero.

Mean whole-host CPU over wrapper samples is 12.18/14.27% local and
10.57/10.86% worker in forward repeats; 10.43/9.86% local and
13.25/8.44% worker in reverse repeats. Sender cores reach 100%.
Probe working sets stay below 32 MiB in these final wrapper samples.
These averages do not negate individual-core saturation.

The unchanged funding arithmetic is
**(64 structural + 8 gameplay + 4 control/realtime + 8 transport + 12 slack)
MiB/s = 96 MiB/s = 805.306368 Mbit/s**. Four grants, active-grant 16-MiB/s
peer drain floor and all accepted margins remain intact. Although both forward
goodput samples exceed that aggregate number, reverse repeats do not;
therefore aggregate/per-grant physical funding is unestablished. The maximum
trustworthy clean *sample* is about 1 Gbit/s each way, not a demonstrated stable
capacity. The minimum final reverse sample is **172.635 Mbit/s**, which is an
observed generator-limited result, not a physical-link ceiling. No lighter
qualification offer or acceptable-loss threshold was substituted.

### Configuration disposition, cleanup and next task

| Change | Disposition | Final state |
| --- | --- | --- |
| Probe socket buffers, pacing and nonblocking mode; cts frame scheduling | C — diagnostic only | Process-local; all processes exited |
| Worker RSS base 0 → 8 | D — ineffective/inconclusive, reverted | Base 0, max 22, max processors 8, queues 8 |
| Worker RX moderation Moderate → Aggressive | D — delivery failure, reverted | Moderate, registry value 1 |
| Five scoped worker firewall rules | C — diagnostic only | Removed |
| Pktmon filter/counters | C — diagnostic only | Stopped; no filters |
| MTU, driver, firmware, offloads, flow control | Unchanged | Original configuration |

No A permanent requirement or B accepted tuning is proposed; no reboot
persistence assumption is needed. Cleanup at **2026-09-23T08:33:00–01Z** verifies
no test listeners/endpoints or probe processes, exact advanced-property and
available RSS-field equality with both baselines, intended addresses Preferred,
on-link routes, 10-GbE Up state and MTU 1500. No unrelated process was stopped.

The exact next task is a bounded **sender kernel-cost and receive-drop attribution
investigation** on this same baseline path, using a profiler or demonstrably
stable alternative generator, followed by repeat bidirectional funding. An OS
or firmware remediation is not authorized by this evidence alone. Only a later
passed funding gate can precede exact-source builds and the **32-actual-client
Local/Node matrix**. This task stops here; no clients were launched.

Diagnostic artifacts, every failed/setup/undergenerated trial, source/binary
hashes, scripts, paired counters and analysis are under
`build/fiber-diagnostic` in the isolated worktree, with worker originals under
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-diagnostic-20260923`.
The separate `build/fiber-diagnostic-20260923.zip` archive contains **418
manifest-verified files**, is **6,711,581 bytes**, and has SHA-256
`6cd6a2aad9ee87866ca1e0c8b58dae52ff0e0cabff192471eeebc46d63395061`.
Its worker copy under the diagnostic directory has the same hash. The first
archive below remains unchanged. Validation reconciles **35 completed paired
probe trials**, four final funding trials and three invalid client wrappers,
and verifies baseline restoration. Documentation checks pass four files and
50 relative targets, retained initial arithmetic, source equivalence, CI receipts
and whitespace. Native/Engine tests were not rerun for this documentation and
standalone diagnostic change.

## Historical initial fiber receipt (earlier 2026-09-23)

The remaining sections preserve the first attempt, its measurements, source/CI
receipts and cleanup. Its verdict, unattributed causes and next-task statements
are historical and superseded by the diagnostic resumption above.

### Initial verdict

**STOP: the measured UDP service does not fund the accepted profile.** The
direct fiber path is installed and carries verified TCP traffic at
9.246–9.471 Gbit/s. That does not qualify its UDP service. The forward UDP
trial delivers **651.715 Mbit/s** with **19.113758% sequence-accounted loss**
at nominal 96 MiB/s offered load. Reverse UDP has playback-window errors and
a sender-wrapper timeout; exact reverse packet loss and whole-trial unique
packet goodput are **not measured**. No higher UDP load was attempted.

This is a failed funding demonstration on the tested hosts/tools/configuration,
not proof of a hard fiber capacity ceiling or an Engine defect. The loss location
and reverse pacing/playback cause remain unattributed. No arbitrary acceptable
loss threshold, lighter workload or revised service profile was introduced.

**KI-006 OPEN; Foundation 3L B — PARTIALLY READY / NOT READY FOR FINAL ACCEPTANCE.
No physical deployment profile qualified. No merge; no Foundation 3M.**

## Source and build provenance

The requested branch is `foundation/3l-content-availability` in
`gmoddev/gargantuan`. The original checkout was at
`b471b78e78376fb90a50771c1b99e3f8bfe82a4e`, with unrelated morphology/documentation
edits and untracked build/assets. Those were preserved. Fetch established
published HEAD **`4d27238553fdc27b9f6772b438c49393eac3a8eb`**. An initially clean,
isolated detached worktree at that revision owns this receipt and task artifacts.

Only six documentation files differ between that HEAD and the retained production
receipt **`eec0c7123a39698761e49ed276ee37aae673e023`**. No Engine, test, build,
dependency or profile source changed in this task. The previous
[physical receipt](PooledPhysicalQualification3L.md) applies to the September 15
LAN; its deferred-cable statement is historical and is superseded here.

The existing worker build was inventoried, **not executed or accepted as current
qualification binaries**. Its source checkout reports base `417f452a6` with
many overlays, and its binaries have mixed September 9–14 timestamps.
An exact current-source server/client/benchmark rebuild remains necessary before
any future application qualification; filename, timestamp or a partial source
hash match is insufficient provenance. Recorded SHA-256 values:

| Existing executable | SHA-256; inventory only |
| --- | --- |
| `GargantuanServer.exe` | `3da16eedbfd1eecb84e96778bcc0651617c0555cab5cab6180fd5106a5e6043a` |
| `GargantuanPlayer.exe` | `65a6e7b8c1c1bead155255d802602ee4501e07f194a28e3be3d18360cecb7679` |
| `gargantuan_game_session_benchmark.exe` | `cbb8e0bae5899a64af87b6c4ccf80090b4ab972af9cf49d3f5f6b4c4c0dca461` |
| `gargantuan_game_session_real_transport_tests.exe` | `f6ad3abc341eb4d67b5d4486bd477165c9178f5dc41ca53be2b42d2add4ed3df` |

They reside in the worker's
`C:\Sandbox\Codex\Builds\gargantuan\runtime-host-f1-1-baseline2-msvc-gns-vs\Release`.
The cache records Visual Studio 17 2022, MSVC **19.44.35228.0**, x64-windows,
Release outputs, `GARGANTUAN_WITH_GNS=ON`, BCrypt and vcpkg dependencies.
Canonical [GNS configuration](../../cmake/GameNetworkingSockets.cmake) pins
**`2cb93a06350bb065db53abdb0d87cf297e0bfd34`**, with the existing service-fairness
and exact reliable-feedback patches. Cache and source-hash receipts are retained;
this inspection does not certify the old binaries' full dependency closure.

Unchanged exact-source production [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/34900780304)
and [GNS CI](https://github.com/gmoddev/gargantuan/actions/runs/34900779755)
are terminal success. Published HEAD also has terminal-success
[Native CI](https://github.com/gmoddev/gargantuan/actions/runs/35025874973) and
[GNS CI](https://github.com/gmoddev/gargantuan/actions/runs/35025875129), rechecked
through GitHub CLI. Unaffected simulator, sanitizer, native feedback, recovery
and pooled-service evidence retains its existing scope. No native suite was rerun.

## Physical path

The operator confirms a **direct cable between the two Mellanox cards**, with
no intermediate switch. The user describes the medium as fiber. Exact cable,
optic/transceiver manufacturer/model, wavelength and length are **not measured**;
Windows exposes the media as 802.3. No 25-Gbit/s claim is made.

| Inventory | Workstation / intended client generator | Worker / intended server |
| --- | --- | --- |
| Host | DESKTOP-B8V8NAN | HOSTPC (`dockerbox` control alias) |
| CPU | Ryzen 9 7950X3D; 16 cores / 32 logical | Ryzen 9 5900X; 12 cores / 24 logical |
| RAM bytes | 33,157,488,640 | 34,282,344,448 |
| Free memory at inventory, KiB | 15,114,232 | 14,444,356 |
| OS | Windows 11 Pro, build 26200 | Windows 11 Pro, build 26200 |
| NIC | Mellanox ConnectX-4 Lx Ethernet Adapter #2 | Mellanox ConnectX-4 Lx Ethernet Adapter |
| Driver version / reported date | 2.53.23539.0 / 2015-04-13 | 2.53.23539.0 / 2015-04-13 |
| Interface / index | Ethernet 3 / 50 | Ethernet 4 / 19 |
| Qualification IPv4 | 169.254.117.214/16 | 169.254.173.205/16 |
| Negotiated link / IPv4 MTU | 10 Gbit/s / 1500 | 10 Gbit/s / 1500 |

Both routes select the stated interface and on-link `169.254.0.0/16`, next hop
`0.0.0.0`. Tools bind these addresses explicitly; TCP connection receipts show
both fiber endpoints. Per-trial Mellanox byte counters increase. The forward UDP
sender records 1,867,952,806 additional sent NIC bytes and receiver records
1,861,220,108 additional received NIC bytes over their wrapper windows. These
include overhead and startup/drain and are not application goodput. The receiver
also records 4,669 NIC receive discards and zero receive-error increment, which
does not explain all 206,148 missing application sequence numbers. Control SSH
continues over the separate LAN. No route, address, NIC or driver was changed.

Both NICs report RSS enabled with eight receive queues / eight maximum processors,
512 receive buffers, RX/TX flow control and IPv4/TCP/UDP checksum offloads enabled,
UDP segmentation offload enabled, adaptive receive interrupt moderation with
moderate RX/TX profiles, and Jumbo Packet 1514. Complete advanced-property,
LSO, RSS and checksum inventories are retained. The hosts are shared; measured
free RAM is neither a reservation nor proof of client-generator headroom.

## Bounded TCP and UDP measurements

Tools are the retained Microsoft ctsTraffic **2.0.3.9** and NTTTCP **5.40**.
Both endpoint copies were hash-verified before testing against the prior receipt:

- ctsTraffic: `0548089e59c872306ce2c98e7163e2a717119756010cf64d3cb3da2854f632cf`.
- NTTTCP: `f66561d09af91305412fd60ca4b28d57c7b650035d3c1edcc00a57b079e2247e`.

TCP uses four simultaneous verified-data connections, each 536,870,912 bytes,
2,147,483,648 application bytes per trial. Each direction has three sequential
trials. ctsTraffic push is workstation to worker, pull is worker to workstation.
Offered TCP rate is unpaced and finite-byte bounded. Reported received rate uses
the receiver's first connection-established to last connection-success timestamp;
listener startup and control bytes are excluded. Every trial has four successful
connections, zero network/protocol errors and zero wrapper timeouts.

| Direction | Trial | Receiver interval, s | Application Mbit/s |
| --- | ---: | ---: | ---: |
| Workstation to worker | 1 | 1.818 | 9449.873 |
| Workstation to worker | 2 | 1.854 | 9266.380 |
| Workstation to worker | 3 | 1.858 | 9246.431 |
| Worker to workstation | 1 | 1.826 | 9408.472 |
| Worker to workstation | 2 | 1.818 | 9449.873 |
| Worker to workstation | 3 | 1.814 | 9470.711 |

Forward UDP uses one NTTTCP stream, 1,400-byte datagrams, nominal **96 MiB/s**
(805.306368 Mbit/s), 15 seconds measurement, two seconds warmup and one second
cooldown. Its KiB/s pacing truncates to 100,663 bytes/ms. Sender XML reports
805.130 Mbit/s; receiver XML reports 651.597 Mbit/s over 15.013549 seconds.
The independent interior-sequence analysis, using the observed receive span,
reports:

| Forward UDP measurement | Result |
| --- | ---: |
| Expected sequence interval | 143,877–1,222,408: 1,078,532 packets |
| Unique received / missing | 872,384 / 206,148 |
| Loss in bounded interior interval | 19.113758% |
| Observed receive span | 14.992284 s |
| Received application rate | 651.715 Mbit/s |
| Transit variation p50 / p95 / p99 | 0.0026 / 0.0077 / 0.1026 ms |
| Maximum transit variation / maximum EWMA jitter | 9.5243 / 0.627967 ms |
| Maximum observed receive gap | 16.3981 ms |
| Logged duplicates / reordered arrivals | 0 / 0 |

Loss uses unique sequence identities between observed endpoints after warmup;
trailing packets outside that interval are excluded. Jitter is absolute change
in relative transit time, with a 1/16 EWMA, and does not require synchronized
clock offsets. Good jitter percentiles do not cancel loss.

Reverse UDP uses one ctsTraffic stream, nominal 96 MiB/s, 72,000 frames/s,
15-second configured stream and the default one-second playback buffer.
Integer sizing yields 1,398-byte frames, one datagram each. Of 1,080,000 expected
frames it completes only 19,527, reports 1,060,473 missing completed frames and
226,886 error frames. The receive byte counter is 344,485,374; ctsTraffic reports
172.264 Mbit/s for the connection. Neither counter supplies unique packet
goodput in this error condition. Missing playback frames are **not** exact
network loss. The client exits zero despite these errors; the worker is stopped
by its 45-second wrapper deadline (45.697 seconds observed).

Reverse whole-trial packet loss, usable throughput and arrival-order jitter are
**not measured**. The surviving completed-sequence subset has transit-variation
p99 0.2721 ms and maximum EWMA 0.217697 ms; it covers only 1.277 seconds and
cannot characterize the full trial. No clean repeat is substituted for the
failure. No 104-MiB/s escalation or further saturation was performed.

Post-load baseline ICMP sends 100 sequential 1,200-byte DF echoes each way,
500-ms timeout, 50-ms spacing. Both directions return **100/100**, with all RTT
samples below the integer-millisecond API resolution (reported zero).
DF payload 1,472 succeeds and 1,473 returns `PacketTooBig` both ways, consistent
with MTU 1500. Loaded RTT and absolute one-way delay are **not measured**.

## Funding and resource disposition

The unchanged [Option C profile](PooledReliableServiceProof3L.md#selected-32-peer-candidate)
requires **100,663,296 B/s = 96 MiB/s = 805.306368 Mbit/s** usable envelope:
64 MiB/s structural, 8 gameplay, 4 control/realtime, 8 transport reserve,
plus 12 MiB/s unallocated slack. Four grants and the 16-MiB/s active-grant peer
drain floor remain unchanged. The negotiated 10-Gbit/s line gives arithmetic
space outside that envelope, but cannot replace a measured usable-service floor.
Forward measured UDP is **153.591 Mbit/s below** the envelope; reverse has no
trustworthy usable minimum. Aggregate and per-grant physical funding are not
established. No extra numerical margin or loss gate was invented.

TCP wrapper samples show host CPU means 4.00–23.67% workstation and 5.20–11.00%
worker, with maximum observed tool RSS 12,972,032 / 11,767,808 bytes. Samples
include startup and are sparse over short transfers. Forward NTTTCP reports
9.041% / 5.871% host CPU; its RSS was **not measured**. Reverse ctsTraffic host
means are 8.43% / 11.95%, maximum observed RSS 20,721,664 / 11,358,208 bytes.
The worker reaches a sampled core at 100%, but that is not causal attribution.
One network-tool process ran per endpoint, with wrappers; trials were sequential.

The limiting demonstrated metric is UDP application delivery. Whether the
bottleneck is receiver servicing, pacing, socket buffering, scheduling, driver
behavior or another cause is **not measured**. No CPU, memory or physical spare
capacity claim follows from aggregate CPU or link utilization.

## Deferred actual-client matrix and cleanup

The preflight stop precedes harness implementation, builds and application runs.
Actual GameSession clients / Gargantuan servers launched: **0 / 0** for Local
and **0 / 0** for Node. For both providers the following are **not measured**:

- 32-client connectivity, health, attribution and per-peer fairness;
- Character/root due publications, observations, service latency and fanout;
- RPC p50/p95/p99/max, Event ACK/gaps, action results and ordinary service;
- physical Engine rates, scheduler/admission rejections, pending state, exact
  structural retirement, debt conservation, grants and slow-peer containment;
- initial load, eviction/reload counts and fresh identities, planning/selection/
  journal/Known bounds, Name coalescing, service recovery and convergence;
- server/client CPU, RSS, service/tick cost, process count and resource headroom;
- application connections, pooled debt, readers, content/cache/residency cleanup
  and residual application RSS.

The [canonical workload](RecipientServiceWorkload3L.md) and
[reconciled recovery contract](PooledReliableServiceRecoveryContract3L.md) remain
unchanged. The one-client-plus-observers fixture is not actual-client evidence.
The historical 200-peer diagnostic remains separate from this 32-client gate.

Four temporary inbound worker firewall rules were scoped to the exact fiber
addresses, tool executables and TCP/UDP ports, with names
`Codex-3L-Fiber-20260923-{Cts-TCP,Cts-UDP,Ntt-TCP,Ntt-UDP}`. They were removed.
Cleanup receipts at **2026-09-23T07:49:53Z** establish no remaining task rules,
ctsTraffic/NTTTCP/Gargantuan processes, TCP listeners or UDP endpoints on test
ports on either host. No local inbound rule, global firewall change, UAC window,
driver/MTU tuning, unrelated process termination or persistent test service was
used. Scripts were run with process-local execution-policy bypass on the worker;
machine policy was not modified. Logs/tools are intentionally retained.

## Evidence and next permitted task

Raw paired tool receipts, inventories, per-trial Mellanox counters, CSV sequences,
analyzers, hashes, CI snapshots and cleanup live under
`C:\Users\aiden\.codex\worktrees\foundation-3l-fiber\gargantuan-main\build\fiber`.
Worker originals live under
`C:\Sandbox\Codex\Logs\gargantuan-3l-fiber-20260923`.
The archive `fiber-preflight-20260923.zip` contains **121 manifest-verified
files** and is 14,946,069 bytes. Its SHA-256 is
`2c6c9be665f2dd97de2482c7954893ddd780754a543d8c06cc7bf056b9b35422`.
The manifest and archive preserve unsuccessful results as well as successful
TCP trials. These are local artifacts, not permanent public hosting.

Validation passes all four edited documents, 50 relative file targets, six TCP
receipt rows, forward sequence/rate/loss arithmetic, reverse unavailable-value
semantics, two cleanup receipts, four terminal-success CI snapshots, production
source equivalence and `git diff --check` (including a separate whitespace check
of the new receipt). Astro was not rerun: no site input changed, and these
engineering Markdown receipts are not generated Astro pages. No new native or
application qualification is implied by documentation validation.

Final Foundation 3L acceptance sweep: **not performed**, because KI-006 cannot
close. The exact next permitted task is to attribute and resolve the UDP
delivery/pacing/playback failures on this same direct path, then repeat bounded
bidirectional preflight against the unchanged profile. Only after that passes,
establish exact-source application binaries, implement the bounded missing
32-actual-GameSession-client harness and qualify both Local and Node with the
accepted workload and all service/resource/cleanup gates. No 3M.
