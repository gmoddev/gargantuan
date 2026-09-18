# Concrete-class property default qualification

Status: concrete-class default foundation qualified on 2026-09-18 (UTC).
Production reset-to-default was outside this foundation qualification; its
subsequent [implementation and qualification](AuthoritativePropertyResetToDefault.md)
are recorded separately.

Source base: accepted architecture `a10779015186b5d31416bdb44d72920fb6ec7bbc`,
on Engine property-batch baseline `04e1353edfe24ffc9d23968f8791afaf245f8ee8`.
Implementation branch: `feature/concrete-property-defaults`.
Implementation source: `94f245c629e75aa3822c052a45af797f2401390c`.

The first hosted Windows execution (`35294604118`, source `f7115cb3`) passed
59 of 60 tests. The new equivalence fixture exposed existing string-literal
fallbacks stored as `const char*` instead of `std::string`. Classgen now emits
the declared native string types for those fallbacks. The corrected Windows
matrix passed all 60 tests in [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/35298022606).
The latest [GNS sanitizer workflow](https://github.com/gmoddev/gargantuan/actions/runs/35298022586)
also passed. The dependent Linux ASan/UBSan/LSan gate passed all 52 tests in the
same Native CI run. Both jobs generated their sources from a fresh checkout.

The [current contract](../CurrentArchitecture/ConcreteClassPropertyDefaults.md)
records syntax, lifecycle, limits, discovery compatibility, and exclusions.

## Evidence matrix

| Gate | Evidence / current state |
| --- | --- |
| Classgen | Generation succeeds; 26 focused literal/identity/duplicate/malformed/bound checks pass locally, including both finite Float extrema; repeated by both Native CI jobs. A repeated generation skipped all 62 outputs. Base-chain dependency tracking avoids regenerating unrelated classes. |
| Native compilation | Full hosted MSVC Release and Clang 19 sanitizer builds pass, in addition to local syntax probes. |
| Construction | `gargantuan_concrete_property_defaults` passes 179 property cases comparing direct construction, registry construction, effective resolution, and actual production GetSchema across all prepared-safe saved properties on the five migrated classes, Frame, Part, UIListLayout, and an inherited-label fixture. All 16 overrides are covered. |
| Inheritance | Fixture protects no-override fallback, inherited override, nearer TextBox/TextButton override, exact declaring pointer identity, and stale concrete/declaring versions. |
| Invalid metadata | Candidate validation rejects unknown/own property, duplicate, wrong native type, nonfinite value, wrong identity/version, transient read-only property, invalid enum, range/native-validator failures, and class count overflow. |
| Persistence/clone | All 15 class/value-mode cases pass for effective defaults, declared fallbacks, and edited values; saved fields remain present and load/Clone preserve every saved property. |
| Prepared compatibility | Existing prepared/property-batch regressions pass; the new fixture checks migrated metadata eligibility. No reset-through-batch qualification is performed. |
| Legacy discovery | Legacy Classes keeps its declared fallback. Strict older schema-version readers reject v7 safely and need a later consumer update. |
| Resource accounting | 16 overrides across 62 production native classes. MSVC: 120-byte record and 24-byte vector; `16 * 120 + 62 * 24 = 3,408` fixed bytes per schema copy, or 10,224 for the usual three copies, plus name allocation/allocator bookkeeping. Native values total 62 bytes (inline on MSVC), property names 227 bytes, and encoded values 672 bytes. Override discovery adds exactly 3,695 bytes. The declared-string correction adds 2,493 bytes across existing projections, for a total production discovery increase of 6,188 bytes. |
| MSVC / sanitizers | MSVC Release 60/60, Linux ASan/UBSan/LSan 52/52, and the naturally triggered GNS sanitizer workflow pass on the implementation source. Remote `dockerbox` SSH unavailable; no sustained local build substituted. |
| Documentation | Astro build passed all 19 pages locally; relative documentation links and whitespace checks pass. |

The fixture's `ClassVectorBytes=1512` includes one extra test-only inherited
class; production has 62 vectors, or 1,488 bytes. The override delta is unaffected
by that fixture class. The separate string delta is derived for the 62 production
classes from the 24 newly encodable literal defaults and their inheritance
fan-out. Two of those declarations use native string views; the other 22 own
strings inside existing `Unmodified` fields. Their allocation depends on the
standard-library small-object/string optimization.
The fixed-byte figures count live members and override records, excluding
container capacity slack, allocator overhead, and common variant storage for
additional project-defined enums/extensions. They are not process-RSS figures.
Linux/libstdc++ reports a 72-byte override record and the same 24-byte vector:
`16 * 72 + 62 * 24 = 2,640` fixed bytes per native schema copy, or 7,920 for the
usual three copies, plus name/value allocation. Its fixture reports the same
179 property cases, 62 native value bytes, 227 name bytes, 672 encoded value
bytes, and 3,695 override discovery bytes. Both platforms pass all 15
persistence/Clone scenarios inside that fixture, including explicit saved
fields for values equal to either kind of default.

No Studio, Foundation 3L, prepared coordinator, mutation command, or persistence
format changes are part of this foundation slice. At this recorded revision,
runtime reset still used the declared fallback and KI-009 remained open for
the subsequent production reset integration and qualification.

The foundation gate is satisfied. The subsequent Engine task updated ordinary
`ResetPropertyToDefault` and qualified effective-default reset through
SetPropertyBatch, as recorded in the reset receipt above. It also added explicit
v7 discovery selection while preserving parameterless v6 for older readers.
Studio schema-v7 consumption and reset controls are now the next separate task.
