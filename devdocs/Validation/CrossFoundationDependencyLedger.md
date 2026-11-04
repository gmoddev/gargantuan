---
status: integration-in-progress
owner: cross-foundation-integration
last_verified: 2026-11-04
---

# Cross-foundation dependency ledger

This campaign composes independently qualified geometry and streaming sources
in a separate integration branch. Composition alone is not qualification of
their combined behavior. Neither owner branch nor main is changed.

| Dependency | Exact source | Status and use |
| --- | --- | --- |
| Asset 2B / Geometry 1A, Engine PR14 | `30750f88bb76cd5d363eb84f72e117ce1446f940` | Verified current head; all three hosted CI jobs successful; composed as an explicit merge parent |
| Foundation 3M, Engine PR13 | `470c9c7d51eaeddac107a279560581032bc8265c` | Verified current head; preserved ancestor of PR15 |
| 3M streaming readiness, Engine PR15 | `96226fc4d46581154e02a71486a36c26766ec361` | Verified current head; Windows113/Linux69/GNS11 qualification; starting integration parent |
| Common ancestor of PR14 and PR15 | `e682be08df84d776bfa455137728265c6494b5e8` | Documented common source; original histories preserved |
| Studio PR8 | `d20aa9868b3b651809b359660b8d16d5e8eea43f` | Verified open in gmoddev/gargantuan-studio; presentation dependency only, no source imported |
| Studio PR9 | `d1016d98eee2536dd798c83c4763cefed1d4a327` | Verified open in gmoddev/gargantuan-studio; presentation dependency only, no source imported |

The integration checkout is
`C:\Users\aiden\.codex\worktrees\cross-foundation-integration`, branch
`integration/geometry-streaming-authoring`. PR14 and PR15 are open draft
dependencies, not merged to main. No Studio/LUI source is composed. Studio
authoring interfaces remain a separate repository/campaign dependency.

Composition commit `96a88d9e68b1e9b8bca54fc8d26e93b22ac69d29` preserves
PR15 and PR14 as its two direct parents. The initial merge resolves only the
adjacent KNOWN_ISSUES additions by retaining
both KI-3M-001 and KI-2B-001 unchanged. All other merged files are ordinary Git
composition of these pinned histories. Integration-specific implementation,
reviews, test results, performance and source identities will be recorded here
as they are established. KI-3M-001 remains open until the authoritative producer
and actual package/consumer regression are implemented and qualified.

Worker preparation is local only. GodotLuau holds the current independently
coordinated lease; this campaign has no active reservation, transfer or remote
process. A new private Sandbox namespace, fixed build recipe and explicit
checked handoff are required before sustained worker execution. Compiler
parallelism is capped at two; historical physical certification is out of scope.

## Campaign coordination

The human supplied the same integration brief to the existing Studio chat.
The chats agreed to keep this one implementation lineage and one draft PR.
Implementation and its owned producer/diagnostic/E2E files remain here.
The Studio chat independently prepares producer/consumer/lifecycle/performance
qualification fixtures and review in its own checkout, sending isolated patches
for composition. It owns this campaign's worker recipe/reservation/execution;
this implementation root and its agents perform no parallel remote operations.
Any qualifying source is frozen to an exact commit/census before transfer.
