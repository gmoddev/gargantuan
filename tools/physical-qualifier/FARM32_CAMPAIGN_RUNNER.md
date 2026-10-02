# Farm32 campaign runner candidate

`farm_campaign_runner.py` is a source-only, three-stage wrapper for the already
defined `thirty-two-client-farm-lifecycle.json` Agent Coordinator workflow. It
does not install services, transfer artifacts, issue bootstrap secrets, launch
Node, or start a physical run by itself. All three actions take one locally
staged, bounded JSON file. They do not accept commands or paths from a control
message.

1. Stage byte-identical role manifests, verified Server/Player packages, the
   fixed role adapters, preflight reports, side-by-side Farm32 capture tools,
   and separate fresh role/capture evidence roots on their respective hosts.
   Run the role-local preflight immediately before execution. Its report is
   inventory evidence, not qualification. Issue one access-controlled,
   single-use coordinator UUID and two 256-bit token tickets outside this
   script; copy each ticket securely to its endpoint. Never put tickets in a
   package, capture, role-evidence root, PR, or coordinator journal.
2. Start `host HOST_CONFIG.json` on the coordinator host, then start
   `role ROLE_TICKET.json` locally on each endpoint within the workflow's
   registration budget. The host verifies its byte-pinned copies of both
   tickets; each endpoint verifies its own local binary/config/preflight pins.
   The role starts the fixed, pinned capture controller and waits for its
   `capture-controller-ready.json` before joining Agent Coordinator. Only then
   can both roles register and the existing workflow arm. The controller
   requires role evidence by capture-start plus 500 seconds; its bounded
   Stop/autostop and offline export remain independent of the 540-second
   coordinator execution deadline. A missing capture or endpoint index fails
   the outer role result. The coordinator success result alone is insufficient.
3. After copying both **complete immutable** role and capture roots to an
   offline analysis location, run `reconcile RECONCILE_CONFIG.json`. It calls
   the hash-pinned 32-tuple bidirectional analyzer, then the fixed capture
   binder. Its result is `CAPTURE_DIRECTIONS_MEASURED`, with provider and
   Foundation 3L gates explicitly incomplete. Run the independent role
   reconciler, native service/fairness/resource analyzers, real Node TLS
   evidence validation, and final acceptance ledger separately.

The host configuration has `Format=GargantuanFarm32Campaign`, `Version=1`,
`CreatedUtc`, `RunId`, `CoordinatorRunId`, `SourceCommit`, exact workflow and
manifest paths plus lowercase SHA-256 pins, `HostIp`, `Port`, fresh
`JournalRoot`, fresh `ListeningPath`, and exactly `SERVER` and `CLIENT` role
entries. Each entry is a local **copy** of its ticket path and SHA-256 pin.
The coordinator uses the preallocated `CoordinatorRunId` as its assignment
identity. Host-side ticket checking deliberately does not try to open the
worker's endpoint-local files; each role checks those before joining.

Each role ticket has the same run identity, workflow/manifest pins,
`Role`, `EndpointId`, `PeerIp`, `CoordinatorHost`, `Port`, and a 64-hex-character
token. It pins the endpoint-local `FarmConfigPath`, `CaptureConfigPath`,
`CaptureControllerPath`, and `PreflightPath` with SHA-256 digests. It names a
fresh `JournalRoot` and `ResultPath` outside the immutable role/capture roots.
The capture controller is fixed to `farm_capture_campaign.py`; the role
supervisor and capture tools are separately pinned by their own configs.
`CreatedUtc` must be within one hour and the role preflight must be within two
minutes at launch. A failed run consumes its evidence roots and tickets.

The reconciliation config pins the coordinator result, both role indexes,
both capture indexes, `farm_capture_directions.py`, and
`farm_capture_campaign.py`, and names three fresh outputs: outer receipt,
direction report, and campaign analysis. A direction failure leaves no outer
receipt. A sealed capture is still unqualified until the remaining independent
gates pass. No provider PASS or KI-006 closure is inferred here.

This candidate still needs a reviewed, access-controlled ticket staging and
two-host launch procedure, qualified Farm32 service installation, real capture
timing/security preflight, packaged role binaries, and hosted CI before use in
a physical 32-client campaign. The Local and real-TLS Node campaigns each need
a fresh run identity, separate immutable evidence, and a final ledger audit.
