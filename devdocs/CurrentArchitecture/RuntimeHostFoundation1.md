---
status: current
owner: runtime
last_verified: 2026-09-08
related_code:
  - src/host/
  - src/player/PlayerMain.cpp
  - src/server/ServerMain.cpp
  - include/gargantuan/runtime/
related_adrs: []
---

# Runtime Host Foundation 1

## Boundary

The official packaged runtime has distinct process composition roots:

```text
gargantuan_core
    <- gargantuan_runtime_host (internal packaged-bootstrap helpers)
        <- gargantuan_player_host <- GargantuanPlayer
        <- gargantuan_server_host <- GargantuanServer
```

Core owns Engine, GameSession, RuntimeMode, package semantics, and
ContentAvailability and includes neither host. The common target is internal
native implementation, not a stable public ABI. It parses transport endpoints
and performs package, native-closure, and schema bootstrap.

RuntimeMode remains explicit trusted EngineProviderConfiguration state. Engine
does not inspect executable identity or command-line state. Executable
separation reduces invalid composition; it is not the network authority
boundary.

## Pre-foundation responsibility audit

The old 376-line PlayerMain supported offline, client, and authoritative server
modes.

| Responsibility | Classification | Foundation 1 owner |
| --- | --- | --- |
| arguments and role selection | PlayerOnly / ServerOnly | separate parsers |
| package root, validation, native closure, schema bootstrap | CommonPackagedHost | BootstrapPackagedRuntime |
| snapshot/world loading | CommonPackagedHost | role runners through PackageBuilder |
| local content provider | ServerOnly / offline Player | role-specific Engine configuration |
| Node content/provider credentials | ServerOnly | trusted ServerHostConfiguration injection |
| audio | PlayerOnly | Player configuration |
| Engine construction | PlayerOnly / ServerOnly | explicit RuntimeMode in each runner |
| GameSession construction | PlayerOnly / ServerOnly | role runners; implementation stays in Engine/network |
| bind/listen | ServerOnly | GargantuanServer --bind |
| connect and hydration | PlayerOnly | GargantuanPlayer --connect |
| LocalPlayer bootstrap | AlreadyEngineOwned | Engine/GameSession |
| SDL, events, window, renderer, input, presentation | PlayerOnly | Player runner |
| graphical frame pacing | PlayerOnly | Player runner |
| authoritative tick pacing | ServerOnly | Server runner |
| startup/session smoke | TestOnly, role-specific | separate parsers and loops |
| diagnostics and bounded errors | CommonPackagedHost / role-specific | helper and runners |
| teardown | role-specific | separate lifecycle roots |

No Engine responsibility moved into hosts. Small role-loop duplication avoids a
universal object containing renderer, server, client, provider, and session.

## Player and Server

GargantuanPlayer is graphical/offline/network-client composition. Its entry
delegates to RunPackagedPlayer. It accepts offline startup and --connect and
owns SDL video/events, window, renderer, input, presentation, client hydration,
audio, pacing, and teardown. Test-only headless mode uses HeadlessRenderer.
--server-bind is rejected with a migration message. Unknown Node-provider and
server-policy options are bounded errors. Player constructs only Offline or
NetworkClient.

GargantuanServer is authoritative packaged-server composition. Its entry
delegates to RunDedicatedServer. It accepts --bind, loads the same package,
constructs Engine with NetworkServer, configures the same GameSession, and owns
server pacing, termination, and teardown.

ServerHostConfiguration is trusted native-only input containing an optional
IContentAvailabilityProvider and residency mode. With an injected provider the
host derives package namespace and manifest digest from the validated package.
With none, Server uses the package-local provider. The private Node adapter can
inject its authenticated TLS provider here; endpoint, certificate, and token
environment remain private deployment inputs.

Server rejects --connect and renderer/window/Player-headless flags. It always
uses HeadlessRenderer and never calls SDL_Init, polls SDL events, creates a
window or SDLRenderer, or enables audio. Monolithic gargantuan_core still links
SDL, renderer, and audio implementation, so Server's native closure currently
contains graphical libraries even though startup requires no display, window,
audio device, or GPU. Binary slimming is deferred.

## Retained Engine ownership

Both products use the same GameSession. Hosts do not define GSES, GRPL, GCHR,
Remote, Character, relevance, or scheduling semantics. Both use the same
ContentAvailabilityService. Hosts select provider/residency state but do not
acquire, hash, prepare, admit, evict, cache, or resolve dependencies.

EngineProviderConfiguration is unchanged. Entitlements and Content are trusted
host provider seams, AudioEnabled is Player presentation state, and Mode is
explicit Engine semantic state. None is serialized into packages, DataModel,
snapshots, journals, replication, Luau, or handshakes.

## Package, lifecycle, and compatibility

Game package v2, content-manifest v1, instance-schema v4, GRPL, GCHR, and Luau
surfaces are unchanged. gargantuan_runtime_distribution stages Player;
gargantuan_server_runtime_distribution separately stages Server through the
existing distribution schema, without adding Server to normal client packages.
The schema still calls its executable field Player; retaining that internal
field avoids a format revision.

The packaged fixture builds identical temporary game content into both
profiles and launches GargantuanServer --bind with GargantuanPlayer --connect.
Offline Player remains single-process and needs neither Server nor Node.

Player stops GameSession before Engine, then renderer and SDL. Server stops
GameSession before Engine; Engine stops ContentAvailability and joins/cancels
provider work. SIGINT/SIGTERM request normal Server-loop exit. Existing session
and content generations remain stale-work authority.

Package, client, and Luau state cannot construct ServerHostConfiguration,
choose a Node token/endpoint, select NetworkServer, or invoke trusted retry.
Node credentials remain environment-referenced private state and are not
logged, serialized, or replicated. Executable identity grants no authority.
EditorHost remains a separate authenticated document boundary.

The helper runs only at startup; no recurring host scheduler or per-tick
polymorphic dispatch was added. Future factoring may split graphical platform,
render, and audio implementation from core. Repository extraction should wait
for genuinely divergent product ownership/cadence, platform integration,
deployment orchestration, or a stable supported host ABI.

Foundation 3M remains Engine policy composed by Server:

```text
GargantuanServer -> trusted ServerHostConfiguration
    -> Engine(NetworkServer) -> residency policy -> ContentAvailability
```

The pre-existing sustained RemoteFunction crash and 3L.1 peer-scale/RSS/soak
gaps are neither resolved nor hidden by this boundary.
