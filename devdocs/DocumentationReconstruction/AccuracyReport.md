---
status: current
owner: documentation
last_verified: 2026-11-15
---

# Documentation accuracy and integrated-source reconciliation

The developer reference is reconciled against the actual merged Engine and
Studio sources. Current qualification and the normal expected-head merge and
post-merge results are recorded on [documentation PR #25](https://github.com/gmoddev/gargantuan/pull/25).
The original dirty Engine checkout and the other integration worktrees remain
untouched; this branch uses its isolated documentation worktree.

## Verified source identities

| Owner | Actual merged source | Tree and qualification attribution |
| --- | --- | --- |
| Engine PR #24 | `b5df1e7b1b80da27158ea794626f90a8ebd8a6d2` | Tree `7ef2a2b6b2bb34732bbb8335f30dff9588a68f39`, identical to qualified integration candidate `7bd9f54866427aecba93fb76d2a65d2a536d5bcd` |
| Studio PR #15 | `dc960b5433b73e1d305548e0dc5a8dfc0d9cf2f3` | Tree `f34f0474321b86f73416a9deb5732fa41c1ff1c9`, identical to final candidate `3b8f45c5edb8b06ff61a82669e54adf25b627276` |
| Node inspected source | `f4440423c0701ff396f589fd51b6ce41edc63539` | Separate owner and service/provider contracts; no new Node integration qualification claimed |

Both merge SHAs were independently resolved using Git remotes and PR metadata,
and the trees were read from the actual commit objects. Studio's extensive
native receipt qualifies ancestor `93961e78c6ce91aa59fa297fcbd9efd57e0ac121`;
the later launcher/evidence-only changes and final merged tree retain their
separate identities. Engine GPU qualification does not certify Studio UI.

## Reconstructed current reference

The hierarchy covers Getting Started, project creation/build/package workflows,
Engine authority and lifecycle, practical API behavior, the extracted declaration
catalog, renderer/physics/networking, Engine/Studio/Node boundaries, contribution
policy and historical evidence. The [complete inventory](Inventory.json)
classifies every tracked document/notices/receipt throughout the repository.
Its unknown verification dates and Unverified records remain explicit; this
campaign does not certify every old sentence by directory membership.

Renderer Foundations 3–5 and Runtime 6 are integrated. The guides now describe
PBR materials, HDR with SDR presentation/fallback, bounded local lighting,
reflection probes, cascaded/spot shadows, texture/mip residency and renderer
recovery. Instancing and the scene envelope remain opt-in; compact traversal,
unsupported authoring features, hardware limits and measured regressions are
not promoted to universal capabilities.

Runtime 6 preserves Main-owned Instance/DataModel authority, full ObjectId
generations, exact committed history and ordinary callback order. Derived
final-state work can coalesce, with explicit prepared-batch semantics. Active
Luau entries pin VM lifetime while logical shutdown cancels queued work.
Box3D and XPBD remain the production solver owners. No runtime or protocol
implementation changes are made by the documentation patch.

The API reference reconciles actual declarations and native dispatch, including
PointLight, SpotLight, ReflectionProbe, Lighting controls, destructive-method
capabilities, native property-reset restrictions and Script lifetime. The
catalog retains source metadata rather than claiming every declaration
description is a complete behavior contract. Examples include an actual local
light and retain explicit setup and client/server authority.

Studio documentation now covers merged creator/extension and renderer-authoring
capabilities: retained native draft editing and Undo, separate version-checked
Engine Source commit, explicit single-Script transfer and scoped reviewed
development plugins. Public Untrusted extension execution remains disabled.
The MCP bridge remains a narrower five-method, opt-in subset. Native Host owns
native resources/IPC; LUI owns UI/workflows; Engine EditorHost owns authoritative
project state, history, source, assets and mutation.

Core 3L correctness remains qualified under D03 `POOLED_CORRECTNESS_V1`.
Historical strict F1 misses, unrun/deferred physical-provider scopes, latency
targets and acceptance identities keep their original meanings. No physical
Local32, real-TLS Node32 or universal deployment certification is inferred.

## Traceability and independent review

[Traceability.json](Traceability.json) maps current guide claims to owning
source and existing tests at the merged identities. Detailed review and concrete
finding resolutions are in [Reviews.md](Reviews.md), [API audit](ApiAudit.md),
[renderer/physics audit](RendererPhysicsAudit.md) and [network/Studio audit](NetworkStudioAudit.md).
Each subsystem author received independent source review from another reviewer.
The effective concrete-default restriction was corrected without editing the
production declaration's abbreviated description.

Historical benchmark values, failures, sources and fixtures are preserved.
The original pre-integration audit remains explicitly historical; its former
Engine/Studio merge gate is satisfied by the actual sources above, not by
relabeling old evidence.

## Validation and merge contract

The documentation workflow compiles the real Engine-pinned Luau compiler,
runs Python and Node regressions, checks declaration/catalog/inventory/link
drift and examples, installs the locked Bun/Node site toolchain, builds Astro
and verifies every emitted page, route, anchor and repository target. Source
hashes normalize Windows/Linux checkout newlines; Git routing aliases retain
consistent target semantics on both platforms.

While GitHub billing refuses hosted jobs before execution, the user authorizes
Dockerbox CI equivalents under normal atomic worker admission. Refused hosted
runs remain refused; worker execution must be identified as worker evidence.
No check status is manufactured and no branch protection is disabled. The
documentation PR uses ordinary expected-head merge, followed by exact merged
tree comparison and post-merge checks. The receipt/command results are attached
to PR #25 without introducing a recursive qualification layer.

The patch relative to merged Engine main contains documentation, examples,
site/tooling and documentation CI changes only. Existing Engine/Studio native
and hardware receipts retain their original sources; documentation validation
does not become a fresh gameplay, GPU, latency or physical-provider campaign.

## Retained initial documentation checks

The first phase targeted Engine `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`.
Its [successful documentation run 43596708054](https://github.com/gmoddev/gargantuan/actions/runs/43596708054)
qualified documentation head `ad68093ec4301c23101afe7416a820c8139d9e52`:
250 documents, 67 declarations, eight compiled examples, 34 HTML pages and
2,137 links. Those counts are historical and are not current merged-source
counts.

Earlier failed runs are preserved. Linux exposed virtual route normalization
and root Git symlink-alias differences hidden by Windows. Focused fixes and
regressions resolved both; historical link checks remain enabled. Current
derived artifacts and PR validation report the expanded merged-source inventory
and API counts independently.
