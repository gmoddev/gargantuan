---
status: accepted-qualification-pending
owner: runtime-networking
reviewed: 2026-10-11
supersedes: D01/F1 hard packet timing and implied continuous delivered capacity
---

# D03 — Product service, exact structural permits and independent progress

## Decision scope

The authorized Foundation 3L follow-up adopts D02's B/C direction: retain
funding, exact delivery and hard player outcomes; qualify actual source-shaped
capacity. The coordinated deterministic/native Windows preflight passed eight
affected suites; this record is explicitly adopted with implementation.
Adoption is not Foundation 3L PASS.
Historical failures retain their original source, contract and verdict.

## Hard conjunction

1. **Native structural completion.** For every Ready, generation-valid accepted
   complete grant, `0 < W <= G=524288 B`, preserve its scheduler acceptance and
   activation boundary. Require all unique structural BODY bytes at positive
   native send within the exact uniform budget:

   ```text
   R=16777216 B/s; Q=1248 B
   R*(t_complete-t_activate) <= R*6000+(G+Q)*1000000
                            =626199296000 byte-us
   T_max=37324.3865966796875 us
   ```

   Integer 37324 passes; 37325 fails. Tiny grants receive this same maximum.
   Missing first send and missing last bytes fail at a complete native evidence
   frontier. An unresolved worker result cannot publish PASS or an exact native
   lateness claim. Reconciliation uses the actual worker completion timestamp.
   Earlier incomplete evidence must not be retrospectively repaired.

2. **Protected ordinary FIFO.** A qualified ordinary reliable gameplay/control
   message must complete unique native BODY first-send within **50000 us of its
   original scheduler acceptance**, including any scheduler and GNS queue delay.
   Direct control has no scheduler prefix: its successful submission starts at
   transport entry. WouldBlock before acceptance creates no delivery obligation;
   WouldBlock after scheduler acceptance preserves the original timestamp.
   Retries do not count as new service. All phases 1–5, both directions, complete
   exposed prefixes and continuing arrivals must be covered. RPC p95/p99/max
   remain 150/250/500 ms; Event ACK/action maxima remain 250 ms. Dependency waits
   never restart invocation clocks. No stage receives another 50-ms allowance.

3. **Exact turnover and completed opportunity.** Select **150000 us** as a new
   combined structural turnover entitlement, from activation through exact
   structural ACK/reference retirement, fresh Main reconciliation, actual
   permit return and completion of the admission pass. Select **100000 us**
   for a completed Main reconciliation/admission opportunity when source work
   is ready or timed ownership remains. These are explicit product class
   allocations using the existing 50-ms queue plus 100-ms nonqueue allocation,
   not measurements or inferred GNS ACK/OS guarantees. No extra 100 ms follows
   the 150-ms turnover. First opportunity is armed before preparation.
   A missing ACK never retires debt, regardless of elapsed time.

4. **Fair opportunity.** For the declared 32-peer, four-slot Ready workload,
   with complete groups ready to prepare under the qualified production read
   and operation budgets, require admission of every independently eligible
   peer within:

   ```text
   completed initial opportunity 100000 us
   + conservative 4G global-credit warmup 31250 us
   + seven exact slot turnovers 7*150000 us
   =1181250 us
   ```

   Empty peer credit has its separate maximum G warmup of 250000 us.
   Existing incomplete/invalid feedback and insufficient funded room never
   become eligibility by retimestamping. Source preparation, accepted grant,
   first native send, ACK, retirement and client application remain distinct.
   This explicitly replaces the old mechanically derived 220500-us promise.
   A different valid source configuration is not automatically qualified:
   offered-work feasibility and peer-by-peer progress require source evidence.

5. **Independent finite recovery outcomes.** Keep the 20-second service
   recovery maximum and the existing C3/C5 immutable cessation reference,
   exact quote, source/semantic coverage, finite accepted cut, moving relevance,
   causal replay and original numerical convergence deadline. Its old rate
   terms become an independent reference policy, not a universal delivered
   lower bound derived from credit. The 150/100-ms class does not mathematically
   imply that byte-only deadline for every legal tiny/mixed count distribution.
   All hard canonical outcomes must still pass on the adopted source.
   Live quote frame count is not a future count bound under legal divergence.

## Funding and capacity

