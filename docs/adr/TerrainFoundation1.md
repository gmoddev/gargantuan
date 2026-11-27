---
status: accepted implementation direction
owner: Terrain Foundation 1
---

# World-owned terrain and private chunk execution

The terrain campaign starts from Engine `ff5cf28f8` and the separately retained
research commit `040ee80ba`. `Workspace.Terrain` has one public scoped Instance
identity per live Workspace. Its Field is the world’s editable authority. Main
validates and orders requests through the mutation gateway. Workers derive data
from immutable captures and return candidates to a Main safe point.

Store the signed field in a uniform project lattice with one owner per sample.
A chunk owns 32³ samples and the cells whose minima lie inside its half-open
volume. High face samples are obtained from adjacent owners. Additional halo
samples provide gradient and stencil inputs. Saturated air is implicit in the
authoritative field. An uncaptured dependency or an unsynchronized client field
has explicit unavailable semantics.

Use a sparse directory for immutable stored pages and a private packed
structure-of-arrays registry for active chunk metadata. The matched archetype,
sparse-set, hybrid, and packed comparisons show a useful tradeoff. The packed
store benefited random access, revision validation and churn. Archetype tables
benefited full projection scans. Capture work by bounded region and consume
ready work by indexed queues, which removes the need for steady-state full
scans. A private value ticket pairs a chunk coordinate with scope, activation
generation and separate geometry and material revisions. The scheduler also
checks the complete field dependency set. Stored page identities, public
Instance IDs, renderer resource IDs and provider body IDs remain distinct.
[Metadata measurements](../../devdocs/Validation/TerrainMetadata.md)

Derive one native ambiguity-aware surface and condition its provider surface
with a bounded seam-consistent geometric policy. Publish immutable SDL-ready
values and provider-neutral collider admission in the same edit cohort. Publish
readiness for each consumer at its actual boundary. GPU submission can occur
after collision becomes active. On an ordinary denied stage, discard candidate
resources and retain the prior committed field and colliders. Provider retirement
failure during final install is an exceptional quarantine case and pauses world
physics. Its receipt does not claim a gameplay-ready result.

Ongoing persistence work uses separately bounded, versioned terrain artifacts
and an inert manifest. Future external terrain package types and peer protocols
require their own format, admission and compatibility decisions.
[Current development implementation](../../devdocs/CurrentArchitecture/TerrainFoundation1.md)
