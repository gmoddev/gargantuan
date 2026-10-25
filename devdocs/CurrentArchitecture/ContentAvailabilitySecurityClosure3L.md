---
status: resolved-targeted-validated
owner: runtime-content
last_verified: 2026-09-13
---

# Foundation 3L package-ownership security closure

## Preserved finding and scope

The completed **Security Closure Review** of published
`33f4622543ee7cb80d98d4f02e3655eabb708349` identified **SEC-3L-001**, a Medium
package-ownership lifetime ABA defect, as its sole current Foundation 3L security
blocker. Review provenance: task `6aa666d0-be40-83ea-aa2c-c0506f392ea4` and the
user-supplied SEC-3L-001 implementation request. This receipt preserves that
unfavorable result and records a focused correction, not a repeated full scan.

The old private `Record::PackageObjects` was
`std::unordered_set<const Instance *>`. Admission stored the root and original
descendant addresses. If original child A was destroyed and runtime child B later
occupied A's address under the same root, the eviction predicate classified B as
package-owned and destroyed it. The first invalid decision was historical native
address membership being accepted as proof of the current runtime lifetime.
No C++ use-after-free was required. Ownership is Gargantuan ContentAvailability;
neither the provider nor GNS makes this decision. Direct remote exploitability
is **not measured**; runtime hierarchy churn is the relevant precondition.

## Correction and lifecycle

`src/content/ContentAvailability.cpp` now uses
`std::unordered_set<ObjectId>` with the canonical `std::hash<ObjectId>` and
default complete slot/generation equality from `runtime/ObjectId.hpp`.
The eviction predicate is current root/descendant full ObjectId membership in
the original admitted set. An unknown full identity pins automatic eviction,
even if its native address and registry slot match a destroyed package child.
Registry invalidation increments generations and retires exhausted generations;
no new identity system or owning references are introduced.

Admission retains the original pre-commit hierarchy snapshot. It allocates hash
buckets and every node before `SetParent(Workspace)` using transient empty-key
node handles, without publishing detached identities. Only after successful
authoritative admission does it fill those nodes with the original objects'
published ObjectIds and swap the set into the record before Resident state.
Node insertion uses reserved capacity and canonical nonthrowing hashing.
This preserves the pre-commit ownership-allocation boundary and does not mistake
callback-created children for package contents. Commit rejection leaves the
record's set empty. Normal eviction, external-root cleanup and Stop clear it;
record destruction releases the container. Immutable byte-cache reload builds
fresh objects and a fresh identity set.

This is a private lifetime-classification correction. Provider and demand
semantics, public APIs, execution domains, client authority, serialization,
wire formats, reliable ordering, GNS, admission credits, Known, journal bounds
and KI-007 are unchanged.

## Regression and resource bound

`TestPackageOwnershipLifetime` admits R/A, destroys A, creates B with the same
registry slot but a new generation, verifies stale lookup rejection, parents B
under R, releases demand, and requires both R and B to survive eviction attempts.
A second runtime descendant independently pins R after B is detached. Removing
the last pin permits eviction without destroying detached B; reload creates fresh
root/child identities and unchanged original contents subsequently evict normally.
Weak-reference expiry checks ensure package metadata does not retain the old
child or evicted root.

The additional `gargantuan_content_ownership` executable uses a complete,
test-only allocation override in `tests/ContentOwnershipAllocator.hpp`, following
the existing content benchmark's allocation-override precedent. It retains A's
released allocation after object destruction and final weak-control-block
release, then reuses that exact block for a same-class B. It asserts exact address
reuse and distinct generation; no production allocation hooks, dangling object
access or probabilistic retry is involved. The ordinary content executable also
runs the semantic regression with its standard allocator.

On unchanged `33f462254` production source, this exact-address regression exits
1 at the replacement-pins-root assertion. All preceding reuse/generation checks
pass. Linux baseline artifacts are `baseline-build.log`, `baseline-reuse.log`
and `baseline-reuse.exit` under the evidence directory below.

Additional focused cases cover destroyed original child without replacement,
admission-callback runtime child, its removal, externally destroyed root, rejected
authoritative admission into a destroyed Workspace, and record cleanup. Existing
content tests cover cancellation during acquisition/preparation, dependencies,
repeated admission/eviction/reload, limits and Stop.

