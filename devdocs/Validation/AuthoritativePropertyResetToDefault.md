# Authoritative reset qualification

Source base: `feature/concrete-property-defaults` at
`5035d00d19e325058a3fcdb57d143057142e8371`.
Implementation branch: `feature/authoritative-property-reset`.
Implementation source: `14126fe35b6f33330a9b2b7dbe9d9b720187ba73`.
Status: authoritative native reset and reset-through-SetPropertyBatch qualified
on 2026-09-18 (UTC). **READY FOR STUDIO CONSUMPTION** for the documented native V1 surface.

[Native CI](https://github.com/gmoddev/gargantuan/actions/runs/35389483395)
passed all 60 Windows tests (job `105744233426`). The fixture reports
`NativeCases=20`, `EditorSingle=1`, `SelectionModes=4`, `MaximumWrites=256`,
and `RejectedWrites=257`. The dependent Linux ASan/UBSan/LSan job
`105756320172` passed all 52 tests with the same reset results and no sanitizer
or leak report. Both jobs generate the class sources from their fresh checkout.
The normally triggered [GNS sanitizer workflow](https://github.com/gmoddev/gargantuan/actions/runs/35389482259)
passed its five tests and gameplay/overload/recovery checks.

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
negative schema cases and 26 classgen checks also pass. Full Native CI also runs
existing mutation/history/journal, EditorHost, SetPropertyBatch and prepared
failure-injection regressions. GNS runs only as the normal push workflow.

Local checks: class generation and all 26 classgen cases pass. MSVC syntax probes
pass for Instance, EditorHost and the fixture. Astro builds all 19 pages; the
final documentation scan verified 100 relative Markdown links. Whitespace
validation passes. These local checks are supplemental to hosted execution. The trusted
dockerbox SSH worker timed out; no sustained local build is substituted.

No reset-specific storage, transaction or wire representation is added. Explicit
v7 selection adds only the small read request field; v7 payload and sparse default
storage retain the [qualified foundation bounds](ConcreteClassPropertyDefaults.md).
Legacy v6 omits the 3,695-byte override projection. No default-based persistence
elision is introduced.

KI-009 is resolved for this Engine surface. Studio remains unchanged. The next
task is to opt Studio into v7, validate/cache effective defaults by connection,
registry generation and exact class/property identity, and implement single and
whole-selection Properties reset controls through the existing authoring
operations. Qualify that consumer's no-op, stale-cache, mixed-default, limit,
Undo/Redo and journal reconciliation behavior before claiming Studio UX complete.

The subsequent qualification-documentation commit changes no production code or
tests. Repository policy still requires its own hosted Native CI run; the task's
final report records that exact final HEAD and run, without a self-referential
follow-up documentation commit.
