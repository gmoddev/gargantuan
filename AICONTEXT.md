# Gargantuan AI context

This is the fast orientation for AI-assisted work. It intentionally summarizes
the verified architecture; it does not replace the current architecture,
invariant, protocol, or accepted-design documents.

## Start here

1. Read [`docs/src/content/docs/meta/agents.mdx`](docs/src/content/docs/meta/agents.mdx)
   for contribution policy and documentation authority.
2. Read [`docs/architecture/README.md`](docs/architecture/README.md) and
   [`docs/invariants/Core.md`](docs/invariants/Core.md) before a code change.
3. Check [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) for verified defects and gaps in
   the area being changed.
4. Load only the subsystem documents named in the routing table below. Do not
   read the whole repository or treat a directory name as an architectural
   contract.

## Non-negotiable model

```text
Authoritative DataModel / live Instance graph
    <- validated commands -> MutationGateway on Main
    -> committed ChangeJournal -> snapshots, replication, EditorHost
    -> RenderExtractor -> immutable RenderSnapshot -> renderer
```

- `DataModel` owns the live object graph, identity scope, authoritative
  mutation scope, and committed changes.
- `ObjectId` is slot plus generation. It is scope-bound, never pointer/name/
  path based, and stale IDs must not resolve after reuse.
- Authoritative external mutation must be bounded, authorized, validated, and
  committed through the mutation path. Rejection must not produce partial
  state or change records.
- `Main` owns authoritative Instance mutation. Worker computation returns
  results to `Main`; the graph is not generally thread-safe.
- Renderer, Studio, snapshots, replication receivers, and document caches are
  non-authoritative projections. They do not retain live Instance pointers or
  become a second source of truth.
- Runtime schema is one frozen, transactional registry. Schema metadata
  describes access requirements but grants no privilege.
- Script domains describe context, not trust rank. Native boundaries enforce
  explicit capabilities.

## Ownership and boundary map

| Area | Owns | Critical boundary |
| --- | --- | --- |
| DataModel | Live graph, services, identity, committed history | No GPU, Studio UI, or transport policy ownership |
| Runtime schema | Stable native/custom class, enum, and extension identity and reflection policy | Build candidate, validate, then freeze atomically |
| Mutation | Validated authoritative commands | Does not own transport or Studio projection state |
| Change journal | Ordered committed history per DataModel scope | Not a packet, peer, render, or RPC sequence |
| Rendering | Immutable extracted state and GPU resources | Never traverses/mutates DataModel; no Instance pointers in snapshots |
| EditorHost | Public versioned engine/editor IPC boundary | DTOs, stable IDs, schema, journals, commands, pixels only |
| Studio | Private editor UX and replicated document projection | Returns mutations through EditorHost; no engine pointers/GPU handles |
| Scripting | Luau VMs, contexts, bindings, scheduling | Capability checks remain at native/reflection boundaries |
| Persistence/replication | Bounded versioned projections | Never use native pointer identity or bypass authority |

## Subsystem routing

| If changing… | Read first |
| --- | --- |
| Any architecture-affecting code | `docs/architecture/README.md`, `docs/invariants/Core.md` |
| Instances, lifecycle, identity, jobs, changes | `devdocs/CurrentArchitecture/FoundationRuntime.md` |
| Property writes, commands, journal cursors | `devdocs/CurrentArchitecture/MutationGateway.md` |
| Schema/reflection/custom enums/class extensions | `docs/src/content/docs/developing/runtime-schema.mdx` |
| Attributes or tags | `instance-attributes.mdx` or `instance-tags.mdx` in the same directory |
| Rendering or viewport picking | `render-extraction.mdx` and `devdocs/CurrentArchitecture/EditorViewport.md` |
| Studio, EditorHost, snapshots, journals, viewport IPC | `editor-host.mdx` plus the relevant `devdocs/CurrentArchitecture` protocol document |
| Luau execution or permissions | `devdocs/CurrentArchitecture/ScriptSecurity.md` |
| Game networking | `devdocs/CurrentArchitecture/ProtocolInputHardening.md`, `NetworkingContracts.md`, `SimulatedTransport.md`, then `networking-architecture.mdx`; pure contracts and the in-memory simulator exist, but no real transport exists |

## Current implementation landmarks

- Schema lifecycle is `Bootstrap -> NativeRegistration -> CoreRegistration ->
  PreRunRegistration -> Validation -> Frozen -> Runtime`. PreRun is narrowly
  sandboxed for schema definition and remains capability- and phase-gated.
- Class extensions are distinct frozen definitions targeting an existing class
  ID. Their scalar properties use sparse per-Instance state through the mutation
  gateway; they do not change class identity or use Attributes as storage.
- Custom classes are canonical constructible class definitions with stable
  custom identity, stable inheritance, scalar declarative properties, and an
  engine-owned data-only host policy. The first approved host is `Engine.Folder`;
  project code cannot register native behavior callbacks.
- Attributes and tags are bounded dynamic Instance state, not schema changes.
  Their writes use mutation authority; tags maintain generation-safe indexes.
- Render extraction happens after simulation and `PreRender`; snapshots are
  immutable value data and use stable `ObjectId` for picking.
- EditorHost is the public boundary to the separately authored private Studio.
  Studio sees versioned DTOs and pixels, not private headers, engine pointers,
  renderer objects, or shared-memory handles in Luau.

## Change rules

- Use PascalCase for new applicable identifiers; preserve established API or
  local naming conventions. The narrow exception is
  `self = setmetatable({}, ...)`.
- Update architecture, invariant, ADR, or protocol docs in the same change when
  ownership, lifecycle, authority, boundary, format, capability, or failure
  semantics change.
- Add focused positive, negative, bound, and lifecycle tests. Keep unrelated
  work untouched; never stage, discard, or commit it.
- Update `KNOWN_ISSUES.md` when a verified issue is introduced, materially
  reclassified, or resolved.
- `devdocs/CurrentArchitecture/` is being reclassified. Verify the relevant
  document against code/tests before relying on it as current.
- Roadmaps, proposals, future architecture, and historical audits are not
  current behavior unless explicitly marked otherwise.

## When documents disagree

Do not silently choose one. Follow the authority order in
`docs/architecture/README.md`: tests and enforced invariants, verified current
architecture, accepted ADRs, protocol specifications, source evidence, then
future/historical material. Identify and resolve an in-scope discrepancy.