| Logical bound | Value |
| --- | --- |
| Full ObjectId | Two uint32 fields; 8 bytes on validated x64 targets |
| Retained IDs per record | At most 512, including the root |
| Identity payload per record | At most 4,096 bytes |
| Manifest records | At most 65,536 |
| Conservative aggregate IDs/payload | 33,554,432 / 268,435,456 bytes (256 MiB) |
| Additional retained logical payload versus x64 pointers | 0 bytes |
| Hash nodes/buckets, allocator overhead, transient node-handle storage | Not exactly measured |
| Simultaneous worst-case RSS at all content ceilings | not measured |

Admission's temporary node-handle vector is bounded by the same 512-object unit
limit and is discarded after commit/failure. Historical IDs do not keep objects
or registry entries alive. The aggregate arithmetic is a conservative logical
ceiling, not a measured RSS claim or promise that all maxima coexist.

## Validation and focused verdict

Current-source MSVC Release and Linux Clang 19 ASan/UBSan/LSan each pass **13/13**
affected CTests (2 focused plus 11 broader tests). The suite includes content ownership/availability, content
benchmark smoke, 100-iteration ContentScaleUnwindTests, Foundation, asset
foundation, GameSession, replication, relevance, spatial index, scheduler,
Character networking and physics. The expensive GNS overload fixture is outside
this patch's affected path and is not rerun for this correction.

| Check | MSVC Release | Clang 19 sanitizers |
| --- | --- | --- |
| ContentAvailability + exact-address ownership regression | 2/2, 1.57 s | 2/2, 2.76 s |
| Broader affected suite | 11/11, 12.55 s | 11/11, 63.36 s |
| Exact-address diagnostic | `forced_address_reuse=1`, ObjectId/pointer 8/8 bytes | Same |
| Validation exit | 0 | 0, ASan/UBSan/LSan enabled; no sanitizer report |

Both builds use `cmake --build <build> --parallel 4 --target` for the named
affected executables. CTest focused selection is
`-R '^gargantuan_content_(ownership|availability)$'`; the second selection is the
11 remaining named entries above, with `--output-on-failure --no-tests=error`
and JUnit output. Windows uses `-C Release`, serial focused tests and two-job
broader tests. Linux runs serially with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, Clang 19 symbolizer and dummy SDL.
The original failure no longer reproduces, and all ordinary admission/eviction,
reload, cancellation and cleanup controls pass. The Node 24 Astro documentation
build passes (19 pages); the pre-existing missing-404-entry warning remains.
Patch whitespace and changed-document relative links are checked before commit.

Worker: `dockerbox`, bounded four-job incremental builds; source snapshot is
verified using normalized-text SHA-256 across 562 native/configuration files.
MSVC evidence: `C:\Sandbox\Codex\Artifacts\sec-3l-001-20260913`.
Linux evidence: `C:\Sandbox\Codex\Artifacts\reliable-workload-20260913\sec-3l-001`
(container `/evidence/sec-3l-001`, build `/build-engine`). Baseline and candidate
evidence are separate. No third-party target or unrelated repository is involved.

Focused source verification and one fresh independent read-only candidate review
find no surviving bypass or candidate-introduced regression. Both root and live
descendant predicates use full identities; raw addresses are no longer
authoritative, slot-only matching is absent, failure/cleanup/reload boundaries
remain intact, and runtime children still pin roots. No client authority was
introduced. The reviewer independently checked identity generation, callback
capture, allocation preparation, record cleanup and Windows test deployment;
its review was static and does not substitute for the executed tests above.

**SEC-3L-001: resolved. Foundation 3L security verdict: PASS for the reviewed
current Foundation 3L scope**, composing the preserved full review with this
targeted correction, regression, affected validation and focused delta. This is
not a new full security scan or general deployment assurance. Publication CI
will be inspected separately at the normally pushed commit; terminal CI success
is not inferred from worker validation.

Public unauthenticated GameSession admission remains a separate deployment /
future-architecture issue. This correction neither changes that boundary nor
qualifies external deployment. KI-006 client/scale and broader gameplay fanout /
performance gates remain separate from security closure. Foundation 3L remains
partially ready; do not begin 3M. Next: complete those remaining qualified
client/scale and gameplay performance checks against the accepted workload.
