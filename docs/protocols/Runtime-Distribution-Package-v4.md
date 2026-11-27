# Format 4 runtime distribution

Status: development candidate. Public multiplayer requires Foundation 10A acceptance and managed native delivery qualification.

## Authoring request

`gargantuan-packager build --project <root> --output <new-directory> --distribution <request.json>` accepts the same request as EditorHost `StartPackageBuild.RuntimeDistribution`. Legacy invocations continue writing format 2 or 3. Optional `--runtime` selects the appropriate trusted tool distribution. Format 4 requires a tool-only `runtime-distribution-capability.json` with `Version: 1`, `PackageFormatVersion: 4`, and the exact staged `ExecutableSha256`.

```json
{
  "Version": 1,
  "Target": "OfflinePlayer",
  "Preset": "Hybrid",
  "Diagnostics": "PrivateSymbols",
  "PublisherTrust": "Required",
  "SigningKeyId": "release-ed25519",
  "KeyAccess": "LocalProvisioned",
  "ContentKeyId": "release-scripts",
  "ModLoading": "Disabled"
}
```

Targets are `OfflinePlayer`, `DistributedPlayer`, and `DedicatedServer`. Presets are `Open`, `Compiled`, `Hybrid`, and `Protected`. Diagnostics are `Minimal` (Luau D0), `LineInfo` (D1), and `PrivateSymbols` (D1 plus private artifact index). Playable readable source remains an intentional release representation. `PublisherTrust` is `DigestOnly` by default and `Required` selects signing. Protected records require `LocalProvisioned` on Player or `HostProvisioned` on Server. Only `Disabled` mod loading is supported. Alternative key services, offline license expiry, and extension policy have no admitted values in v1.

Write optional project policy in `.gargantuan/runtime-distribution.json`. Open the project and save legacy scenes to persist durable script SourceIds before authoring membership or expecting reproducible offline CLI builds.

```json
{
  "Version": 1,
  "SessionIdentity": "73d82611762646f8b3a75217d20e04bd9",
  "Defaults": { "Representation": "Compiled", "Diagnostics": "Minimal" },
  "Targets": { "DedicatedServer": { "Representation": "Source" } },
  "Groups": {
    "PublicExamples": {
      "Members": ["ee45f8ab9c74485c982aafa693bc17ea"],
      "Representation": "Source",
      "Encryption": "None"
    }
  },
  "Overrides": {
    "6dbab9f4bf7142e5be830bf8fd442b74": { "Encryption": "Authenticated" }
  }
}
```

The two network targets require the same randomly generated, nonzero 128-bit `SessionIdentity`. Keep the original policy in authoring storage. Rotate the value when starting a new compatibility generation. The public handshake commitment incorporates the eligible source projection without exporting the original policy value or raw source digest. The 10A `package-audience.json` still supplies the Public Player asset allowlist. Native eligibility is evaluated first. Preset defaults, project defaults, selected target or `PolicyTarget`, named group membership, and individual SourceId overrides then resolve each eligible script field. Group conflicts require an overriding value per field. `Protected` is the alias for Compiled with authenticated encryption. Source plus encryption is invalid. Server-only code cannot become eligible by selecting a representation.

EditorHost previews with `InspectPackagePolicy(ExpectedRevision, Configuration, RuntimeDistribution)`. Authoritative revision, unsaved state, script rows, and the captured `PolicySha256` return without source/key bytes. Preview caps output at 128 rows and a bounded aggregate. Supply `ExpectedPolicySha256` to bind the start request to the resolved project policy. Final package results include target, exact manifest and executable hashes, and optional private index path. Studio Build and Run is available for managed Offline Player and pins the emitted manifest.

## Stored data

The root manifest adds one closed `RuntimeDistribution` object with version, target, release ID, scoped PublisherId, signing and content key IDs, key-access mode, compiler identity, client code identity, and fixed catalog path. PublisherId initially equals ProjectId. The catalog `content/runtime-scripts.json` holds sorted rows keyed by SourceId, native Script/ModuleScript class, path segments, resolved representation, and diagnostics. Compiled and protected files have fixed names `content/scripts/<SourceId>.luac` and `content/scripts/<SourceId>.gse`. The root content table pins every artifact's stored bytes. The producer validates and installs the closed artifact set through existing bounded, atomic package publication. The catalog limit is 4,096 entries and 4 MiB, with 64 MiB aggregate decoded code and 1 MiB per artifact plaintext.