Keep all existing rates, credit caps, reserves and resource limits:
96-MiB/s backend funding; 64-MiB/s structural funding/refill; 8/4/8-MiB/s
gameplay/control/transport reserves; 2-MiB/s peer refill; four structural
permits; one accepted grant per peer; G peer pending and 32G global pending;
4G accepted structural debt; 50-ms feedback freshness and one-second bounded
requalification. Credit and configured send rate are not delivered capacity.

The 16-MiB/s design value continues to fund prospective serialization and
defines the uniform maximum above. There is no additional default 31.25-ms
post-first G gate or continuous 16/64-MiB/s delivered entitlement. A 14-MiB/s
peer can serialize G in 35.714286 ms and G+20 KiB in 37.109375 ms; these are
feasibility arithmetic, not qualification. It may qualify only if every actual
hard outcome passes. A 14-MiB/s **aggregate** sender needs 142.857143 ms for four
simultaneously activated G groups and fails the uniform deadline. This is a
meaningful capacity negative without inventing a minimum legal grant size.

Repeated waves must include actual Main opportunities, group sizes, preparation,
fair rotation, ACK/reference lifetime, ordinary traffic, credit and regrant.
Offered demand below the required source workload, withheld final bytes/ACKs,
growing backlog, excluded peers or a late immutable recovery prefix cannot PASS
through averages or a favorable configured rate. Exact byte conservation and
resource bounds are checked throughout and at cleanup.

## Structural ownership correction

`ReliableByteAdmission::RefreshService` returns a permit only after exact
generation/token/byte retirement and complete, fresh Available feedback.
Unrelated ordinary ACK debt no longer owns that permit. Ordinary bytes remain
independently funded; native first-send, timeout, stale/incomplete evidence,
mismatched tokens and unaccounted disconnects do not return capacity.
Terminal cancellation and accounting remain explicit and separate.

Ordinary native-unsent bytes use a new exact unique BODY frontier, not ordinary
ACK debt. The accountant reuses its existing exposure word for this prefix;
its compact-state bound is unchanged. Before accepting a structural grant it
reserves the uniform G completion budget plus 20 KiB gameplay and the complete
64 KiB control burst in the 50-ms prospective peer window:

```text
ordinary_unsent_limit
=floor((R*50000-626199296000)/1000000)-20480-65536
=126645 B
```

This conservative resource eligibility check is not proof of actual FIFO
service. A large native-unsent prefix denies new structural exposure even if
global funding fits; the same already-first-sent ordinary ACK debt does not.
The independent global ordinary funding and reliable FIFO order stay intact.

## Independent journal and grant rotation

A shared cursor can be redirected by nonemitting journal reads while all slots
are held. The deterministic supported 128-record peer/512-record global pattern
uses the actual geometric read tail. Its acceptance step advances 12 positions;
six held steps advance 14 each. `12+6*14=96=0 (mod 32)`: every seven-step
turnover returns to the same four peers, starving 28 despite finite ACKs.

POOLED_SERVICE now rotates admission only after the last accepted grant.
An independent cursor rotates the existing nonemitting journal pass, using the
same bounded connection list and unchanged read caps. No payload queue or new
scheduler is introduced. The supplied corrected schedule serves all 32 peers
by 816683 us with exact retirement, four permits, 308 B debt high and identical
84*512 journal charges. This is a deterministic schedule, not a host measurement.
The previously qualified recovery encoder correction remains intact and its
historical receipt remains valid at its recorded revision.

## Health and evidence migration

Keep legacy F1 arithmetic and histories intact as nonfatal diagnostics only
under the explicitly adopted `HYBRID_F1_V1`. Native completion integrity,
native-positive truth, chronology, security, reliability and generation/lifetime
errors remain hard. Native, adapter and GameSession use the separate hard
completion fields together; qualified FIFO failures fence new structural service.
Execution/turnover breaches use existing structured failure and terminal
accounting, never time-based retirement.

Evidence readers require explicit contract identity. Legacy `finite_grant_v1`
and admission V1/V2 remain strict. New admission V3 identifies HYBRID_F1_V1.
Grant certificates require exact counts, completion, retirement and completed
Main opportunity/turnover identities; zero or missing evidence is not success.

The missing H2 acceptance fact is supplied by existing positive segment hooks:
qualified origin/timestamp POD follows the existing reliable message; unique
BODY intersections advance only after positive native send. A fixed 40-entry
qualification metadata table retains up to 20 timely completed messages in
one unresolved packet and 20 currently awaiting completion under the declared
16-message burst/64-per-second/50-ms class. It assumes no actor reconciliation
bound, owns no payload/reference and cannot change delivery. Overflow makes
qualification incomplete/invalid. Bootstrap and legal deliberate overload
remain outside fixed ordinary phases, observable under their existing gates.

