---
status: proposed
owner: persistence
reviewed_sources: 2026-11-23
---

# Protected GAS exports

GASM and GASP can use an optional OpenPGP envelope for offline exchange and
storage. The envelope carries one unchanged canonical GAS document. Canonical
GAS remains independently usable and deterministic. Protected exports use
fresh randomness and sit outside runtime package identity and reproducibility.
This proposal assigns no new wire identifiers or Engine dependencies.

The threat is someone who obtains or modifies an exported file, including an
attacker who stores it for future cryptanalysis or tries to exhaust resources on
import. Protection covers the enclosed bytes. Filenames, ciphertext size and
visible recipient identifiers can disclose metadata. Separately distributed
CAS blobs and packages retain their own access policies. The unlocking machine
and intended recipients can access the plaintext.

## 1. Password-protected exports

Evaluate the RFC 9580 OpenPGP profile using v2 Symmetrically Encrypted and
Integrity Protected Data (SEIPD)
with AES-256 and OCB AEAD. OCB is mandatory to implement in RFC 9580. Generate
a fresh random 256-bit content/session key for each document. A v6 Symmetric
Key Encrypted Session Key (SKESK) packet uses Argon2id and the standard AEAD
wrapping operation to protect that key. Use a fresh 16-byte KDF salt and
library-generated nonce and SEIPD salt. Verify the final authentication tag and
individual chunk tags. Fail the operation if secure randomness is unavailable.
[RFC 9580](https://www.rfc-editor.org/rfc/rfc9580.html)

Start qualification with the RFC 9106 constrained profile of 64 MiB, three
passes and four lanes. The first reader
would accept this one cost tuple. Admit new tuples through an explicit
allowlist after Windows/Linux measurements. Pass exact UTF-8 password bytes
without truncation or normalization and bound them to 1,024 bytes. File theft
allows offline guessing regardless of application throttling. Use generated
secrets with high entropy for long-lived material.
[RFC 9106](https://www.rfc-editor.org/rfc/rfc9106.html)

## 2. Configurable defaults

Users may explicitly select a local default and override it per export. Scope
the default to the user, optionally with a trusted local project identity.
Store secrets in Windows Credential Manager or a Linux Secret Service
collection and keep an opaque profile reference in preferences. Retrieve the
secret for a user-initiated protected operation. File input cannot select a
vault item, account, project configuration, or service endpoint. Headless
builds require an explicitly configured, noninteractive credential source.
[Windows credential handling](https://learn.microsoft.com/en-us/windows/win32/SecBP/handling-passwords)
[Linux Secret Service client](https://gnome.pages.gitlab.gnome.org/libsecret/)

Every export using a default still gets a fresh salt and session key. Reuse
shares the consequences of password disclosure across files. Changing the
default applies to future exports. Older exports require their original
credentials until re-exported. Retain old secrets only through a deliberate
versioned backup or recovery policy.

## 3. FIDO2 security keys

Use the enrolled credential's WebAuthn PRF or CTAP2 hmac-secret output to
unlock a local OpenPGP decryption identity. Use that high-entropy 32-byte
output in the library's RFC 9580 AEAD secret-key protection interface. The
GAS export uses the identity's ordinary OpenPGP public recipient key. Retain
the encrypted secret key, enrolled credential handle, relying-party
identifier, evaluation input, and profile version locally. An offline
sender needs only the recipient public key.
[RFC 9580 secret-key protection](https://www.rfc-editor.org/rfc/rfc9580.html#section-3.7.2.1)
[WebAuthn Level 3 PRF](https://www.w3.org/TR/webauthn-3/#prf-extension)
[FIDO CTAP 2.2](https://fidoalliance.org/specs/fido-v2.2-ps-20250228/fido-client-to-authenticator-protocol-v2.2-ps-20250228.pdf)

Provision dedicated credentials and a separate random evaluation input per
local identity. Probe actual extension output at enrollment. Require user
presence and verification, a fresh assertion challenge, and verification
against the enrolled credential and relying party. WebAuthn domain-separates
its input before invoking hmac-secret. Demonstrate that mapping across
providers before offering cross-platform use of one enrollment. A signature
alone supplies proof of an assertion and lacks a confidential reproducible
secret. Support varies by authenticator, OS, transport, PIN/UV policy and
sync behavior. The private key becomes available to the trusted application
during unlock. This design makes no post-quantum claim about the FIDO session.
[Windows WebAuthn APIs](https://learn.microsoft.com/en-us/windows/security/identity-protection/hello-for-business/webauthn-apis)
[libfido2 assertion API](https://developers.yubico.com/libfido2/Manuals/fido_assert_set_hmac_secret.html)

## 4. Post-quantum public-key recipients

Evaluate the final RFC 9980 v6 Public Key Encrypted Session Key (PKESK)
profile using ML-KEM-768+X25519. Require published test vectors and a library
implementation of the complete recipient algorithm including its combiner.
The standardized FIPS 203 ML-KEM primitive establishes a shared secret.
Conformance to the complete OpenPGP profile requires the RFC 9980 key
format, combiner, wrap, and validation rules. Security remains dependent on
current cryptanalysis and implementation.
[FIPS 203](https://csrc.nist.gov/pubs/fips/203/final)
[RFC 9980](https://www.rfc-editor.org/rfc/rfc9980.html)

## 5. Hybrid transition

The RFC 9980 composite recipient uses both components of one recipient key to
unwrap its session key. The hybrid hedges against a break in one component
under the construction's assumptions. Adding a separate traditional recipient
creates an independent classical decryption path. A policy for long-term
post-quantum confidentiality should require composite recipients for all
paths, including recovery.
[RFC 9980 migration guidance](https://www.rfc-editor.org/rfc/rfc9980.html#section-8)

## 6. Multiple recipients, rotation and recovery

Wrap one random session key independently for each selected public-key
recipient. Verify trusted public-key fingerprints during enrollment. Defer
mixed password and recipient exports until their weakest-unlock-method semantics
are clear in the product. An independently held recipient or out-of-band backup
of the original identity and enrollment provides a recovery path. Losing
the password or authenticator without a working recovery path makes the
export unrecoverable. A replacement FIDO key requires its own enrollment.

Rotate or remove recipients through authenticated decrypt and re-encrypt with
a fresh session key. Distributed copies retain their previous unlock methods.
An attacker with a compromised recipient key could decrypt previously shared
copies addressed to that recipient.

## 7. Metadata, format dispatch and tamper handling

Use RFC 9580 packet formats and specified AEAD associated data. Authenticate
the entire GAS header, kind, format/schema versions, and metadata as encrypted
payload. Use one binary Literal Data packet with an empty filename and zero
timestamp. The proposed names .gasm.pgp and .gasp.pgp are file-dialog hints.
Detect the envelope from framing and determine kind from the verified inner
header. An operation that requires protected input rejects plaintext.
Reject wrong document kind, incorrect tags, unexpected packet or algorithm
versions, extra payloads, and trailing data. Failed decryption ends processing.
[RFC 9580 packet and AEAD rules](https://www.rfc-editor.org/rfc/rfc9580.html)

OpenPGP does not cover the complete visible recipient set as authenticated
application metadata. Removing a recipient packet may deny that user access
while other unlocks remain valid. Enforce the chosen recipient and algorithm
policy independently. Cryptographically attesting the complete recipient set
would require additional signature-policy and interoperability review.

## 8. Platform and package boundaries

At Engine baseline e7bec187def55e79c6ca956b17feb82926e5d1fe, GNS selects BCrypt
on Windows and OpenSSL elsewhere. Windows Node-enabled Server staging handles
OpenSSL DLLs for that optional provider. Those dependency paths do not qualify
a shared authoring provider. Package SHA-256 is a content digest.
[GNS build](../../cmake/GameNetworkingSockets.cmake)
[Runtime staging](../../CMakeLists.txt)
[Package building](../../src/packaging/PackageBuilder.cpp)

Sequoia OpenPGP 2.4.2 is an evaluation candidate with shipped RFC 9580 APIs,
Argon2 S2K, and multiple backend features. Its v6 SKESK interface exposes the
exact S2K parameters and accepts the session key and password. It adds Rust
build/FFI and LGPL-2.0-or-later redistribution review. Demonstrate the exact
AEAD, bounded KDF, final-tag and RFC 9980 operations with its selected Windows
and Linux backends. Native C++ librnp offers a C API and Windows/Linux support
and BSD-2-Clause and Apache-2.0 notices. Its latest published release at
review, 0.18.1, replaces a retracted version with a critical public-key
session-key defect. PQ and crypto-refresh capabilities in current source
are feature-gated. Require a maintained pinned release with independent
interoperability evidence before adoption.
[Sequoia releases and API](https://docs.rs/crate/sequoia-openpgp/2.4.2)
[Sequoia SKESK API](https://docs.rs/sequoia-openpgp/2.4.2/sequoia_openpgp/packet/skesk/struct.SKESK6.html)
[Sequoia source and license](https://gitlab.com/sequoia-pgp/sequoia)
[RNP releases](https://github.com/rnpgp/rnp/releases/tag/v0.18.1)
[RNP license notices](https://github.com/rnpgp/rnp/blob/main/LICENSE.md)
[RNP C API](https://github.com/rnpgp/rnp/blob/main/include/rnp/rnp.h)

Probe Windows native WebAuthn and libfido2 on Linux. libfido2 documents both
platforms, a BSD-2-Clause license, and signed releases, including 1.17.0.
Keyrings, hardware,
and user prompts may be unavailable in headless environments. Collect
credentials only inside a trusted authoring or narrowly scoped build provider.
Keep secrets out of EditorHost JSON and package manifests. Unlock before
admission to the existing builder. Reproducibility depends on identical
authenticated canonical input and the existing package contract.
[libfido2 platforms and license](https://developers.yubico.com/libfido2/)
[libfido2 signed releases](https://developers.yubico.com/libfido2/Releases/)

## 9. Bounded decryption and publication

Preflight framing before prompts, key derivation, decapsulation or large
allocation. Initial prototype limits are one password packet or at most 32
public-key recipients, one content container, emitted 64 KiB AEAD chunks,
1 MiB envelope overhead above the GAS file limit and one password KDF in
flight per authoring process. Measure actual peak memory and support for
additional chunk sizes before expanding the profile. Bound outer packet
lengths and segment counts, input and output bytes, credential attempts,
provider memory, and secret-key packet parsing. Reject recursion, compression
and externally resolved keys. Treat cancellation and provider/I/O failures
as an unsuccessful operation.

Decrypt to a bounded private buffer and verify the entire message before
GAS validation. Keep plaintext and secrets out of temporary files, protocol
captures, logs, telemetry and diagnostic snapshots. Keep secret copies
short-lived, clear owned buffers, and assess crash-dump exposure for each
platform. Publish ciphertext export through atomic replacement after
finalization. On import, authenticated bytes still pass the normal GAS
schema, dependency, ownership and transactional checks. Preview remains
script-free.

## 10. Confidentiality, integrity and authorship

AEAD provides confidentiality and detects alteration of authenticated data by
an attacker without the needed key. Anyone with the password or public
recipient key can make a valid envelope. Authorship requires a separate
signature policy. If requested, qualify standard OpenPGP binary signatures
over the exact canonical GAS bytes and verify against explicitly trusted
signer fingerprints. RFC 9980 defines composite signature algorithms for
a future signer profile. Decryption and verification grant no Engine
authority or permissions.
[RFC 9580 signatures](https://www.rfc-editor.org/rfc/rfc9580.html#section-5.2)
[RFC 9980 composite signatures](https://www.rfc-editor.org/rfc/rfc9980.html#section-5)

## Qualification sequence

1. Continue canonical GAS and packaging work without a crypto dependency.
2. Prototype password protection as an explicit authoring operation. Pin
   versions, backend, license notices and update policy. Test independent
   Windows/Linux encrypt/decrypt, wrong password, wrong kind, downgrade,
   truncation, tampering, RNG failure and KDF ceilings. Fuzz malformed
   envelopes and run sanitizers where supported. Check unchanged package
   identity for identical canonical input and no project or disk plaintext
   publication after authentication failure.
3. Add opt-in OS-backed defaults. Verify locked vaults, missing secrets,
   cancellation, rotation, headless use and absence of secrets in traces.
4. Qualify RFC 9980 with published vectors and two interoperating
   implementations. Exercise multi-recipient recovery, malformed recipients
   and recipient-policy rejection.
   Bouncy Castle Java documents the target hybrid as a candidate independent
   test implementation. [Bouncy Castle OpenPGP specification](https://github.com/bcgit/bc-java/blob/main/docs/specifications.md)
5. Qualify FIDO unlock against named hardware/firmware and Windows/Linux
   versions. Test unplug, cancel, wrong key, absent extension, UV failure,
   recovery with an independent recipient and cross-platform enrollments
   where supported.
