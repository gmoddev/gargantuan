---
status: selected-contract-executable-proof
owner: runtime-networking
last_verified: 2026-09-14
related_code:
  - include/gargantuan/network/ReplicationCoordinator.hpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReliableByteAdmission.hpp
  - tests/PooledReliableServiceRecoveryContractFixture.hpp
  - tests/NetworkingContractsTests.cpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L pooled-service overload and recovery contract

## Decision

**B — SEPARATE SERVICE RECOVERY FROM STRUCTURAL CONVERGENCE.**

The former workload contract used one fixed 20-second requirement for both
ordinary service recovery and complete structural convergence after overload.
That is not mathematically implied by the accepted Option C service floor or by
the existing 3J/journal hard limits.

The 20-second gate remains useful and conservative as a **service-recovery**
requirement: after excess offered demand stops, accepted transport debt, pooled
admission state, ordinary scheduler service and qualified gameplay must return
to their ordinary bounded behavior within 20 seconds. It is no longer a universal
promise that every semantically necessary structural state retained by 3J and the
journal must also have reached every conforming peer by that same instant.

Complete **structural convergence** instead uses a bounded workload-derived
service curve over the semantically necessary retained work that exists when
offered excess stops. This does not raise the accepted profile, enlarge a queue,
weaken Known semantics or permit history to disappear without representation.

The stopped production checkpoint is
`d2742796fc15a56f30048edcc20d1f3a444d62e2`, directly on published base
`222c5beb318552a9cc07ec043c5b34581d64980f`. No production pooled-service source
is resumed or qualified by this contract task.

## Five distinct guarantees

### Resource boundedness

All delivery machinery remains finite:

- ChangeJournal retention is 16,384 records per scope.
- current 3J pending transitions are bounded to 65,536 per peer and 1,048,576
  across the session;
- the pooled admission model/implementation may retain at most one next
  complete-group offer per peer, G = 512 KiB, with at most 32G pending globally;
- peer/global credit remain capped at G / 4G;
- at most four accepted structural drain grants exist globally;
- committed pooled structural debt remains at most 4G;
- only one prepared structural frame may await scheduler acceptance for a peer;
- generation teardown releases peer-local pending/prepared/admission ownership.

These are different bounds. In particular, the pooled `PerPeerPendingCap = G`
does **not** mean the peer can have only G bytes of semantically retained journal
or materialization work behind the next admission opportunity.

### Service boundedness

For a connected, conforming peer with fresh feedback and the accepted drain
floor, structural service opportunity remains finite and fair. The accepted
profile provides:

```text
PeerCreditRate = 2 MiB/s
GlobalCreditRate = 64 MiB/s
StructuralPool = 64 MiB/s
PeerDrainFloor = 16 MiB/s while a grant is active
MaxConcurrentDrainGrants = 4
N = 32
```

The sustained per-peer service guarantee is therefore limited by 2 MiB/s credit,
not by the 16 MiB/s active-grant drain burst. Aggregate sustainable structural
service is 64 MiB/s. The accepted model's maximum first-grant fairness wait from
credit eligibility remains 220.5 ms; a zero-credit maximum group needs at most
250 ms to earn G credit.

### Overload containment

When offered structural demand exceeds sustainable service:

- no extra semantic payload queue is created by pooled admission;
- a peer cannot own more than one active structural grant;
- unrelated gameplay/control retirement cannot retire structural debt;
- Known and journal progress remain acceptance-only;
- gameplay retains the existing reserve/FIFO guarantees for qualified traffic;
- non-draining or unqualified peers stop receiving new structural grants;
- finite journal and pending-transition limits remain fail-closed boundaries.

Overload may increase retained semantic work up to those existing bounds. It may
also make the 3J diagnostic age deadline fire. Neither condition licenses false
progress or time-based debt release.

### Service recovery

After excess offered demand stops, ordinary service must return to its qualified
behavior within the existing **20-second service-recovery deadline**. This gate
covers bounded transport/admission cleanup and subsequent gameplay probes. It is
not proportional to historical mutation volume and therefore does not claim
complete structural convergence.

