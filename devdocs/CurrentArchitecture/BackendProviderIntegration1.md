---
status: current
owner: runtime
last_verified: 2026-08-26
related_code:
  - include/gargantuan/runtime/EngineProviderConfiguration.hpp
  - include/gargantuan/entitlements/EntitlementProvider.hpp
  - include/gargantuan/services/EntitlementService.hpp
  - src/services/EntitlementService.cpp
  - tests/EntitlementServiceTests.cpp
related_adrs: []
---

# Backend Provider Integration Foundation 1

## 1. Engine semantic service ownership

The public Engine owns gameplay semantics. `EntitlementService`,
`IEntitlementProvider`, `PlayerIdentity`, `EntitlementId`, and
`EntitlementDecision` are canonical public types. A backend implements those
semantics; its endpoint, protocol, tenant model, and credentials are not Engine
semantics.

```text
PUBLIC ENGINE

Gameplay / Luau
       |
EntitlementService
       |
IEntitlementProvider
   /       |         \
None     Custom     [private adapter interface boundary]

                         |
                  PRIVATE INTEGRATION
                         |
                NodeEntitlementProvider
                         |
                  Node protobuf/gRPC
                         |
                 gargantuan-node
```

The dependency direction is private adapter to public Engine. Public Engine
does not compile, link, clone, package, or test against the private adapter.

## 2. Provider composition

`EngineProviderConfiguration` is an in-memory trusted bootstrap value. Its one
Foundation 1 slot accepts a `shared_ptr<IEntitlementProvider>` before runtime
gameplay starts. A native host can later call
`Engine::ReplaceEntitlementProvider`. Neither API is reflected to Luau.

This is deliberately a typed slot, not a generic `BackendService`, string
registry, raw RPC tunnel, or arbitrary DLL loader. A second semantic provider
should prove shared requirements before a generalized internal registry is
introduced.

## 3. Provider lifecycle and health

Providers implement `Start`, `Stop`, `Check`, `CheckMany`, and the closed health
state `Unavailable | Ready | Degraded`. Default lifecycle methods support
simple in-process custom providers. A candidate must start successfully and be
`Ready` before publication. A failed candidate is stopped while the working
provider and generation remain unchanged. Optional backend failure therefore
does not crash Engine startup.

## 4. Provider generation and hot swap

Hot swap means replacement by trusted native host authority only:

1. validate and start the candidate;
2. require candidate readiness;
3. atomically publish it as generation N+1 and clear semantic cache;
4. cancel generation N; and
5. stop generation N under the cooperative bounded provider contract.

New admission uses N+1 after publication. Work admitted under N captures its
provider and generation cancellation token. Its completion becomes
`Unavailable` if cancellation, deadline, shutdown, or generation mismatch is
observed. It cannot populate N+1 cache. Generation advances exactly once per
successful commit and never for candidate failure or same-provider transport
reconnect.

## 5. DataModel boundary

No provider implementation selection or deployment secret is authored or
serialized in DataModel. Endpoint, TLS roots, token references, transport
deadline, provider class, and private schema remain host/deployment inputs.
DataModel may contain portable gameplay intent such as entitlement identifiers
used by scripts. Existing runtime schema/custom-class support remains the place
for authored backend-neutral game configuration; no second generic
configuration database or `BackendPolicy` Instance was added.

## 6. Offline/no-provider behavior

Every DataModel has `EntitlementService` and an explicit
`NoneEntitlementProvider`. Checks return `Unavailable`; runtime and packages
boot without a private module, protocol descriptor, endpoint, credential, or
connection attempt. A trusted development host may choose
`LocalEntitlementProvider` with immutable grants.

## 7. Custom backend implementation

A game implements `IEntitlementProvider` in its host application and translates
its unrelated REST, custom protobuf, WebSocket, SQLite, platform SDK, or other
wire model into Engine decisions. Provider source need not live in the Engine
repository. Gameplay remains:

```luau
local Decision = game:GetService("EntitlementService"):CheckAsync(Player, "game.base")
```

The deterministic custom-provider vertical and public conformance helper prove
that the interface is not shaped around one private protocol.

## 8. Private Node adapter

The production adapter is owned by the private `gargantuan-node` repository at
`integration/gargantuan`. It depends on public Engine provider headers and the
private Entitlements V1 protobuf. Generated C++ protocol files exist only in
that private build tree. Translation validates request correlation, identity,
entitlement ordering, status, expiry, batch count, and response bytes.

Transport, authentication, authorization, malformed responses, restart,
deadline, cancellation, and network failures never become `Granted` or
`Denied`; they fail closed as provider errors and Engine `Unavailable`.