For signed packaging, `game.package.sig` contains exactly one 64-byte detached Ed25519 signature. It signs `Gargantuan runtime package manifest Ed25519 v1\0` followed by exact `game.package.json` bytes. Signature verification precedes executable admission. The runtime never treats an included public key as an external enrollment. Digests verified per script after reading bind the exact bytes into the package owner.

Source-free refers to authored scripts selected for Compiled or Protected. Engine-owned launcher and controller resources keep their existing representations. Trusted project PreRun remains a source-based Open feature. A Compiled, Hybrid or Protected request rejects an authored PreRun instead of staging it in cleartext.

A protected script stores `GRS1`, a little-endian 32-bit plaintext length, a 32-byte salt, a 12-byte GCM nonce, ciphertext, and a 16-byte tag. HKDF-SHA256 derives an artifact key with info `Gargantuan runtime script AES-256-GCM key v1`. AAD is the specified domain prefix with NUL, the exact binary header and canonical metadata containing project, release ID, publisher, script ID, role, package/entry policy and pinned compiler. Reads enforce framing, lengths, format, cryptographic authentication and producer provenance before binding. No compression or encoded opcode suite is enabled.

A private diagnostic artifact, when selected, is a separate owner-controlled sibling named by the emitted manifest digest. It maps script IDs to authoring paths and source digests. It can interpret D1 line-bearing diagnostics. It contains neither a full PC map nor original source. Keep it in private storage and exclude it when publishing package directories.

## Enrollment and launch

Provision absolute native paths via `GARGANTUAN_RUNTIME_PACKAGE_BUILD_ENROLLMENT` and `GARGANTUAN_RUNTIME_PACKAGE_RUNTIME_ENROLLMENT`. The file header is `GargantuanRuntimePackageEnrollmentV1`. Lines use TAB-delimited fields. Use an owner-controlled file and private Windows ACLs for secret enrollment. The second line is `mode<TAB>Local` or `mode<TAB>Host`. Additional rows use the following fields.

```text
trust|signer|content    ProjectId    ReleaseId-or-*    PublisherId    KeyId    64-hex-key
```

Each row contains exactly one role. Trust is a raw Ed25519 public key, signer is the private 32-byte seed, and content is a separately generated 32-byte secret. Enroll the corresponding public key for validation during build. A runtime enrollment cannot contain signer rows. Use a scoped release or explicitly opt into a project-scoped wildcard and retain a separate credential for another project, purpose, or publisher. Scope updates constitute trust rotation. Provider choices never come from package filesystem paths or game scripts.

For unsigned Compiled/Protected, deliver the builder's manifest digest via trusted native provisioning and run Player, Server, or packager validation with `--expected-manifest-sha256 <digest>`. Signed deployments can supply `--require-package-publisher`. Verify the installer and runtime executable closure externally before launch. Independent offline use works without backend services. Loss, change or unavailability of the local material prevents opening the affected protected package until it is reprovisioned.

## Network compatibility

Legacy GSES v2 remains for ordinary terrain-free hosts. Format 4 network participants require matching 64-lowercase-hex client code identities in GSES v3 Hello and Accepted. Their readiness uses the existing epoch/player binding. Authoritative Server publishes no ModuleScript clear source on managed sessions. The Player uses its local package for approved scripts and denies nonempty reflected source on live structural frames. Initial package binding records the server identity for any later materialization of those scripts. Ordinary recreation under a reused hierarchy path does not replace an approved code binding. The package pair must be reissued to update authoritative executable client code.

A managed Offline Player can carry the fully verified project terrain closure.
The Engine rejects authored terrain for managed Dedicated Server and Distributed
Player targets. Trusted loopback Development terrain sessions have a separate
[GSES version 4](../adr/TerrainNetworkBootstrap.md) handshake. Their terrain
binding does not change the version 3 managed package format.
