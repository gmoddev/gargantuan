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

## Trusted reliable-service configuration (2026-09-12)

`ServerHostConfiguration::ReliableService` is startup-only native deployment
input, separate from content/provider configuration. The dedicated Server accepts
the complete `--reliable-rate` / `--reliable-aggregate-rate` / `--reliable-peers`
tuple or an injected host profile, not both. Validation occurs before packaged
runtime acquisition. No client, package, Node content descriptor or Luau field
can supply it. The host passes connection/aggregate reservations to GameSession
and a separate backend rate to its GNS listener. See the
[deployment contract](NetworkingReliableDeploymentContract.md) for numeric limits,
packet/realtime headroom, low-rate unqualified behavior and qualification gates.
Omission retains legacy development behavior and reports it as unqualified.

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
| Node content/provider credentials | ServerOnly | trusted ServerHostConfiguration and GargantuanServer CLI |
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

ServerHostConfiguration is trusted native-only input containing a tagged
ServerContentConfiguration. LocalServerContentConfiguration is the default.
NodeServerContentConfiguration contains only endpoint, root-certificate path,
workload-token environment-variable name, residency mode, and bootstrap
deadline. InjectedServerContentConfiguration remains an internal focused-test
seam. Every provider receives package namespace and trusted manifest digest
from the already validated package; there is no independently configurable
Node package identity.

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
accepting/polling peers, stops and destroys GameSession, calls Engine::Destroy,
and only then releases Engine. Both hosts declare the renderer owner before
Engine and GameSession, outside their runtime `try` block, so the borrowed
renderer remains alive during catch-handler cleanup as well as normal return.
The 3L.2 exception audit found and corrected Server's former try-local renderer;
normal shutdown had concealed a stack-use-after-scope on startup/runtime error.
Engine stops ContentAvailability; that service
invalidates demand/session generation, asks the provider to cancel active RPC
contexts, joins its bounded worker queue, destroys Resident roots, and releases
the provider/channel with Engine. SIGINT/SIGTERM request normal Server-loop
exit. Existing session and content generations remain stale-work authority.

The trusted `--session-smoke` diagnostic additionally records at most 4,096
Server/Player tick samples and 256 structural plus 256 application send samples.
An internal Server transport decorator forwards every operation unchanged and
observes the existing backend queued-reliable-byte statistic. It creates no
queue or scheduling policy. Traces contain timing, counts and byte sizes, never
payloads, credentials or certificate material. Ordinary host mode constructs the
transport directly. Fixture RPCs carry a monotonic timestamp as an ordinary
return value to distinguish handler service from delayed reply delivery.
Player samples additionally report successfully handled GCHR/Remote message
counts and exact monotonic maximum intervals between those handler calls.
These aggregate service counters have fixed per-session storage and no payload
labels. A service gap includes a legitimate sender idle interval, so it is a
starvation measure only when the fixture continuously offers corresponding
traffic; it is not a claim that every handled GCHR frame changes a transform.

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

The pre-existing sustained RemoteFunction crash was reproduced during the 1.1
official-host work, root-caused to ScriptEngine closing its Luau VM before a
world-held RemoteFunction released its registry references, and repaired with
an explicit handler-clear phase before VM teardown. The independent 3L.1
peer-scale/RSS/soak gaps are not hidden by this boundary.

## Foundation 1.1 official Node composition

Foundation 1 established the role split but left the production-shaped C++
ContentStreaming adapter private to `gargantuan-node/integration/gargantuan`.
Official GargantuanServer could accept an injected provider, but its executable
did not parse Node deployment configuration, construct the adapter, stage its
native runtime closure, or prove Node -> official Server -> official Player.

Foundation 1.1 promotes the minimum adapter implementation to
`src/host/server/NodeContentProvider.*` and builds it as the internal
`gargantuan_node_content_provider` target only when
`GARGANTUAN_WITH_NODE_CONTENT=ON`. The target implements the existing
`IContentAvailabilityProvider`, generates C++ bindings from Node's canonical
`gargantuan.node.content.v1.ContentStreaming` proto, and links only into
`gargantuan_server_host`. Core does not depend on either host, and
`gargantuan_player_host` neither links nor constructs the adapter.

The adapter owns one Server-lifetime TLS channel/stub, a bounded copied
workload token, monotonically generated bounded request IDs, and the active
unary ClientContexts. It makes synchronous bounded GetManifest/GetContent calls
on ContentAvailability's worker threads. A small per-call cancellation watcher
bridges Engine cancellation/generation/deadline state to `TryCancel`; the same
worker receives the response and enqueues immutable completion state through
ContentAvailability. Only Main calls ContentAvailability::Step, performs final
namespace/key/size/digest/schema checks, prepares/adopts the detached document,
and performs authoritative SetParent. No provider callback owns ServerHost or
mutates DataModel state.

