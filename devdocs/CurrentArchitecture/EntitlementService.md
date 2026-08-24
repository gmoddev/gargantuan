---
status: current
owner: runtime
last_verified: 2026-08-24
related_code:
  - assets/services/EntitlementService.luau
  - include/gargantuan/entitlements/
  - include/gargantuan/identity/PlayerIdentity.hpp
  - include/gargantuan/services/EntitlementService.hpp
  - src/entitlements/
  - src/services/EntitlementService.cpp
  - tests/EntitlementServiceTests.cpp
related_adrs: []
---

# EntitlementService Foundation 1

## Ownership and public semantics

`EntitlementService` is the one canonical, headless, DataModel-scoped game
service for durable rights such as a base-game license, DLC, feature access, or
a subscription tier. It is not authentication, commerce, inventory, or a
generic backend RPC surface.

Normal Luau supplies a runtime-owned `Player`, never provider/subject strings:

```luau
local Entitlements = game:GetService("EntitlementService")
local Decision = Entitlements:CheckAsync(Player, "game.base")
```

`CheckManyAsync(Player, EntitlementIds)` accepts 1 through 32 identifiers and
returns one ordered decision per identifier. Foundation 1 providers complete
through the native bounded call contract; the `Async` public name reserves the
external-authority behavior expected of later adapters without exposing a
transport or callback protocol to gameplay.

An entitlement identifier contains 1 through 128 ASCII bytes in two or more
dot-separated segments. Every segment starts with `a-z`; the remaining bytes
may be `a-z`, `0-9`, `_`, or `-`. `game.base`, `dlc.expansion_1`, and
`feature.private_servers` are valid. Platform AppIDs, database row IDs, and Node
RPC names are not entitlement identifiers.

The closed decision is:

```text
Status: Granted | Denied | Unavailable
EntitlementId: canonical semantic ID
Identity: immutable provider-qualified Player identity
ExpiresUnixMilliseconds?: neutral optional expiry
```

`Denied` is an authoritative lack of a grant. `Unavailable` means no authority
is configured, an identity is not established, cancellation/deadline occurred,
the provider failed, or its result violated the semantic contract. Provider
exceptions and private diagnostics are never returned to Luau. A provider does
not invent expiry for a permanent grant.

## Player identity and authority

`PlayerIdentity { Provider, Subject }` is distinct from runtime `PlayerId`, a
transport connection, and an authentication flow. Provider names are canonical
bounded lowercase ASCII; subjects are bounded valid UTF-8. A trusted native
bootstrap initializes the identity once. Luau cannot read or mutate an identity
property, submit a replacement identity, configure credentials, grant rights,
or replace the provider. The current local runtime initializes
`(local, player-1)`; a future authenticated server bootstrap can initialize the
same engine-owned type without changing entitlement gameplay calls.

## Provider boundary and lifecycle

`IEntitlementProvider` is engine-owned. It consumes only:

- `EntitlementRequestContext` with cancellation and a steady-clock deadline;
- the canonical `PlayerIdentity`; and
- one or a bounded span of `EntitlementId` values.

It returns engine-owned `EntitlementDecision` values or a closed provider error
category. No protobuf, gRPC, HTTP, Steamworks, Node capability, credential,
database, or transport type crosses this boundary. The default batch
implementation checks each item in order and treats a provider error as a
whole-batch failure; the service then returns `Unavailable` for every item.

`ConfigureProvider` is native-only trusted application/bootstrap
configuration and is absent from reflection/Luau. Replacement increments a
monotonic provider generation. The generation is the required invalidation
component for any future cache key. Foundation 1 deliberately has no decision
cache, persistent cache, or background work.

The included providers are:

- `NoneEntitlementProvider`: always `Unavailable`, requiring no network;
- `LocalEntitlementProvider`: immutable trusted development grants; absent or
  expired grants are `Denied`.

Local grants are runtime/bootstrap inputs and are not project serialization.
Provider mappings and secrets must remain deployment state.

## Custom backends and Node non-dependence

A game application may implement `MyBackendEntitlementProvider :
IEntitlementProvider` around REST, custom protobuf, WebSocket, SQLite, a platform
SDK, or another private protocol. That adapter owns its credentials, mapping,
timeouts, response bounds, and error translation. `EntitlementService`, Player
identity, and gameplay Luau do not change.

Private Gargantuan Node integrates later through an optional
`NodeEntitlementProvider : IEntitlementProvider` in a private integration module:

```text
PUBLIC ENGINE

    EntitlementService
           |
    IEntitlementProvider
       /      |       \
    None    Custom    Private Node adapter

PRIVATE NODE

    Entitlements V1
          |
    EntitlementProvider
       /          \
    Steam       First-party Custom
```

No core engine header, schema, generated source, service, or test depends on
gargantuan-node or its protobuf. The neutral JSON vectors under
`tests/conformance/entitlements-v1.json` describe shared behavior without a
binary or schema dependency.

## Failure, bounds, and deferred work

Identity provider, identity subject, entitlement ID, batch count, deadline, and
provider output are bounded. Providers must propagate cancellation and must not
continue indefinite background work. The engine deadline is five seconds in
Foundation 1. The service is renderer-, Studio-, and network-independent and is
therefore available on a dedicated/headless DataModel.

Foundation 1 does not implement a Node adapter, authentication transport,
commerce, inventory, purchase flows, billing, persisted grants, provider
credentials, entitlement management UI, or a cache. The first adapter milestone
is to implement the private protobuf client entirely behind
`IEntitlementProvider`, translate all Node statuses/errors into the engine
contract, pass the neutral vectors, and inject it from trusted server bootstrap.