Accepted structural debt itself is much smaller than retained semantic history:
4G maximum committed debt plus bounded pending admission is already covered by
the accepted pooled debt and drain proofs. The 20-second gate remains deliberately
conservative for that state.

### Structural convergence

Let, at excess-demand cessation:

- `W_i` be the complete-message bytes of **semantically necessary** structural
  work retained for conforming peer `i`, after only semantics-preserving
  coalescing already permitted by the 3J contract;
- `W_sum = sum(W_i)` across conforming peers;
- `r_i = min(PeerCreditRate, StructuralPool / N) = 2 MiB/s` for the selected
  32-peer profile;
- `R = min(GlobalCreditRate, StructuralPool) = 64 MiB/s`;
- `T_credit <= 250 ms` be the conservative zero-credit G warmup;
- `T_fair <= 220.5 ms` be the accepted maximum first-grant fairness delay from
  credit eligibility;
- `T_service <= 20 s` be the separate fixed service-recovery allowance.

A conservative complete-convergence bound is:

```text
T_converge
  <= T_service
   + max(
       max_i(T_credit + T_fair + ceil(W_i / r_i)),
       ceil(W_sum / R)
     )
```

This is intentionally conservative because structural draining may proceed during
the 20-second service-recovery interval. The formula is a hard upper envelope,
not an instruction to delay structural service until service recovery completes.

The guarantee is conditional on the peer remaining connected and continuing to
supply fresh feedback at or above the required drain floor. A peer that cannot
meet that deployment contract remains contained and ineligible for new grants;
terminal peer policy is not converted into a false convergence promise.

## Retained-demand lifecycle

The relevant stages are separate and remain owned by their existing subsystems:

1. **Discovered** — relevance/catalog/journal inspection observes authoritative
   current state or a committed mutation. Discovery does not imply delivery.
2. **Planned** — 3L dependency-complete continuation derives a bounded complete
   candidate. Incomplete scratch is neither Pending nor Known.
3. **Journal-retained** — authoritative committed mutation history remains in
   the bounded ChangeJournal window. This is delivery evidence, not a second
   semantic authority.
4. **Peer pending** — 3J records current Desired/Known Enter/Leave relationships,
   bounded by the existing per-peer/session transition limits. Journal cursor lag
   is a separate source of peer work.
5. **Credit eligible** — a complete encoded group no larger than G is offered to
   pooled admission. No backlog-sized serialized queue is created.
6. **Granted/reserved** — finite peer/global credit, debt funding and one of four
   drain slots are reserved synchronously.
7. **Accepted** — matching scheduler acceptance commits exactly the prepared
   frame. Rejected or expired preparation does not advance sequence, journal or
   Known state.
8. **Known / cursor represented** — Enter membership and represented journal
   progress change only at the matching accepted commit.
9. **Retired** — exact sender-local native retirement receipt releases accepted
   structural debt. Terminal release remains a separate accounting outcome.

No new authority layer is introduced at any stage.

## Journal semantics and safe coalescing

The current 3J contract already distinguishes history that must remain ordered
from current-state replication that may collapse:

- an unknown pending Enter reconstructs the current complete materialization and
  skips pre-publication property history already represented by that Enter;
- create/destroy, reparent, Attributes, tags and other structural barriers retain
  their established ordered handling;
- hard-reference changes remain dependency-sensitive and cannot be collapsed
  across a state where doing so would make a required target unavailable;
- for already-known ordinary native properties stored in the catalog property
  map, `ProduceIncremental` already substitutes current authoritative values where
  the established replication semantics permit current-state projection.

`Name` is a narrow special case. Snapshot/materialization stores it in the
separate `SnapshotObject::Name` / `PublishReplication::Name` field and explicitly
excludes `Name` from the generic native `Properties` map. Consequently the current
known-object journal path does **not** perform the generic current-catalog value
substitution for `Name`; it replays each retained `Change.Value`.

