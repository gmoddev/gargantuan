# Farm32 Node source-only staging candidate

`PhysicalGameSessionFarmNode.ps1` is an optional, uninstalled worker-local
helper for the 32-actual-client real-TLS Node campaign. It does not modify the
installed capture service, the four-client F1 profile, or the Local provider.
It owns one Node child on the Server endpoint. The current profile intentionally
accepts only `127.0.0.1:port`; a remote Node requires separate process ownership
and certificate policy.

`Prepare` requires the farm run manifest and its out-of-band SHA-256, the
hash-verified Server package, the qualified-scale descriptor and its SHA-256,
an approved Node binary and SHA-256/source commit, a certificate/private-key
pair, and the manifest-pinned root CA. It checks the full Server deployment
manifest, then requires the descriptor's dynamic `project_id`/`revision` to
match `game.package.json` and `content/content.manifest.json`. The generated
TOML gives Node only a filesystem content package for that exact identity and
the `content.manifest.read`/`content.blob.read` game-server principal. The
Node token is read from the environment; neither it nor the private-key bytes
are copied into the config or stage receipt. The helper checks key/certificate
compatibility, endpoint SAN, validity, and a chain to the pinned CA. Its new
stage directory contains `node.toml` and `node-stage.json`; the printed stage
SHA-256 must be passed independently to `Run`.

`Run` rechecks all public pins, package identity, certificate/key/CA, and token
presence. The endpoint validator source imported for deployment checks is
pinned at `Prepare` and rechecked before import/use at `Run`. Run then creates
an exclusive `node-run.claim`. It launches only the pinned
binary with `serve --config <pinned TOML>`. The child is bounded by a hard
60–1200-second limit (default 900), a 15-second TCP readiness limit, and a
same-run `stop.request` file containing the run ID. It terminates and reaps its
owned process tree, and writes `node-run.json`. Standard output/error are
retained as separate, hash-pinned files with an 8-MiB hard acceptance cap each;
the supervisor aborts on log overflow. The Node JSON log can therefore supply
the matching negotiated-TLS receipt without logging the workload token or
private-key bytes. A consumed stage is not reusable.
The final `STOPPED` line prints the `node-run.json` SHA-256; retain this
out-of-band value for the offline TLS matcher.

`node-tcp-ready.json` and `node-run.json` explicitly record `TlsProven=false`:
a TCP connect does not prove TLS. Physical Node qualification still needs the
Gargantuan Server's authenticated gRPC content request over the pinned CA,
Node process/config/certificate provenance tied to the same run, and the
independent TLS/provider transcript required by the farm reconciler. No
campaign PASS should be inferred from helper staging or TCP readiness alone.
The supplied Node source commit is a provenance claim paired with the binary
hash; the build pipeline must independently attest their relationship.

Run the source-only deterministic fixture with:

```powershell
pwsh -NoProfile -File tests/PhysicalGameSessionFarmNodeTests.ps1
```

Optionally pass `-OfficialNodeBinary <path>` to also check the generated TOML
with the pinned official `gargantuan-node validate-config --config` command.
The fixture builds a local mock listener, tests bounded owned-child stop/reap,
single-use staging, descriptor mismatch, wrong SAN, binary/validator-pin
rejection, and exclusion of token/key material from staging. CMake registers it
on Windows when both PowerShell 7 and Go are available. It never runs the
physical farm.

The separate `PhysicalGameSessionFarmNodeEvidenceTests.ps1` covers the native
ServerHost's bounded `authenticated_manifest` record, including its exact
`server-content-N` request ID. That record is emitted
only after `NodeContentProvider` uses `grpc::SslCredentials`, connects, makes a
Bearer-authenticated `GetManifest` RPC, validates the response identity/hash,
and `ContentAvailability` verifies the manifest against the packaged digest.
The role-local endpoint compares the record with the hashed Server package and
run-manifest CA pin, then writes a new `node-provider.json` into its evidence
index. The reconciler verifies the typed receipt and reports
`AUTHENTICATED_MANIFEST_RPC_MEASURED`, while leaving the broader provider and
real-TLS gate `NOT MEASURED`: negotiated TLS details, the separately pinned
Node child/config, complete content delivery, and physical campaign behavior
remain independent evidence. `PhysicalGameSessionFarmNodeTls.ps1` can match
that request ID and package identity against an independently retained Node
JSON log record from the official Node `GetManifest` success path and verify
its TLS version/cipher pairing. It produces only
`OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN` only when the bounded
`node.stdout.log` matches the independently hash-pinned `node-run.json` and
`node-stage.json`, including run ID, root CA, stage hash, log path, byte count,
and digest. The match is still not the full physical provider PASS gate.

The Farm32 campaign's Node SERVER role ticket pins the prepared
`node-stage.json` and this helper by SHA-256, in addition to its existing
PowerShell pin. The ticket stager accepts those two pins only for the Node
SERVER role; Local and Node CLIENT retain their existing schemas. Prepare the
Node stage on the Server endpoint from an independently pinned copy of the
same run manifest and package before sealing tickets. Its directory must be
disjoint from role staging, package, capture, registry, and evidence roots.
The role runner verifies the stage identity, starts the owned Node child
before capture/Coordinator join, waits for its fresh run-bound TCP marker,
and writes the same-run `stop.request` after role and capture completion. It
requires a successful owned-child `node-run.json` before sealing its result.
TCP readiness remains separate from TLS evidence.

Optional Node-specific offline reconciliation requires independent SHA-256
pins for `node-stage.json`, `node-run.json`, the server role's sealed
`node-provider.json`, PowerShell, and the TLS matcher. It checks that the
server role evidence index contains the provider receipt, then runs the
pinned matcher and records its result hash. `ProviderGate` remains
`NOT_MEASURED`; full content-provider and Foundation 3L gates require the
separate physical acceptance evidence.
