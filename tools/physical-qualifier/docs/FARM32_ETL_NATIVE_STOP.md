# Farm32 exact-owned native ETW Stop candidate

## Status and evidence

**Installed candidate; full near-capacity qualification remains outstanding.**
The existing `Farm32Capture16GiB-v2` storage profile, 16-GiB recorder cap,
strict 15-GiB completeness threshold, 600-second service lease, 60-second
privileged hook bound, packet scope and offline export bounds are unchanged.
No service executable, IPC operation or upstream deployment pin changed.
The separately recorded hook adoption below remains unqualified for provider use.

## Installed adoption and fresh diagnostic (2026-10-03 UTC)

Source `9ae8f68c2c8ed50589b5c54ed56657fa6fd02aad` supplies the installed hook
SHA-256 `E286C083022E963E228D5C2CE0C346BC8CE2F9BE450F01D0E8226B94780C4E93`.
The fixed-input v2 updater retained prior bytes/ACLs, changed only the hook and
its hash pin, then proved both services running and Farm32 idle. Configuration
SHA-256 is `1F93A4EA2AEAD911AAD4DF0FE6D952ADAFE91E570A9B9348800FE64A3D7F9614`.
Its receipt is retained under
`C:\ProgramData\GantriaEngine\AgentCoordinatorCaptureFarm32\hook-adoption-Farm32NativeStop-v2-9ae8f68c2c`.
The v1 updater aborted before mutation because it rejected netsh's measured
exit code 1 for the exact no-session response. V2 accepts that response only
with code 0/1 and empty stderr; twelve direct idle-check cases and seven
transaction/rollback cases pass. No capture behavior changed in that updater.

Fresh bounded diagnostic `a17d3e70-d5c7-428e-afa2-2277ab550e3a` remains
**FAILED**. The installed native Stop recorded 10,287 buffers written, zero
events/log buffers lost, and a 5,393,350,656-byte ETL. Named closure and service
idle were proven. The client application missed 629 DATA packets despite all
HELLO/END records arriving; the worker application received its complete stream.
Bounded observations found no receive exception, a maximum 7.300-ms client loop
gap, and zero host-wide UDP input-error increment. Both NIC error/discard deltas
were zero. These counters do not locate the missing packets or waive the loss.

Independent replay subsequently found all 3,443,900 marked packets exactly
once in both directions in each capture. The client capture contains 3,443,900
frames, 5,083,196,932 bytes, SHA-256
`d34fb1e2329aea4e7166174255a2baa3a13f43ab9cd75661e39a08962dc048db`,
with all dumpcap loss counters zero. All 629 application-missing packets and
13 neighboring packets have the expected tuple/run/payload, valid IPv4 and
nonzero valid UDP checksums, 1,428-byte unfragmented IPv4 packets and 1,408-byte
UDP datagrams. The loss therefore lies after the client capture observation
and before application receive; the exact Windows component is **NOT MEASURED**.

The original controller aborted offline worker export after the application
failure. A separate bounded export completed in 354.919 seconds and produced
3,474,171 packet frames, 5,111,354,168 bytes, SHA-256
`b70187f4f1bd0c5d79e588c3b7cb3e266a1343ec54f50df3853c6f6ca29352f5`.
This diagnostic replay cannot change the original `DIAGNOSTIC_INCOMPLETE`
result. Original task cleanup proved no owned endpoint children or UDP sockets
and both worker services running/idle. Full near-capacity qualification and
production/provider use remain blocked.

Independent receipts are retained at
`C:\Users\aiden\.codex\artifacts\farm32-a17d-client-loss-audit-v1\client-loss-attribution.json`
and
`C:\Users\aiden\.codex\artifacts\farm32-a17d-worker-audit-v1\worker-capture-audit.json`.
Both raw worker files were copied and size/hash-verified under
`C:\Sandbox\Codex\Evidence\Farm32InstalledStop5GiB-v1\a17d3e70-d5c7-428e-afa2-2277ab550e3a\worker-capture-archive`
before their exact worker copies were retired. The retained worker relocation
receipt identifies the archive; small original receipts remain in place.

