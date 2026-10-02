# Foundation 3L Farm32 capture candidate

This is a separate candidate for the 32-client five-phase Local and real-TLS
Node campaigns. The Agent Coordinator Farm32 service at pinned revision
`5ee889fab571a9047cbc0f8724f2077c7bb9eb74` is installed side by side on
the worker. Its installed status and denial preflight passed, but no actual
Farm32 capture or campaign has run. The installed one/four-client capture
service and `worker/PktMonCapture.ps1` are unchanged.

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
complete bidirectional evidence. Recheck the fixed 2560-MiB reserve on the
selected dedicated evidence volume immediately before staging and each run;
an earlier low-space observation of worker C: is no longer current.

`farm_capture_campaign.py` is a source-only local adapter for the fixed
Farm32 capture operations. A role-local `role <config-json>` process must be
started before the coordinator is armed. It creates a fresh capture run root
separate from the role evidence root, verifies pinned executable/script hashes,
starts the worker capture through the Farm32 v2 service or starts the local
client dumpcap wrapper, and publishes `capture-controller-ready.json` only
after capture activity is observed. It waits for the same-run role result and
role SHA-256 index. The worker uses fixed service Stop and idle status before
a separately bounded offline `Finalize`; the client wrapper remains owned until
its 600-second dumpcap autostop. The adapter then seals the capture files in
`capture-sha256.json`. A role result later than capture-start plus 500 seconds
is incomplete, preserving an 80-second Stop bound within the 600-second
service lease. A failed operation attempts owned cleanup and records an
incomplete marker. No generic remote command is accepted.

The `bind` command hashes a successful coordinator result, both role indices,
and both capture indices into a same-run outer receipt. All capture and outer
indices deliberately say `SEALED_UNQUALIFIED`: this adapter does not prove
bidirectional packet tuples, role result correctness beyond the sealed result,
or the physical campaign verdict. The two-host orchestration, preflight and
capture packet analysis remain separate work. The source-only adapters have
not run a physical campaign, and the installed Farm32 service has not started
a capture. Its installed preflight receipt SHA-256 is
`2caa2250554f8423302d348e86ae0fbb0e50f4ad0690881f688d4212d361e163`.

Before a physical campaign, hash-verify the versioned source/tool bundle on
both endpoints and recheck the installed Farm32 service/root. Do not replace
the old installed service or hook. Preserve all old evidence.
Rollback stops and deletes only the new Farm32 service after checking its
ownership record, retains audit/ETL/pcap evidence, and leaves the baseline
service and installed scripts untouched.
