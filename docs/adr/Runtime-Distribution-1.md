---
status: candidate
owner: runtime packaging
---

# Runtime distribution foundation

The isolated Engine candidate starts at `ff5cf28f8089426251e761fb1ce532bf572a4698`. It integrates the exact Foundation 10A audience implementation from `28a3c254ba41a79f2ee1a79cb4d434a3d2644a72` while retaining Foundation 9 GUI hydration. That dependency still has a separate public release gate.

## Decision

The existing PackageBuilder, EditorHost revision capture, content table, and package admission remain the authority. Opted-in format 4 builds may contain Source, Compiled, and Protected records in one script catalog. Open, Compiled, Hybrid, and Protected populate authoring defaults. Package policy selects diagnostics, publisher signing, provisioned key access, and audience independently. External mod execution remains disabled in this foundation. Existing v1–v3 package entry points keep their format semantics.

Capture filters by native audience before compiling or encrypting. The exact 10A projection remains the boundary for a public Player. Compiled and protected entries remove the reflected Source from a trusted project snapshot or blank it in the fixed 10A client projection. Their executable identity lives in a separate bounded catalog. Only native package admission can bind code onto an unstarted LuaSourceContainer. The Engine runtime keeps the existing Script execution domains, coroutine scheduling and module cache keyed by generation-safe live object identity. Cloning copies an immutable admitted code handle into the new instance and creates a fresh live module identity.

Compiled admission requires an out-of-band expected manifest digest or an enrolled publisher key authorized to attest the pinned compiler output. SHA-256 from a package is content integrity information. Ed25519 establishes publisher approval of exact manifest bytes. The enrollment owner supplies the distinct compiler-origin trust. Arbitrary bytecode, untrusted publisher keys, and extension directories cannot reach the Luau loader. The Engine uses stock Luau 0.734 at `3fc82b1071ab387531175869afc4fb528464afa4` and the existing Engine bytecode profile.

Managed Server suppresses shared ModuleScript Source across the entire session, including when Server itself ships that script as Open. New package pairs use GSES v3 Hello and Accepted to match a binding of project, authored pair identity and approved client projection before native Player admission. The managed client rejects clear Source on structural publish and property-update routes before touching the live replica. Initial catalog hydration continues before attaching the client Engine. Subsequent rematerialization binds only the recorded package script and original server slot/generation. Publication applies managed code before deferred replica notifications. A newly created server generation cannot claim an existing script binding by name. Managed GUI clones use the package session binding, template SourceId and version for opaque provenance.

## Trust and availability

Build and runtime enrollment are trusted native inputs and have separate paths. Format metadata can select a scoped key ID. It cannot choose an enrollment path, grant permissions, or appoint a publisher. Use a protected operator directory and owner-only ACLs under Windows. Unix enrollment rejects unsafe ownership and modes. Public verification keys still need operator-controlled integrity. Native launcher and preloaded DLL closure must already be trusted. In-process package verification cannot authenticate code the operating system already loaded. For signed-only deployments, supply `--require-package-publisher` in the trusted launch configuration. Pin the expected digest when exact rollback or version selection is necessary.

Platform SDK builds can enable reviewed OpenSSL 3.x. Ed25519 and authenticated AES-256-GCM with HKDF-SHA256 use the library APIs. Source and externally pinned unsigned Compiled work without the optional backend. Missing crypto, enrollment, key and failed authentication are structural failure outcomes. Release identity is deterministic for the tested plaintext policies with stable authored IDs. Protected sealing uses fresh random release and per-script cryptographic values. Identical protected source can therefore produce new stored digests, reducing byte-level deduplication across builds.

Locally or host-provisioned keys support independent offline and dedicated Server use. The runtime holds the content key only while opening the bounded catalog. Admitted bytecode remains in its native owned handle, and Lua VM instructions and constants exist for the executable's lifetime. An owner of an authorized client can recover executing code. Store, account, entitlement-backed key issuance and expiry checks are separate provider work. Native Node content remains an immutable byte provider for existing asset/content namespaces. Node changes are unnecessary for the local and headless provisioned modes.

## Scope and qualification

The first managed load decodes admitted package entries once during bootstrap, under explicit byte and entry caps. Lazy per-module AEAD, measured producer caches, full VM symbolication, external mods and optional advanced bytecode transformations remain separate candidates. Runtime GAS authoring import and export remain independent.

The [format contract](../protocols/Runtime-Distribution-Package-v4.md) describes precise admission and creator configuration. Consult the [qualification record](../../devdocs/Validation/RuntimeDistributionFoundation1.md) before using this candidate as a promotion baseline.
