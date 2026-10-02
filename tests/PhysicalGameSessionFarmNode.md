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
ServerHost's bounded `authenticated_manifest` record. That record is emitted
only after `NodeContentProvider` uses `grpc::SslCredentials`, connects, makes a
Bearer-authenticated `GetManifest` RPC, validates the response identity/hash,
and `ContentAvailability` verifies the manifest against the packaged digest.
The role-local endpoint compares the record with the hashed Server package and
run-manifest CA pin, then writes a new `node-provider.json` into its evidence
index. The reconciler verifies the typed receipt and reports
`AUTHENTICATED_MANIFEST_RPC_MEASURED`, while leaving the broader provider and
real-TLS gate `NOT MEASURED`: negotiated TLS details, the separately pinned
Node child/config, complete content delivery, and physical campaign behavior
remain independent evidence.
