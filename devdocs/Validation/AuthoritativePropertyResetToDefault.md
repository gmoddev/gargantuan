# Authoritative reset qualification

Source base: `feature/concrete-property-defaults` at
`5035d00d19e325058a3fcdb57d143057142e8371`.
Implementation branch: `feature/authoritative-property-reset`.
Status: implemented; hosted final-source qualification pending.

See the [current contract](../CurrentArchitecture/AuthoritativePropertyResetToDefault.md)
for eligibility, runtime versus authoring history, discovery selection and
unsupported cases. Studio reset UX is not implemented.

The existing `gargantuan_concrete_property_defaults` fixture now also exercises:

- native reset of every migrated GUI override, an inherited TextLabel subclass,
  nearer TextBox/TextButton overrides, and declaring Frame/Part fallbacks;
- normal enum/scalar no-op, declaring identity, revision, post-commit notification
  and ordinary journal publication;
- read-only/transient/contextual, custom Name/custom property, destroyed object
  and wrong execution domain rejection without state or publication changes;
- actual production `GetSchema` v7 values feeding semantic single `SetProperty`
  with history, Undo/Redo and no-op; stale concrete identity rejects;
- actual `SetPropertyBatch` over TextLabel and TextBox defaults 1/0, common/mixed
  current values, some and all already-default selections;
- 256 heterogeneous resets in one action, atomic observer visibility and replay,
  normal replication consumption, and 257-write rejection without chunking;
- whole-request stale declaring version/revision, unsupported target and old
  project-scope denial; parameterless v6 compatibility and explicit v7 discovery.

The prior 179 construction/discovery cases, 15 persistence/Clone scenarios,
negative schema cases and 26 classgen checks remain. Full Native CI also runs
existing mutation/history/journal, EditorHost, SetPropertyBatch and prepared
failure-injection regressions. GNS runs only as the normal push workflow.

Local checks: class generation and all 26 classgen cases pass. MSVC syntax probes
and documentation/link/whitespace validation are supplemental. The trusted
dockerbox SSH worker timed out; no sustained local build is substituted.

No reset-specific storage, transaction or wire representation is added. Explicit
v7 selection adds only the small read request field; v7 payload and sparse default
storage retain the [qualified foundation bounds](ConcreteClassPropertyDefaults.md).
Legacy v6 omits the 3,695-byte override projection. No default-based persistence
elision is introduced.
