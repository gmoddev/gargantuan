# Concrete-class property default qualification

Status: implementation complete; native qualification pending.

Source base: accepted architecture `a10779015186b5d31416bdb44d72920fb6ec7bbc`,
on Engine property-batch baseline `04e1353edfe24ffc9d23968f8791afaf245f8ee8`.
Implementation branch: `feature/concrete-property-defaults`.

The [current contract](../CurrentArchitecture/ConcreteClassPropertyDefaults.md)
records syntax, lifecycle, limits, discovery compatibility, and exclusions.

## Evidence matrix

| Gate | Evidence / current state |
| --- | --- |
| Classgen | Generation succeeds; 22 focused literal/identity/duplicate/malformed/bound checks pass locally; repeated by both Native CI jobs. |
| Native compilation | Local MSVC syntax checks pass for runtime schema, generated TextBox, EditorHost, and the focused fixture; full Release build pending hosted CI. |
| Construction | `gargantuan_concrete_property_defaults` compares direct construction, registry construction, effective resolution, and actual production GetSchema across all prepared-safe saved properties on the five migrated classes, Frame, Part, UIListLayout, and an inherited-label fixture. Pending execution. |
| Inheritance | Fixture protects no-override fallback, inherited override, nearer TextBox/TextButton override, exact declaring pointer identity, and stale concrete/declaring versions. |
| Invalid metadata | Candidate validation rejects unknown/own property, duplicate, wrong native type, nonfinite value, wrong identity/version, transient read-only property, invalid enum, range/native-validator failures, and class count overflow. |
| Persistence/clone | Fifteen class/value-mode cases cover effective defaults, declared fallbacks, and edited values; verify saved fields are present and load/Clone preserve every saved property. Pending execution. |
| Prepared compatibility | Existing prepared/property-batch tests retained; new fixture checks migrated metadata eligibility. No reset-through-batch qualification is performed. |
| Legacy discovery | Legacy Classes keeps its declared fallback. Strict older schema-version readers reject v7 safely and need a later consumer update. |
| Resource accounting | 16 overrides; ABI record size and exact JSON delta printed by native fixture. Measurement pending execution. |
| MSVC / sanitizers | Required hosted Native CI pending. Remote `dockerbox` SSH unavailable; no sustained local build substituted. |

No Studio, Foundation 3L, prepared coordinator, mutation command, or persistence
format changes are part of this slice. Runtime reset still uses the declared
fallback and is intentionally not qualified. KI-009 remains open for that
remaining production reset integration and its subsequent qualification.

After foundation qualification, the next Engine task is to update ordinary
`ResetPropertyToDefault` to resolve the effective concrete default and resume
effective-default reset-through-SetPropertyBatch qualification. Studio schema-v7
consumption and reset controls follow only after that Engine gate succeeds.
