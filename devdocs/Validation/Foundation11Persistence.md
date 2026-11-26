# Foundation 11 persistence qualification

## Starting points

| Repository | Qualified starting reference |
| --- | --- |
| Engine main | `0be849f24549167e21fda905c647212d3e4cc199`, the Foundation 10B promotion tree `650a5fe81b6144c846472bc41dec00e7895132ba` |
| Node master and DataStore v1 protocol | `cd6294818670f3ddad5a4d93d949c711e495ec22` |
| Studio main | `0934af6db45faed2e55e6bc9fa31255b33ce5dd6` |

Foundation 11 adds a provider-neutral Engine consumer and Studio local Play
data workflow. It carries forward accepted Foundation 10B evidence for
unchanged Remotes, Instance identity and deadline contracts. Targeted gates
cover the newly registered server-only service, script safe-point publication,
provider composition, EditorHost behavior and offline startup.

## Independent boundary assessment

The independent source review examined the Engine provider boundary and
the retained userdata, native completion and cancellation paths. It found
four issues corrected in the candidate source. Completed reads now reserve
result capacity through observation, while successful Lua payloads are
consumed once. Callbacks without an attributed managed Script now share a
separate eight-operation quota. Decimal revision length is checked before
native parsing. The binding roots the operation userdata before dispatch to
preserve its acknowledgement channel across a Lua allocation failure.
Provider access rechecks the active server domain, execution context and
runtime identity on each handle use.

The review identified two narrower lifecycle dependencies. The main
scheduler owns Script attribution for the original managed coroutine.
Derived callbacks whose owner is unavailable use the bounded runtime-owned
pool and require explicit cancellation for departed Players. Foundation 11
does not introduce provider-driven Lua continuations. Native providers also
depend on bounded OS and network operations while the process joins its at
most two executing requests during shutdown.

## Recorded qualification

| Boundary | Evidence |
| --- | --- |
| Memory and opt-in SQLite | Standalone Windows MSVC focused suites passed, including repeated concurrent first access and corrupt file preservation. Pinned SQLite 3.54.1 recovered 24 acknowledged 60 KiB values and exact revisions after termination of a separate writer with a 1,859,080-byte hot rollback journal. Recovery took 21.45 ms in the final standalone run, followed by a successful revision 25. A 50 ms request stalled by another process's write lock returned Deadline at 57.33 ms. |
| Node | Scoped PostgreSQL 17.10 conformance and native TLS fault tests passed. The adapter observed old-credential rejection after a Node restart and recovered acknowledged bytes and revision after credential rotation and a separate PostgreSQL process restart. Independent Luau processes attempted the same create-only revision concurrently. One won, one observed conflict, and both reloaded the single reward. A loopback TLS fixture discarded the acknowledgement after a real PostgreSQL commit. The Luau sample reconciled the reward with one mutation attempt and one commit. |
| Studio | Release Host, Play Data and toolbar Luau checks, and the .NET 10 EditorHost driver were verified. The driver used a sealed 94-file Engine closure and covered disposable state, document and binary recovery across a fresh host, copied-project isolation, stale reset, failed local storage and unchanged authored files. These journeys cover the headless native protocol and workflow logic. |
| Engine | The Windows MSVC 19.51 Release closure with GNS, Node and SQLite passed six focused CTests, including provider/runtime tests, PreRun EditorHost and both real-GNS Remote and GameSession tests. The Linux Clang 19 ASan, UBSan and LSan build with SQLite passed the persistence runtime, local provider and PreRun EditorHost checks. The sample also covered committed-but-unknown reward reconciliation in a deterministic native fixture. |

The Windows Release timing fixture ran ordinary Luau document put/get with
4 KiB documents. A 20 ms provider delay produced a Script step p50/p95/p99
of 15.9/70.9/124.4 microseconds in the sample, with persistence pump
p50/p95/p99 of 1.0/4.4/6.0 microseconds. The pump issued zero C++ allocations
over 191 measured calls. The fixture measures Script steps and native C++
allocations, excluding VM allocator work and full game-frame latency.

The PostgreSQL query benchmark exercised selective and unfiltered reads of
32, 256 and 1,024 documents, ten repetitions per case. At 1,024 documents
the selective case matched 64, while SQL still returned 1,024 candidates
and recorded 48 shared buffer hits. It took 7.40 ms per query and allocated
about 2.14 MB of Go memory per operation. This warm, bounded fixture exposes
the remaining full-candidate cost without estimating production capacity.

Release qualification covers the isolated Windows and Linux environments
recorded in the source-pinned supervisor receipt. Studio qualification is
limited to the local Play workflow and EditorHost boundary. Existing
physical networking, commercial authentication, managed deployment and
client-audience confidentiality evidence retain their original scope.

## Independent disposition

The final source review found no blocking persistence defect in this bounded
private engineering envelope. Native providers own adherence to their
allocation and deadline contracts. Engine moves before the Studio candidate
pin when promotion is separately authorized. The current Node DataStore v1
protocol is sufficient for Engine integration. The Node candidate adds
qualification and documentation without changing the deployed Node service.

## Handoffs

- **Verified account binding.** Thread a bounded provider assertion through
  authenticated GameSession admission before Player allocation, normalize
  provider and subject, and test replay, outage and cross-server rejoin.
- **Entitlements.** Preserve stable trusted Player binding and distinct
  unavailable/denied outcomes when a production host selects its adapter.
- **DocumentStore.** Qualify indexed scalar-path pushdown with number,
  null/missing, nested field, ordering and stale-cursor parity before
  changing the configured collection ceiling.
- **Content V2.** Preserve canonical whole-artifact admission and tenant
  publication authority. Scope private host ingestion and bounded,
  digest-checked resume separately from public client visibility and the
  Foundation 10A package confidentiality gate.
- **Creator distribution.** Keep local Play data outside project and
  package serialization. Seal an exact Engine/Studio pair only after its
  native journey passes and preserve all historical profile receipts.
