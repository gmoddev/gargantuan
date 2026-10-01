# Foundation 3L Farm32 capture candidate

This is a separate, source-only candidate for the 32-client five-phase Local
and real-TLS Node campaigns. It has not been installed, used for a capture or
qualified on the worker. The installed one/four-client capture service and
`worker/PktMonCapture.ps1` are unchanged.

The worker candidate `worker/PktMonFarm32Capture.ps1` retains the pinned
Mellanox miniport identity, physical NDIS layer, IPv4 UDP scope and complete
frame export. It writes unique Farm32 artifacts in a fresh run directory. Its
`Start` requires at least 2560 MiB free on the evidence volume, refuses any
preexisting output, and starts a **single-file, noncircular** 1024-MiB ETL.
Its `Stop` stops only its owned trace and rejects an ETL at or above 960 MiB.
Its unprivileged `Finalize` checks zero lost events, exports miniport frames to
pcapng, rejects empty/oversize output, and refuses to overwrite an existing
pcap or pending export. Finalize requires at least 1536 MiB free after ETL
capture and must be run with a separate hard deadline by a fixed local adapter.
No ring buffer or append/overwrite mode is used. An over-capacity, lost-event,
truncated or one-direction trace is incomplete evidence, not a PASS.

The client candidate `DumpcapFarm32Capture.ps1` is a fixed, hash-pinned
foreground worker for the local fiber NIC with a 600-second independent
autostop and a 1048576-kB file autostop. It uses a single pcapng, no ring
buffer, `udp port 39450 and host 10.253.3.2`, and rejects a file at or above
960 MiB. It verifies the exact local fiber IPv4/MTU/link identity and a
locally supplied dumpcap SHA-256, requires 2560 MiB free, and refuses a reused
run directory artifact. Run it hidden as a task-owned child of the local
supervisor; its 600-second normal duration must be budgeted in the lifecycle.
An abort must terminate only that owned child and mark capture incomplete.

The separately versioned Agent Coordinator Farm32 service candidate has a
600-second hard lease and 60-second privileged hook timeout. The 600-second
bound follows the current workflow's 205-second legal startup path plus the
role supervisor's 300-second total runtime, leaving 95 seconds for placement
and Stop initiation. The 60-second Stop and offline conversion of a large ETL
are **not yet worker-qualified**. Before a physical campaign, measure those
paths at the actual bounded trace size, reconcile them with the 540-second
coordinator execution budget, and prove both endpoint captures finish with
complete bidirectional evidence. The worker's current C: free space is below
the new profile's reserve; choose a dedicated approved volume before staging.

Stage by hash-verifying a versioned candidate bundle on both endpoints, then
installing only the new Farm32 service/root under a reviewed local procedure.
Do not replace the old installed service or hook. Preserve all old evidence.
Rollback stops and deletes only the new Farm32 service after checking its
ownership record, retains audit/ETL/pcap evidence, and leaves the baseline
service and installed scripts untouched.
