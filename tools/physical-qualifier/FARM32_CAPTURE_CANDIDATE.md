# Foundation 3L Farm32 capture candidate

## Version 2 status

**Source candidate only: `Farm32Capture16GiB-v2`. Not installed, staged, or
operationally qualified by this change. Do not launch a physical campaign
until the near-capacity qualification below passes.**

This replaces the initial 1 GiB Farm32 *source profile* with bounded,
single-file 16 GiB recording and strict **less than 15 GiB** acceptance for
each ETL and pcapng. Both ownership markers and sealed capture indices bind
the profile identity. A stale or mixed profile cannot publish capture
readiness or qualify offline. Existing capture formats retain version 1;
`Profile` identifies the storage/runtime candidate.

No physical evidence, installed hook, service, managed runtime, published
tag, or `upstream.lock.json` deployment pin is changed. The pinned generic
Agent Coordinator Farm32 boundary remains a 600-second live lease and a
60-second privileged hook deadline. File size and offline conversion belong
to the Gargantuan adapter, so no upstream service change is needed.

## Capacity decision and limits

The previous 1024 MiB hard cap / 960 MiB completeness threshold is insufficient
for the corrected full workload. Retained failed local diagnostic evidence
already proves at least 982,728,665 application bytes before the mixed case.
Using maximum 1472-byte UDP payloads, Ethernet/IP/UDP headers and pcapng packet
blocks gives a conservative projected pcap lower bound of 1,032,132,175 bytes,
above 960 MiB. This is a source/log projection, not physical capture evidence.

The source permits 480 opportunities x 16 Names x 24 KiB x 32 peers in each
of structural and mixed cases: 12,079,595,520 raw Name fanout bytes. Adding
the defined gameplay/mixed echoes and small recovery echoes gives a known
producer basis of 13,114,500,160 bytes (12.21383 GiB). This excludes other
traffic, framing, retransmissions and ETW overhead; coalescing can reduce
actual output. Neither this basis nor the 96 MiB/s queue-funding parameter
proves a worst-case wire/storage ceiling.

**16 GiB is an operational candidate, not guaranteed coverage of every legal
workload.** Capacity exhaustion, missing packets, truncation, or recording
ending before the role completes fails closed. Do not rotate, overwrite,
drop selected packets, shorten the workload, or infer PASS from a file below
the cap. All 32 nonce-bound tuples must remain bidirectional at both endpoints.

| Boundary | Version 2 value | Meaning |
| --- | --- | --- |
| Worker netsh `maxSize` | 16384 MiB | Single ETL, `fileMode=single`, no wrap |
| Client dumpcap `filesize` | 16777216 KiB | Single pcapng, no ring |
| Every ETL/pcap completeness | strictly < 15 GiB | Reject the threshold itself |
| Worker free disk at Start | >= 34 GiB | 16 GiB ETL + 16 GiB export + 2 GiB headroom |
| Worker free disk at Finalize | >= 17 GiB | 16 GiB export + 1 GiB, in addition to retained ETL |
| Client free disk at Start | >= 18 GiB | 16 GiB pcap + 2 GiB headroom |
| Privileged capture lease | 600 s | Unchanged live bound |
| Privileged hook / controller Stop | 60 s / 80 s | Unchanged service / enclosing request bounds |
| Role receipt wait after capture start | 500 s | Unchanged; leaves Stop room before lease |
| Baseline / recovery role runtime | 300 s / 420 s | Unchanged |
| Client autostop / close bound | 600 s / 630 s | Unchanged |
| Offline Finalize | 1800 s | Candidate budget; trace must already be idle |
| Role capture finish | 3000 s | 500 + 80 + 10 status + 1800 + 610 hash/receipt guard |
| Outer endpoint / controller | 3050 s | Capture finish + 50 s cleanup guard |
| Capture member transfer | 1800 s per member | At most 20 indexed files; strict size/hash checks |

The offline budgets are finite engineering candidate budgets, not service
constants or proven completion maxima. They require empirical qualification.
Longer offline conversion, hashing and transfer do not extend any live gate.
Disk checks are fresh minimum free-space checks, not an OS storage reservation;
later storage exhaustion remains an incomplete-capture failure.

## Implementation and evidence integrity

`worker/PktMonFarm32Capture.ps1` retains the pinned Mellanox miniport identity,
physical NDIS layer, IPv4/UDP scope and complete Ethernet frame export.
`DumpcapFarm32Capture.ps1` retains the exact client fiber identity and
`udp port 39450 and host 10.253.3.2` filter. Neither broadens capture or changes
snap length, packet direction, or zero-loss acceptance.

Worker Stop closes only its owned trace. Offline Finalize requires idle capture,
zero lost ETW events, a fresh output and sufficient free space. Both exporters
reject the packet that would reach 15 GiB before writing it. An oversized
pending export is not published. The fast exporter reads one EventLog record
and writes one packet at a time, with 64-bit stream offsets and packet-local
32-bit block lengths. Its versioned type cannot reuse an older loaded exporter.
The actual EventLogReader assembly is referenced on PowerShell 7 as well as
Windows PowerShell 5. Offline Python hashing and pcapng parsing remain streaming.

`farm_capture_campaign.py` retains `SEALED_UNQUALIFIED` receipts. Sealing,
collection, nonce/port reconciliation, loss checks and direction analysis
remain separate gates. The prior service/root and one/four-client
`worker/PktMonCapture.ps1` are untouched. Historical 1 GiB receipts and the
920 MiB synthetic result describe their original profile; they do not qualify
version 2.

## Required qualification before staging a physical candidate

1. Build/hash the versioned source and verify the exact installed service
   executable **and managed DLL/runtime**, helper, hook and client dumpcap
   identities. Do not infer a managed DLL pin from an unchanged EXE launcher.
2. Confirm recorder support for the explicit 16384 MiB netsh argument on the
   actual Windows build. The documented interface exposes a finite MB setting,
   but its numeric acceptance and near-cap stop behavior are not yet measured.
3. On dedicated evidence roots, independently qualify both recorders near the
   15 GiB acceptance threshold with complete frames, all required directions,
   zero loss, no wrap and no truncation. Prove that exact-threshold/full-cap
   evidence is rejected. Preserve failed evidence.
4. Measure actual privileged Stop/final flush <= 60 s, idle confirmation, full
   offline conversion <= 1800 s, and enclosing hash/receipt and transfer
   completion. Record peak memory, disk free space before/after, file sizes,
   frame counts, SHA-256 values and capture loss counters. Failure of a budget
   blocks the profile; do not silently extend it.
5. Reconcile the actual full workload size, both endpoint capture lifetimes,
   role/live gates, service state and cleanup before any authorized campaign.
   Recheck disk and memory immediately before each run.

The source simulations exercise size arithmetic beyond 4 GiB, exact rejection
at 15 GiB, disk denial, immutable profile binding, streaming writer refusal
before threshold, and timeout nesting. They deliberately allocate no large
capture file and are **not** the empirical qualification above.