All 32 server connection certificates and 32 client certificates, across
33 processes, must join exactly to the existing successful ordinary demand
denominators, including direct controls and all original qualified queued
work. Native pending results and absent/unknown contracts cannot certify FIFO.
Source and observer overhead remain measured separately where available;
unmeasured instrumentation perturbation must be disclosed.

## Required qualification before closeout

1. Deterministic ownership, invalid evidence, boundary, fairness, FIFO,
   source/capacity and recovery negative regressions.
2. Affected Windows native, pinned real GNS and Linux sanitizer suites;
   all required exact-source hosted jobs.
3. One fresh bounded native32 campaign with actual offered shapes and all hard
   conjunctions. Correct only demonstrated defects; retain failed evidence.
4. Clean resource/link/capture/credential preflight, canonical Local32 and
   real-TLS Node32, both endpoint capture/resource receipts and verified cleanup.
5. Independent original Foundation 3L acceptance ledger and provider parity.

Until these receipts pass, KI-006 stays OPEN and Foundation 3L remains partially
ready. Keep PR #6 draft. No merge, release, Foundation 3M or historical relabeling.

### Initial coordinated preflight receipt

Base source `4eea2307ce8bf78e3f7a8354d5cb672dcd677dc8` plus the task changes:
MSVC Release, existing pinned GNS/cache, four build jobs; eight affected CTests
PASS in 71.48 s. Networking production has 28 cases, the historical/corrected
capacity schedules 13, native finite curves 19 and feedback 20. The first
preflight failed the unchanged compact accountant memory bound after adding
an exposure word; the implementation now reuses the existing word and the
same bound passes. Both logs and JUnit are retained at
`C:\Sandbox\Codex\Artifacts\foundation3l-permit-ownership` and the corresponding
local `C:\Users\aiden\.codex\artifacts\foundation3l-permit-ownership` directory.
This dirty-source preflight does not replace exact-source hosted CI or a physical
receipt. Additional queue-watchdog regressions and final source gates follow.

### First committed-source CI and supervisor correction

Native source `7c1be009fdbfcbe0857824393f8615a9c01a4225` preserves the prior
deterministic commits. Both original GNS sanitizer workflows passed:
push `38192457367` and PR `38192459467`. The push collection verified the
exact checkout marker and 11 original JUnit cases. This is partial CI evidence.
Windows push `38192457355` passed 95/96 CTests; the endpoint fixture failed
with `redirected output did not drain` after its harmless 32-child sampler
observation. Its dependent Linux job was skipped, so the required CI
conjunction did not pass. Native32 has not launched on this revision.

A bounded isolated Windows counterexample occupied the shared worker pool,
exited the owned child and retained its real anonymous pipes. `CopyToAsync`
missed the existing 5000-ms drain wait; the existing dedicated
`PhysicalFarmPipeDrain` completed both streams within that unchanged wait and
preserved exact stdout/stderr bytes. This demonstrates the implementation
mechanism; the original CI did not instrument pool saturation, so attributing
its precise scheduling state remains inference. The endpoint correction reuses
that helper and pins/stages its dependency. It changes no networking contract,
shutdown timeout or native implementation. A new exact-source CI conjunction
is required before the single fresh native32 campaign.

Correction preflight: endpoint fixture PASS in 21.49 s; isolated starvation
and the existing 33-child/66-stream pipe test PASS in 9.22 s; analyzer inventory
PASS; final acceptance reader 39/39 PASS; existing outer staging mocks 48/48
PASS. The isolated fixture waits for completed pipe writes before occupying
the pool and uses a child blocked on its own redirected stdin, avoiding an
unrelated child-start or descendant-spawn race. Its intermediate fixture
quoting failure remains retained. This observer-process correction does not
attribute the historical native initial-service failure.

Logs and original JUnit files are retained at
`C:\Users\aiden\.codex\artifacts\foundation3l-endpoint-pipe-attribution`.

Retained failure log SHA-256:
`7f6a83570750e1b0c5639d2a2554d96d044b3d9568c6db5dc26b925f4dc29ce9`,
at `C:\Users\aiden\.codex\artifacts\foundation3l-permit-ownership\native-push-38192457355-failure.log`.
Historical failures and the qualified recovery correction remain unchanged.