The source hook passes 304 qualifier tests, both PS5.1/PS7 interop-compiling
simulations, and the legacy four-client hook simulation. Candidate build and
eight probe self-tests pass; both instrumented Release aggregate workload modes
pass on the worker. Exact-source hosted Windows Release, Linux ASan/UBSan and
GNS sanitizer jobs pass. Original JUnit artifacts independently verify 95
Windows, 53 Linux and 10 GNS tests. Native dispatch `37091176350` completed at
2026-10-03 04:19:14 UTC; GNS run `37091175492` had already passed. The source-owned
CI and official qualified-package verifiers both return `MEASURED_PASS`.
The retained CI index at
`C:\Users\aiden\.codex\artifacts\f1-ci-evidence-9ae8-v1\ci-index.json`
has SHA-256 `15e4cfb62ae667bd753ce047074fb025f24b8de4fc9358b8aed03505893476be`.
These are CI/package results only. No fresh F1 or provider PASS
follows from this diagnostic.

## Client drop diagnostic preparation and external action

Client driver access requires an Administrator process; the normal controller
cannot use PktMon. A fixed-input manually launched helper was prepared with
an automated pre-capture barrier, process-creation identity, exact tuple filters,
a 180-second capture lifetime, bounded storage and owned cleanup. It cannot
start application traffic or change network policy. Its truncated drop records
are diagnostic only; normal full-frame capture and exact application delivery
remain independent mandatory checks.

Setup `d3b30465-1abf-4eaf-8a3a-fb73174b2682` failed before capture or traffic:
Windows ellipsized the first long filter name in its table. The exact-name
check rejected it, and cleanup correctly refused an unrecognized inventory.
One task-owned filter remained; no trace was started. V2 uses 23-character
run-derived names, parses the measured complete two-line tuple format, and
preserves full inventory checks. Its integrated harness passes 103 tests;
the separate fixed-run cleanup passes 11 tests and verifies the five retained
old receipt hashes plus two fresh idle/exact-inventory observations before
removing that single known inventory. No arbitrary capture or filter cleanup
is authorized by this recovery helper.

V2 preparation `508b86d6-38fc-4424-b5aa-d52a00889117` expired its 300-second
manual-helper wait without any helper, capture or traffic starting. It remains
`DIAGNOSTIC_INCOMPLETE`; its identity must not be reused. At that checkpoint,
old-filter recovery and a fresh diagnostic required a manual Administrator launch.
The fixed cleanup launcher is
`C:\Users\aiden\.codex\artifacts\Admin-Cleanup-d3b30465.ps1`;
the frozen corrected kit is
`C:\Users\aiden\.codex\artifacts\farm32-client-drop-5g-v2`.
The subsequent execution below supersedes that external-action checkpoint.

## Completed client drop diagnostic (2026-10-03 UTC)

The user launched the fixed recovery helper. Its immutable
`fixed-owned-filter-recovery-v2/recovery-result.json` records
`EXACT_OWNED_FILTER_REMOVED`, no capture Start/Stop, and the original failed
qualification unchanged. Fresh observations confirmed empty filters and idle
Packet Monitor before a new run.

Fresh run `ccd6f159-acd1-4b43-bb69-b58947e4970f` completed the reviewed v2
barrier, drop observer, full captures, offline replay and cleanup. Its outer
result is successful **DIAGNOSTIC_ONLY_NOT_NEAR_CAP**, not provider qualification.
The client sent 1,726,200 DATA datagrams and received all 1,724,500 worker DATA
datagrams. Both applications have zero sequence gaps. Both full captures contain
all 3,460,300 marked DATA/HELLO/END packets exactly once across all 32 tuples.
Client dumpcap and worker ETW report zero loss. Retained files are:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| Client pcapng | 5,107,403,332 | `b0811b6cfe5ebdf11e9e99aeff1315ef9d06173a944be424617c89f7f92b7ed4` |
| Worker ETL | 5,389,156,352 | `6d1fbf5848f7bc63492b67294c2f7f17a0158af32d265ecd99b0fff65f6b6361` |
| Worker pcapng | 5,107,403,824 | `cb653c8882afe49ebfdffe4666de2f8337020eff78464c6ea39a6b6f28ed9027` |

