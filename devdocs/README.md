# Gargantuan engineering audit

This directory is an implementation-grounded snapshot of Gargantuan at commit
`e4fca3575cc84c0d5fa4a946b88bf528aac2223b` (2026-08-12 checkout). It is not a
promise that every named class, roadmap checkbox, or public API works. Current
claims are based on executable paths in `src/`, native interfaces in `include/`,
Luau class/service declarations in `assets/`, build files, tests, and the Studio
project scaffold.

## Read this first

- [Current architecture](CurrentArchitecture/README.md) explains the engine that
  exists today.
- [Subsystem audit](CurrentArchitecture/SubsystemAudit.md) records what works,
  what is partial, what is a stub, and what blocks real games.
- [Implementation gaps](CurrentArchitecture/ImplementationGaps.md) is the
  source-verified readiness inventory and documentation mismatch audit.
- [Security audit](CurrentArchitecture/SecurityAudit.md) separates confirmed
  findings, plausible risks needing runtime proof, and general hardening.
- [Future architecture](FutureArchitecture/README.md) defines the target product
  and its boundaries.
- [Networking and security](FutureArchitecture/NetworkingAndSecurity.md) designs
  the authoritative online model.
- [GUI and Studio](FutureArchitecture/GuiAndStudio.md) defines runtime UI and the
  editor gate.
- [Minimum usable game](FutureArchitecture/MinimumUsableGame.md) defines the first
  milestone where a developer should not need routine C++ changes.
- [Roadmap](FutureArchitecture/Roadmap.md) orders the work and gives exit tests.

## Evidence labels

| Label | Meaning |
|---|---|
| **Implemented** | A present execution path performs the core behavior. This does not imply production quality. |
| **Partial** | A working slice exists, but ordinary use hits missing behavior or unsafe assumptions. |
| **Scaffold** | Types, declarations, or a pass exist without an effective feature path. |
| **Planned, absent** | Documentation or naming indicates intent, but no implementation was found. |
| **Dead/disabled** | Code exists but is commented out, not registered, or unreachable. |
| **Inference** | Likely author intent inferred from multiple source signals; not current behavior. |
| **Unknown** | The repository does not supply enough evidence to decide. |

## Executive verdict

Gargantuan can load a JSON-backed local project or a Luau file, construct an
Instance tree, run basic scripts, move a free camera, simulate primitive Box3D
bodies, emit contact signals, and draw primitive geometry with a directional
shadow pass. Those are useful engine experiments.

It cannot yet support a basic game-development workflow. There is no networking,
player/session model, audio, animation, production asset pipeline, functional
runtime GUI, usable Studio, safe capability model, stable module loader, robust
save/edit loop, or engine CI. Multiple native-boundary and lifecycle issues mean
untrusted projects should not be opened on a valuable host.

The recommended target is not literal Roblox API parity. Preserve the productive
model—Instances, services, Luau, replicated scene objects, event/RPC primitives,
hierarchical UI, scene editing—while replacing implicit authority, ambiguous
ownership, global mutable state, and historical naming with explicit worlds,
execution domains, capabilities, schemas, stable IDs, and observable budgets.

## Validation boundaries

The audit used direct source inspection plus a repeated static security discovery,
validation, and attack-path pass. The checkout available for this review did not
contain initialized vendor submodule worktrees, so a full native compile, GPU run,
and sanitizer/fuzz execution were not performed. Build/runtime-dependent risks are
therefore labeled as deferred where proof matters. Documentation links and
Markdown structure were checked locally; important current-state claims were
cross-checked against their reachable source paths.