That does not make every historical Name mutation semantically necessary. GRPL
is a state-projection boundary: `ChangeJournal.Sequence` never crosses it, a
newly materialized object publishes one current complete Name, and 3J already
permits ordinary property history to coalesce where no ordered structural barrier
requires intermediate states. `Name` is a scalar current-state field and carries
no parent/reference dependency edge. Repeated Name writes for the same live
object between lifecycle/hierarchy/dependency barriers may therefore be represented
by the final current Name without creating a new authority or weakening an
accepted ordering dependency.

The stopped canonical workload exposes that special-case implementation gap. It
mutates only `Name` on 16 already materialized Parts, so production serializes
thousands of historical scalar values even though semantic convergence requires
only the final Name for each live object.

A safe implementation may collapse repeated coalescible scalar property records
for the same `(ObjectId, property)` to one final current-state operation **within
the existing ordered barriers**. For `Name`, the final value must come from the
current catalog publication's separate `Name` field. Cursor advancement must
remain transactional: no skipped/coalesced record becomes represented until the
frame that semantically covers it is accepted. This contract does not authorize
coalescing lifecycle, hierarchy, tag/attribute or dependency-sensitive reference
barriers merely because a benchmark becomes smaller.

The DataModel remains authoritative. Coalescing removes redundant delivery
history, not semantic state. If a peer falls outside the 16,384-record journal
window, current 3J behavior remains `ResnapshotRequired` / peer-local failure;
this task does not invent automatic rebaseline or a second semantic truth set.

## Canonical 180 MiB workload

The workload is:

```text
480 offered opportunities
* 16 already-known objects
* 24 KiB Name value
= 7,680 journal records
= 180 MiB raw value history
```

It is below the 16,384-record hard journal window, so it is a valid finite bounded
overload. However, its 180 MiB raw history is not semantically necessary retained
work under the existing GRPL state-projection and 3J coalescing semantics.

The measured remainder is:

```text
7,248 records * 24 KiB = 169.875 MiB
169.875 MiB / 2 MiB/s = 84.9375 s
```

Thus the old 20-second complete-convergence requirement was impossible for the
current replay behavior even with perfect transport feedback and no packet delay.
That explains the remainder without attributing a starvation defect, gameplay
defect or numeric-profile contradiction.

**Canonical classification: C — production is retaining semantically redundant
history that existing safe coalescing semantics permit it to collapse.** It is
also finite bounded overload in the descriptive sense of A, but A is not the
root cause of the failed fixture: the raw 180 MiB is not the semantic work that
must be delivered.

## Hard retained-work envelope

A conservative byte-service upper bound can be derived without assuming average
traffic. Treat every retained journal record or pending transition as potentially
requiring its own maximum serviceable complete group G. This is deliberately loose
but mechanically derived from existing limits.

Per peer:

```text
journal:     16,384 * 512 KiB = 8 GiB
transitions: 65,536 * 512 KiB = 32 GiB
combined:                         40 GiB
```

Across the selected 32-peer session:

```text
journal fanout: 16,384 * 32 * 512 KiB = 256 GiB
pending transitions: 1,048,576 * 512 KiB = 512 GiB
combined aggregate service work:                 768 GiB
```

At the accepted guaranteed rates:

```text
40 GiB / 2 MiB/s   = 20,480 s = 5 h 41 m 20 s
768 GiB / 64 MiB/s = 12,288 s = 3 h 24 m 48 s
```

Adding the conservative 20-second service-recovery allowance plus 250-ms credit
warmup and 220.5-ms fairness term yields a worst selected-profile envelope of
approximately **20,500.4705 seconds = 5 h 41 m 40.4705 s**.

That number is operationally absurd as a normal recovery target, but it is the
honest consequence of combining the current hard semantic-retention ceilings
with the accepted 2 MiB/s per-peer service floor. It proves that a universal
20-second complete-convergence promise cannot be derived from the current hard
limits. It does **not** require increasing rates or reducing safety bounds: normal
qualification should measure realistic semantic retained work, while the hard
ceiling remains a fail-closed resource envelope.