The worker pcapng has four additional unmarked frames. Exact marked sequence
identity agrees between captures. Native Stop, named cleanup and service idle
passed; both services remain running. No owned endpoint process or relevant UDP
socket remains. The drop helper independently reports
`OWNED_SESSION_AND_FILTERS_CLEARED` with no cleanup error.

The 27,649-byte drop ETL has SHA-256
`0a6d69502c9db2439ab74ed9d3484c0d8de8d142f45fabbd70e517401fe3f8f7`.
It contains 1,113 rundown events, zero raw drop events, and zero values in all
158 drop-counter and 197 flow-counter rows. Its drop-only pcapng has zero
packets. Native event/buffer-loss counters are zero. This establishes no
observed drop, but **observer traffic coverage is NOT MEASURED**. It cannot
prove the preceding 629-packet application loss is fixed or identify its cause.
The earlier failures remain failed. No host-wide IP/UDP counter is attributed
to these flows without packet-level evidence.

Receipts are under
`C:\Sandbox\Codex\Evidence\Farm32ClientDropTraffic5GiB-v2\ccd6f159-acd1-4b43-bb69-b58947e4970f`;
the worker originals remain under
`C:\Sandbox\Codex\Evidence\Foundation3LFarm32\ccd6f159-acd1-4b43-bb69-b58947e4970f`.
Closed drop analysis is retained at
`C:\Users\aiden\.codex\artifacts\farm32-drop-decode-v1\ccd6f159-acd1-4b43-bb69-b58947e4970f\analysis-v2\counter-summary.json`.
The actual text export uses a UTF-16 BOM. The initial strict UTF-8 decoder
failed closed; a separate BOM-aware decoder passes 17 regressions and retains
the failed output. No packet-format calibration is inferred from absent events.

This successful 5-GiB diagnostic does not satisfy the unchanged full near-capacity
gate or explain the intermittent receive-path loss. Those remain prerequisites
to provider use. No fresh F1 or Local/Node provider run was performed.

## Earlier native-stop diagnostic

V4 and V5 lost packet buffers in their retained ETLs. Their metadata replacement
starts exactly at `file_size mod 2^32`; the
[historical attribution](FARM32_ETL_PERFORMANCE_MERGE.md) preserves the exact
counts. The internal Windows fault function is **NOT MEASURED**. Explicit
`perfMerge=no` alone was insufficient.

Separate diagnostic `022b3c56-fb8a-4bc2-83d6-bef807cc2d15` used supported native
Stop. ETL sizes were 5,378,146,304 bytes before FLUSH, 5,390,729,216 before STOP,
and 5,392,302,080 after STOP and named cleanup. Bounded 5-MiB pre-FLUSH and
8-MiB pre-STOP witnesses cover the complete historical 4-MiB overwrite band;
the corresponding packet regions remain identical across native Stop and
netsh cleanup. Header changes are preserved separately. This establishes the
tested path's preservation, not an unlimited claim about Windows internals.

Later independent replay found all 3,442,300 marked packets exactly once in
both directions in each capture. The worker pcap has 3,472,054 packet frames,
5,108,517,148 bytes and SHA-256
`ddf04425546ffb6e71cc38727ed5639ce92501ffe8b1195c49e38c65e605b1f4`.
ETW reports zero lost events. The **original run remains FAILED**: 290 client
application receive gaps exist even though those packets are in the client
capture. No application gate was waived, no receipt rewritten, and no full
near-capacity/provider PASS follows.

Retained independent receipts:

- `C:\Users\aiden\.codex\artifacts\farm32-direct-stop-5g-audit-v1\worker-capture-audit.json`
- `C:\Users\aiden\.codex\artifacts\farm32-direct-stop-5g-audit-v1\native-stop-result.json`
- `C:\Users\aiden\.codex\artifacts\farm32-direct-stop-5g-audit-v1\named-control-v2-result.json`
- `C:\Users\aiden\.codex\artifacts\farm32-5g-loss-audit-v1\client-loss-attribution.json`

The original native-stop receipt says sequence validation was not measured at
that stage; the later independent audit supplies that result without editing it.

## Owned lifecycle