Official Node mode is selected only by trusted Server arguments:

```text
GargantuanServer
    --bind 127.0.0.1:46000
    --content-provider node
    --content-residency on-demand
    --content-node-endpoint node.example:7443
    --content-node-root-ca deployment-root.pem
    --content-node-token-env GARGANTUAN_NODE_TOKEN
```

`--content-provider local` and `--content-residency fully-resident` remain the
defaults. Node selection requires all three Node arguments, a syntactically
valid endpoint, a bounded portable environment-variable name, a present
nonempty bounded token value, a present nonempty root certificate no larger
than 1 MiB, and valid provider/content limits. There is no literal token
argument and no plaintext Node mode. Missing configuration, failed TLS/auth,
missing capabilities, unavailable bootstrap, and package/manifest mismatch are
nonzero fail-closed startup errors before GNS opens; explicit Node mode never
falls back to Local.

The token value is read once by the adapter after package validation and is
used only to construct per-call authorization metadata. It is never placed in
ServerHostConfiguration, DataModel, package/snapshot/journal, diagnostics,
replication, Player, or Luau. Ordinary errors contain stable failure classes,
not raw gRPC details, metadata, certificate contents, or token values. The
environment-variable name may appear in a bounded missing-value diagnostic.
The token buffer is overwritten before provider destruction; gRPC necessarily
retains request-local metadata while a call is active, and shutdown cancels and
joins those calls before provider release.

Node requests always use the ProjectId and PackageVersion derived from the
validated packaged runtime and the manifest-selected PackageContentKey. The
adapter checks correlation ID plus exact project/version/key and response size.
ContentAvailability independently checks the manifest digest and namespace,
then content identity, byte count, advertised digest, locally computed digest,
schema, dependency, and authoritative admission rules. Node supplies immutable
bytes only; it cannot choose RuntimeMode, allocate ObjectIds, construct
Instances, select LocalPlayer/Character authority, affect 3E/3J directly, or
inject GRPL.

The Server runtime distribution stages the gRPC/protobuf/OpenSSL/zlib runtime
closure and notices required by Node mode. Relocation tests scrub PATH to the
distribution plus Windows system directories, so the official Server cannot
accidentally resolve development-tree DLLs. Local mode in that same relocated
distribution starts without a Node process, token, or certificate. The Player
distribution remains separately packaged and has no Server configuration or
credential path. Current monolithic core still carries SDL/render/audio code in
the Server binary closure, but headless Server construction initializes none of
those subsystems and needs no display, window, audio device, or GPU.

The production acceptance fixture starts the actual `gargantuan-node`
executable with real TLS, relocated GargantuanServer in explicit Node OnDemand
mode, and relocated GargantuanPlayer over normal GNS. It proves manifest
bootstrap before peer acceptance; LocalPlayer, Character, movement/control;
RemoteEvent traffic before/after load, eviction, and reload; five paced
RemoteFunction calls; one Node-originated streamable unit through 3K/3H/3E/3J
and GRPL; exact client hierarchy/properties; authoritative/client eviction; and
fresh server/client lifetimes on reload. Equivalent Local/Node and
FullyResident/OnDemand runs produce the same canonical semantic digest.

The lifecycle matrix additionally runs ten fresh Node/Server/Player process
cycles, same-port restart, Node shutdown after Resident admission, and Server
shutdown with a delayed GetContent in flight. In the latter case Stop first
invalidates the content generation, cancels the ClientContext, joins provider
work, admits no object, and reports no stale completion; a replacement Server
creates a fresh provider/channel and loads normally. This closes the original
Runtime Host Foundation boundary. The root-caused RemoteFunction teardown fix
has a focused regression and the exact official vertical now passes both five
paced calls and a separate 100-call run with zero timeouts. This does not by
itself promote Foundation 3L.1: content-coupled 32/100/500-peer evidence, owner
action/root-motion streaming health, long-run RSS plateau, and the remaining
100-cycle soak are independent outstanding gates before Foundation 3M.

Linux uses Clang 19 with ASan, UBSan, and LSan leak detection for the complete
supported Engine suite, the production Node content adapter, and a relocated
official headless GargantuanServer TLS/bootstrap smoke. The Linux adapter test
omits its optional GNS client/server sub-scenario because the pinned external
GNS implementation uses an ABI-compatible type-erased packet callback that
Clang's function-type sanitizer reports; Windows retains the complete official
GNS Node -> Server -> Player vertical. No sanitizer suppression was added. The
Node-enabled CMake path prefers package CONFIG files and falls back to CMake's
standard Protobuf module so both vcpkg Windows and supported Linux packages
resolve the same generated-binding targets.
