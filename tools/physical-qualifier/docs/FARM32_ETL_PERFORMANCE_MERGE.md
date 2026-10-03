# Farm32 packet ETL preservation

The Farm32 worker hook explicitly passes `perfMerge=no` to `netsh trace start`.
This disables optional stop-time performance metadata merging. It preserves the
physical NDIS interface/filter, complete packet retention, noncircular 16-GiB
cap, strict 15-GiB acceptance, disk reserves, 600-second lease, bounded Stop,
offline export and zero-loss requirements. The capacity profile name remains
`Farm32Capture16GiB-v2`; the changed hook hash and explicit boolean
`PerformanceMetadataMerge: false` distinguish the candidate. Staging and offline
acceptance reject absent, enabled or incorrectly typed marker values.

Synthetic run `0309a027-a783-44e7-8465-584eb20babaf` is retained as failed
evidence. Its worker pcap lacks 2,015 marked DATA packets that are present in
the client capture and both application ledgers. Raw NDIS event count equals
exported packet count, excluding an exporter count loss. All missing ranges are
interior, with neighboring packets between 23:48:41.287152 and
23:48:41.547439 UTC on 2026-10-02. No missing data is embedded in other frames.

Supported `OpenTraceW` header inspection reports 28,721 written 512-KiB buffers,
zero lost events and zero lost buffers. The file contains 28,715 buffers.
Independent diagnostic inspection finds main-stream buffer sequence numbers
4146, 4153, 4156, 4164, 4165 and 4166 absent. Six performance-metadata buffers
instead occupy file slots 4139 through 4144 in the affected neighborhood. Every
missing DATA range has both neighbors in the raw surrounding buffers; metadata
buffers contain no marked DATA or NDIS packets. Stop's retained output says
`Merging traces ... done.`, and the resulting file was created during Stop.

These observations identify stop-time metadata merging as the correction
target. They do not expose Windows' internal overwrite branch: the pre-merge
source was not retained. Raw ETL layout inspection remains diagnostic, not a
new public format contract or permission to accept the failed run.

The worker's `netsh trace start help` documents `perfMerge=yes` as the default
for performance metadata merging. `correlation=disabled` is already its default
and controls a different operation. The supported explicit `perfMerge=no`
option is smaller than replacing netsh's ownership and cleanup with a custom
ETW controller. See Microsoft's [netsh trace command documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh-trace)
and [trace logfile header API](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-trace_logfile_header).

Detailed read-only findings are retained in
`C:\Users\aiden\.codex\artifacts\farm32-v4-offline-diagnosis\DIAGNOSIS.txt`
(SHA-256 `794b06d957d20eb84aacd2a2bb4d831f06e7042000f99b547f56043566e74d31`),
with the packet, buffer-header and missing-range joins beside it.

Command-construction and marker-denial tests qualify the requested configuration
only. A new bounded near-capacity exercise must independently prove complete
sequences, full frames, zero capture loss, bounded export and cleanup before
the candidate may be used for provider qualification. No physical or provider
PASS follows from this correction alone.