The existing hash-pinned PowerShell hook embeds a small 64-bit interop wrapper
for Microsoft's supported [QueryAllTracesW](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-queryalltracesw)
and [ControlTraceW](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-controltracew)
APIs. It accepts only QUERY, FLUSH and STOP, bounds enumeration to 256 sessions
and strings to 2,048 UTF-16 characters, checks native layout, and releases all
unmanaged allocations. It needs no new runtime or generic execution interface.

Start writes an exclusive ownership intent before asking netsh to create a
trace. Its control name is `GargantuanFarm32-` plus the lowercase SHA-256 of the
canonical uppercase ETL path encoded as UTF-8. The measured Windows mapping
is `NetTrace-<control name>` for the native session. The source explicitly asks
for 512-KiB buffers. Native enumeration and QUERY must agree on that exact name,
canonical ETL path, nonzero handle, GUID, buffer size, hard cap and sequential
nonappend/noncircular/nonrotating file mode. The observed native handle is
stored as a decimal string, preserving every bit of its unsigned 64-bit value.

Stop rechecks those identities, FLUSHes, re-enumerates, QUERYs again, and then
STOPs by **exact name with handle zero**. This avoids interpreting an old
numeric handle that Windows could reuse for an unrelated name. The recorded
handle/GUID/path still have to match each query. The APIs do not offer an
atomic compare-and-stop generation primitive; the unique path-derived name
and exclusive service-owned run prevent legitimate reuse, while observed
replacement or ambiguity fails closed. This is not protection against another
administrator intentionally replacing the same exact session in the final
API-call race.

After native success, the marker atomically retains `NativeStopRecorded=true`,
the maximum observed `NativeEventsLost` and `NativeLogBuffersLost`, and the
STOP `NativeBuffersWritten`. All are unsigned 32-bit counters. Named netsh
cleanup follows only after native absence is confirmed; native errors never
fall back to an unqualified/global netsh stop. Final named status and native
enumeration must both be idle. Nonzero native loss is reported after cleanup
and remains invalid even if a later exporter reports zero.

The same operation handles service lease expiry and restart recovery without
an age-based rejection. Partial Start before trace creation can clean up
without an ETL; it cannot export. Partial Start after creation matches only
the exact intended name/path and records cleanup while remaining unqualified
because no successful Start identity exists. Repeated successful Stop is
idempotent. If STOP succeeded but its marker write failed, later cleanup may
close netsh's wrapper, but cannot manufacture a successful native receipt.

## Marker and independent replay

`farm32-netsh-owner.json` remains retained evidence. `StopPolicy` is
`ExactOwnedControlTraceW-v1`; `ControlSessionName`, `TraceSessionName`,
`TraceSessionGuid`, `TraceSessionHandle` and `TraceIdentityRecorded` bind Start.
`NativeStopRecorded` plus the three counters bind actual Stop. Before Stop the
flag is false and all three counters are null. The historical active-status
file describes Start only; it is not a current liveness signal.

Both initial and replacement JSON writes use UTF-8 **without BOM** regardless
of whether the fixed service starts Windows PowerShell 5.1 or offline export
uses PowerShell 7. Writes use an exclusive temporary sibling and atomic
move/replace. Fresh readiness rejects absent or malformed start identity;
sealing and offline replay additionally require successful native Stop with
zero loss. Finalize also checks native absence. Old markers do not satisfy
the new policy. Historical receipts retain their original interpretation.

## Validation and remaining gate

Source-executing hook simulations compile the actual embedded interop under
Windows PowerShell 5.1 and PowerShell 7 while replacing only OS calls. They
exercise ownership mutations, duplicate/replaced sessions, append mode,
partial Start on both sides of native creation, full uint64 handles, expired
cleanup, native errors, post-STOP marker-write failure, loss retention,
idempotence, exact named cleanup, no-BOM bytes and existing bounded export.
Python campaign/replay mutations cover the corresponding receipt gates.

These simulations neither start ETW nor prove installed native behavior. The
installed adoption and fresh diagnostic above supply separate bounded native
evidence. Resolve the client receive loss before the next capture preflight.
The unchanged full near-capacity exact-sequence, zero-loss, full-frame,
deadline, export and cleanup gates must all pass before provider use.