## Backpressure placement

Backpressure does not belong before authoritative DataModel mutation merely
because one peer is slow. The authoritative mutation and bounded journal remain
server truth/delivery evidence.

Existing boundaries already provide the correct containment points:

- journal retention stays bounded and eventually returns `ResnapshotRequired` to
  a lagging consumer rather than growing without limit;
- pending Enter/Leave state stores current peer-object relationships, not encoded
  history;
- one prepared frame and one next pooled pending group prevent serialization from
  running arbitrarily ahead of transport admission;
- pooled admission refuses another obligation while a peer owns pending/reserved/
  accepted structural work;
- exact retirement, not elapsed time, reopens structural debt capacity.

Safe current-state property coalescing belongs in the journal-to-frame derivation
path because that layer already owns current authoritative template lookup and
journal cursor representation. It must not discard DataModel state or mutate the
journal globally for the sake of one peer.

## Fairness, freshness and gameplay

The revised convergence contract keeps the accepted fairness model unchanged:
finite rotating peer service, one grant per peer, four active grants, large-waiter
earmark, generation-safe cleanup and no small-vs-large starvation.

A large retained backlog does not create a new priority class. Existing 3J policy
already alternates materialization transitions and journal work, keeps critical
structural work ahead of ordinary work, and preserves FIFO age inside a class.
If later evidence shows newly relevant ordinary state can be starved despite
those rules, that is a separate scheduler-policy issue; this contract does not
invent freshness priority.

The accepted FIFO gameplay proof also remains unchanged. One G structural group
followed by the qualified 20 KiB peer gameplay burst drains through the required
16 MiB/s grant floor in approximately 32.47 ms, leaving the accepted nonqueue
allowance inside the RPC/Event gates. Historical retained work cannot bypass the
one-grant-per-peer rule and become an unbounded FIFO prefix.

## Executable proof

`tests/PooledReliableServiceRecoveryContractFixture.hpp`, invoked by
`gargantuan_networking_contracts`, mechanically checks:

- canonical 7,680 records remain below the 16,384-record journal bound;
- 180 MiB raw history requires 90 seconds at 2 MiB/s;
- the measured 7,248-record remainder represents 169.875 MiB / 84.9375 seconds
  of raw replay at the same rate;
- the conservative hard retained-work ceilings are 40 GiB per peer and 768 GiB
  aggregate;
- the resulting workload-derived hard convergence envelope exceeds 20 seconds;
- one final Name update for each of the 16 canonical objects is encoded and
  decoded through the real GRPL codec with exact object identity/property/value,
  and must fit one accepted complete group;
- the existing G + 20 KiB gameplay follower still fits the funded 50-ms queue
  window;
- repeated bounded canonical overload/recovery cycles return semantic retained
  work to zero rather than accumulating historical debt.

Hosted validation for the proof source is recorded separately when terminal.
Until then, executable proof result is **not measured**.

## Effect on the stopped production checkpoint

The production pooled admission/debt work in `d2742796f` is not rejected by this
reconciliation. Exact retirement, generation cleanup, debt conservation, grant
bounds and gameplay evidence remain applicable at their measured scope.

The next implementation change is narrow: make known-object coalescible scalar
property history, including the special `Name` field, collapse to current
DataModel/catalog state within existing ordered barriers instead of serializing
every superseded mutation. Preserve complete-group atomicity and
acceptance-only cursor/Known commit. Then rerun the canonical structural/mixed
overload fixture using:

1. the unchanged 20-second **service-recovery** gate;
2. measured semantically necessary retained bytes at demand cessation;
3. the workload-derived **structural-convergence** bound above;
4. unchanged gameplay, fairness, lifecycle and debt-conservation checks.

Do not resume physical qualification until that corrected production checkpoint
is green. KI-006 remains open, Foundation 3L remains **B — PARTIALLY READY**, and
Foundation 3M remains **BLOCKED / NOT STARTED**.