## 9. Deployment configuration and secrets

The private configuration accepts a TLS endpoint bounded to 512 bytes, a root
certificate path bounded to 1,024 bytes and certificate content bounded to 1
MiB, an environment-variable name for the workload token bounded to 128 bytes,
and 10 ms through 10 s connection/request deadlines. The token value is read
only when the trusted provider starts. It is not an API argument, DataModel
property, project field, process argument, log field, or telemetry dimension.

`Start` establishes the TLS channel and performs a private, backend-neutral
readiness entitlement probe. The probe verifies the workload credential and
`entitlements.check` capability before Engine can publish the candidate; its
fixed synthetic identity and entitlement never escape the private adapter.

Private workload credentials establish the Node tenant. Gameplay cannot supply
or override a tenant. Credentials belong only on the authoritative game server;
client packages receive neither the adapter nor server credential material.

## 10. Threading, deadlines, and shutdown

Native asynchronous checks are admitted to a fixed two-worker provider pool;
Luau yields and receives completion only when Engine pumps it on Main. At most
256 queued/active calls and 32 IDs per batch are accepted. The provider receives
caller and generation cancellation plus a five-second Engine deadline. Network
work never mutates DataModel directly.

Providers must cooperatively observe cancellation and make `Stop` bounded.
Engine cancellation reaches gRPC `TryCancel`. Runtime shutdown detaches Luau
references, cancels the active generation, stops the provider, and drains the
fixed worker pool.

## 11. Cache and observability

Engine owns a provider-neutral, in-memory semantic cache keyed by
`(PlayerIdentity, EntitlementId, ProviderGeneration)`. Granted and Denied
decisions live at most five seconds; expiring grants use the earlier semantic
expiry. `Unavailable` is never cached. The cache is bounded to 1,024 entries and
uses deterministic oldest-sequence eviction.

Bounded trusted metrics count semantic checks, provider calls, decisions,
timeouts, cache hit/miss, replacement attempt/commit/failure, latency, and
in-flight requests. Diagnostics may name the active low-cardinality provider
and generation, never its endpoint, token, TLS material, tenant, or private
state. External telemetry was not expanded.

## 12. Client/server security

Player identity comes from an immutable runtime-owned `Player`, not Luau
strings. The service validates DataModel ownership, identity and entitlement
bounds, batch bounds, decision identity/ID, expiry, and result count. Ordinary
Luau can check but cannot replace providers, set endpoints, install an
always-granted provider, supply credentials, or choose authority.

Studio receives no private dependency or production-secret UI. Development can
compose Offline, Local Test, or a host-configured integration provider outside
project state. MCP project/script writes gain no provider authority.

## 13. Packaging

Generic package tests boot the runtime in no-provider mode and recursively
reject private adapter names and `.proto` artifacts. The private test host opts
into the adapter explicitly. Public source guards reject private protocol
namespace/package and gRPC imports from the Engine semantic boundary.

## 14. Game-specific service strategy

Reusable concepts may later gain typed Engine semantic services such as player
identity, data storage, or matchmaking. Proprietary game systems that do not
fit a canonical semantic service belong behind a separately reviewed trusted
native/Luau service or game plugin boundary. They must not be pushed through
`EntitlementService` or a generic raw backend RPC API.

The current `ServiceProvider` registers a closed set of native services;
runtime custom schemas provide data, not native implementation registration.
A narrow game-service registration seam is deferred instead of adding a large
plugin framework in this foundation.

## 15. Future generalization

Generic candidates in the private Node implementation are the gRPC host,
principal authentication, capability policy, admission/deadline framework,
redacted diagnostics, secret-reference patterns, and provider-conformance
patterns. Private first-party material remains tenant/deployment policy, Steam
mapping and publisher configuration, account/license business logic,
production topology, and secrets. Generalization must preserve this dependency
split and does not require publishing Node.

Backend Integration Foundation 2 priorities, in exact order, are:

1. extract a reusable internal generation-safe typed `ProviderSlot<T>` only
   after a second semantic service proves the common lifecycle;
2. define a narrow trusted registration seam for game-specific native semantic
   services without a raw backend tunnel or project-controlled DLL loader;
3. add deployment-profile and secret-rotation hooks with redacted operational
   health, keeping all values outside DataModel;
4. add opt-in Studio local settings for Offline, Local Test, and configured
   integration providers without production credentials in projects;
5. expand cross-platform adapter chaos/load, malformed-response injection,
   sanitizer, and reconnect/shutdown coverage; and
6. evaluate a versioned provider module ABI separately, with signed/trusted
   host loading as a prerequisite rather than Foundation 1 behavior.
