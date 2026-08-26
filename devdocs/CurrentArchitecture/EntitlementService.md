---
status: current
owner: runtime
last_verified: 2026-08-26
related_code:
  - assets/services/EntitlementService.luau
  - include/gargantuan/entitlements/
  - include/gargantuan/runtime/EngineProviderConfiguration.hpp
  - include/gargantuan/services/EntitlementService.hpp
  - src/services/EntitlementService.cpp
  - tests/EntitlementServiceTests.cpp
related_adrs: []
---

# EntitlementService

`EntitlementService` is the canonical headless, DataModel-scoped gameplay
service for durable rights. It owns backend-neutral `Granted`, `Denied`, and
`Unavailable` semantics; it is not authentication, commerce, inventory, or a
generic RPC surface.

```luau
local Entitlements = game:GetService("EntitlementService")
local Decision = Entitlements:CheckAsync(Player, "game.base")
local Many = Entitlements:CheckManyAsync(Player, { "game.base", "dlc.pack_1" })
```

Both calls require a yieldable Luau coroutine. Native provider work runs on a
fixed two-worker pool, and Engine resumes completion on Main during its Scripts
phase. `CheckManyAsync` accepts 1 through 32 identifiers and preserves order.
The same call works with None, Local, a game-defined provider, or an optional
private adapter.

An ID contains 1 through 128 ASCII bytes in two or more dot-separated segments.
Every segment starts with `a-z`; remaining bytes may be `a-z`, `0-9`, `_`, or
`-`. The runtime-owned `Player` supplies an immutable provider-qualified
identity. Luau cannot supply identity, replace authority, or access deployment
configuration.

`Denied` means an authority answered that the identity lacks the entitlement.
`Unavailable` means no authority/identity, admission failure, cancellation,
deadline, replacement, shutdown, provider/transport failure, or malformed
provider output. Infrastructure failure never becomes a denial or grant.

## Provider contract

`IEntitlementProvider` consumes only Engine request context, identity, and IDs.
It exposes `Start`, `Stop`, `GetHealth`, `Check`, and `CheckMany`. Its closed
health state is `Unavailable | Ready | Degraded`; provider-specific diagnostics
and transport types do not cross the interface.

Trusted configuration uses `EngineProviderConfiguration` at construction or
`Engine::ReplaceEntitlementProvider` at runtime. These APIs are native-only and
absent from reflection. Candidate replacement is start/ready-before-commit.
Successful commit publishes generation N+1, clears cache, cancels generation N,
then stops N. Old completion is deterministically `Unavailable` and cannot
affect N+1. Failed candidates leave the old provider and generation untouched.

None is explicit and requires no network. Local holds immutable trusted
development grants. A custom game provider may wrap an unrelated REST,
protobuf, platform, database, or other schema without changing gameplay.

## Bounds, cache, and metrics

The service accepts at most 256 queued/active provider calls, has a five-second
deadline, and caches at most 1,024 semantic decisions for five seconds. Cache
key is identity, entitlement, and provider generation. Grant expiry shortens
cache lifetime; `Unavailable` is never cached. Replacement clears all entries.

Metrics are saturating and bounded: semantic checks, provider calls,
Granted/Denied/Unavailable, timeout, cache hit/miss, replacement
attempt/commit/failure, provider latency, and current in-flight count. They do
not contain endpoint, credential, tenant, or private response data.

Provider selection, endpoint, TLS roots, token references, and protocol config
are host/deployment state, never DataModel/project serialization. Generic
packages contain no optional adapter artifact and boot with None semantics.

See [Backend Provider Integration Foundation 1](BackendProviderIntegration1.md)
for composition, private-boundary, packaging, security, and future-extension
decisions.
