---
status: current
owner: repository
last_verified: 2026-08-14
---

# Gargantuan architecture documentation

This index classifies Gargantuan's engineering documentation and routes readers
to the smallest relevant set. It establishes documentation authority; it is not
a complete description of every subsystem.

## Documentation authority

### Current architecture

Current architecture documents describe implementation that has been verified
against reachable code and tests. They contain no unmarked aspirations. When a
current document mentions deferred work, that section is explicitly
non-normative.

### Invariants

Invariant documents define properties that changes must preserve regardless of
implementation. Tests should enforce them where practical. A change that intends
to violate an invariant requires architecture review and usually an ADR.

### Architecture decision records

ADRs explain why a consequential architecture decision was accepted. They are
append-only history: later decisions supersede earlier ADRs rather than rewriting
them. An accepted ADR may lead implementation, so it is not evidence that every
described facility already exists.

The repository does not yet have a canonical `docs/adr/` series. Until that
series is introduced, documents explicitly labeled accepted design direction,
including the game networking architecture, have ADR-like authority.

### Protocol specifications

Protocol documents define exact versions, fields, limits, ordering, malformed
input behavior, and compatibility rules. Product motivation belongs in an ADR or
architecture document instead. Existing EditorHost, snapshot, and journal
documents are interim protocol sources until moved into `docs/protocols/`.

### Roadmaps and proposals

Roadmaps, future architecture, research, and proposals describe work that may
change. They do not define current engine behavior. An implementation task must
not treat an example interface from a proposal as an accepted public contract
unless an ADR or current document says so.

### Historical evidence

Audits and deprecated documents describe a particular revision or an obsolete
design. They remain useful for provenance and risk discovery, but never override
verified current architecture. The original `devdocs` audit is anchored to
commit `e4fca3575cc84c0d5fa4a946b88bf528aac2223b` from August 12, 2026.

## Conflict handling

Use this evidence order when sources agree:

1. tests and mechanically enforced invariants;
2. verified current architecture and invariant documents;
3. accepted ADRs;
4. versioned protocol specifications within their protocol scope;
5. source implementation evidence;
6. roadmaps, proposals, and research; and
7. historical audits.

This is not permission to ignore a conflict. Source and tests may reveal stale
documentation, while an ADR may reveal that current source is transitional or
violates an accepted boundary. Identify discrepancies explicitly and resolve
them in scope rather than silently selecting one source.

## Subsystem ownership map

| Subsystem | Owns | Must not own |
| --- | --- | --- |
| DataModel | Live object graph, services, hierarchy, identity scope | GPU resources, Studio UI state, transport policy |
| Runtime schema | Stable class/member identity, reflection metadata, persistence/replication/editing policy | Live Instance state, implicit privilege |
| Mutation | Validated authoritative commands and committed changes | Network transport, Studio projections, rendering |
| Change journal | Ordered committed authoritative history per scope | Peer relevance, packet ordering, transport acknowledgement |
| Persistence | Versioned bounded project state and schema-aware references | Pointer identity, runtime-only GPU/native handles |
| Replication | State projection and transfer to a receiver or peer | Authoritative gameplay decisions, direct unvalidated mutation |
| Scripting | Luau VMs, execution contexts, native bindings, scheduling | Ambient host authority not explicitly granted |
| Rendering | Immutable extracted render state, renderer-owned GPU resources | DataModel traversal or mutation, retained Instance pointers |
| Physics | Neutral rigid-body semantics, safe-point updates, backend identity and events | Backend-native handles outside the adapter |
| EditorHost | Versioned editor-facing engine boundary, commands, snapshots, journals, viewport transport | First-party Studio UX state, private engine pointer exposure |
| Studio | Editor UX and non-authoritative document projections | Authoritative DataModel state, raw engine/GPU ownership |
| Game networking | Connection, scheduling, delivery, remotes, peer-specific replication intent | Authoritative source history or platform-service policy |
| Hosted node | Discovery, authentication, matchmaking, persistence and platform services | Per-frame authoritative simulation state |

## Current architecture routing

The following documents describe recently implemented, source-verified slices:

- [Runtime schema](../src/content/docs/developing/runtime-schema.mdx)
- [Render extraction](../src/content/docs/developing/render-extraction.mdx)
- [Physics backend](../../devdocs/CurrentArchitecture/PhysicsBackend.md)
- [Instance attributes](../src/content/docs/developing/instance-attributes.mdx)
- [Instance tags](../src/content/docs/developing/instance-tags.mdx)
- [EditorHost and Studio boundary](../src/content/docs/developing/editor-host.mdx)
- [Runtime foundation](../../devdocs/CurrentArchitecture/FoundationRuntime.md)
- [Mutation gateway](../../devdocs/CurrentArchitecture/MutationGateway.md)
- [Snapshot baseline](../../devdocs/CurrentArchitecture/SnapshotBaseline.md)
- [Loopback replication](../../devdocs/CurrentArchitecture/LoopbackReplication.md)
- [Networking contracts](../../devdocs/CurrentArchitecture/NetworkingContracts.md)
- [Deterministic simulated transport](../../devdocs/CurrentArchitecture/SimulatedTransport.md)
- [Network scheduler contract](../../devdocs/CurrentArchitecture/NetworkSchedulerContract.md)
- [Networking foundation validation](../../devdocs/CurrentArchitecture/NetworkingFoundationValidation.md)
- [Real game transport](../../devdocs/CurrentArchitecture/RealGameTransport.md)
- [Script security](../../devdocs/CurrentArchitecture/ScriptSecurity.md)
- [EditorHost protocol](../../devdocs/CurrentArchitecture/EditorHostProtocol.md)
- [Editor viewport](../../devdocs/CurrentArchitecture/EditorViewport.md)

These files are candidates for gradual migration into `docs/architecture/` and
`docs/protocols/` when their subsystem is next changed. Moving files alone is not
a goal; each migration must verify claims against current code and tests.

## Accepted and future direction

- [Future architecture](../src/content/docs/developing/future-architecture.mdx)
  is the broad accepted direction, with implementation status stated per section.
- [Game networking architecture](../src/content/docs/developing/networking-architecture.mdx)
  records accepted boundaries and the evidence-backed GNS transport selection;
  gameplay networking layers remain deferred.
- [Roadmap](../src/content/docs/developing/roadmap.mdx) is non-normative ordering.
- Files under `devdocs/FutureArchitecture/` are design input and must be
  reconciled with newer accepted documents before implementation.

## Invariants

Start with [Core invariants](../invariants/Core.md). Subsystem-specific invariant
files should be added only when a subsystem has enough independent constraints
to justify them.

## Document metadata

New engineering documents should begin with small machine-readable metadata.

Current architecture:

```yaml
---
status: current
owner: runtime
last_verified: 2026-08-14
related_code:
  - include/gargantuan/runtime/
  - src/runtime/
related_adrs: []
---
```

ADR:

```yaml
---
status: accepted
date: 2026-08-14
supersedes: null
superseded_by: null
---
```

Roadmap or proposal:

```yaml
---
status: planned
authority: non-normative
---
```

Only update `last_verified` after comparing the complete document with current
implementation and tests. A date changed mechanically without verification is
worse than no date.

## Documentation lifecycle

```text
Research or proposal
    -> accepted ADR
    -> bounded implementation
    -> current architecture update
    -> tests enforce invariants
    -> superseded design remains as history
```

Architecture-affecting code and its documentation are one change. If another
engineer could make an incorrect future implementation because a new ownership,
lifecycle, protocol, authority, capability, or failure rule is undocumented,
the change is not complete.
