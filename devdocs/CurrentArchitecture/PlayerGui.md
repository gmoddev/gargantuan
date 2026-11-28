---
status: current
owner: runtime-gui
source_of_truth:
  - assets/services/StarterGui.luau
  - assets/classes/PlayerGui.luau
  - src/classes/Player.cpp
  - src/services/Players.cpp
  - src/gui/GuiRuntime.cpp
  - src/network/GameSession.cpp
  - src/network/ReplicationRelevance.cpp
  - src/network/ReplicationCoordinator.cpp
  - src/packaging/PackageBuilder.cpp
---

# StarterGui and PlayerGui

## Authoring and provisioning

`StarterGui` is a canonical DataModel service established before an authored
project's initial revision. Author a `ScreenGui` as a direct child of
`StarterGui` and put its controls, layouts, and permitted script and viewport
content under that root. JSON and GAS preserve these templates alongside other
project services. A non-archivable root is skipped. An eligible root requires
archivable descendants and must satisfy the existing Instance clone rules.
Provisioning allows eight roots and 1,024 aggregate template Instances. A clone
staging failure records a bounded diagnostic. Initial provisioning still
supplies the owner with an empty container.

The local Play host and session server build a non-serialized `PlayerGui`
container before publishing the `Player` and firing `PlayerAdded`. Its typed
read-only accessor is `Player.PlayerGui`. Each owner has one container with
fresh ObjectIds and independent state. Template cloning uses the Instance clone
path, which remaps internal typed references, including a ViewportFrame's
camera reference, and retains stable asset strings. References to another
StarterGui template or the StarterGui container fail provisioning. Each
ScreenGui is cloned independently. Use runtime lookup when one layout needs
content from another, and follow the target property's reference rules.
A server-created player can use the GUI before selecting or spawning any
controlled entity.

Changes to a template affect future provisioning. Setting
`ScreenGui.ResetOnSpawn` opts a direct root into retirement on a later live
Character binding. The first Character binding leaves the initially provisioned
GUI in place. The host retires opted-in server roots and clones the current
opted-in templates at each later binding. Independently created opted-in roots
are retired, and persistent roots keep their identities and local state.
Clearing a Character without binding a new one leaves the GUI in place. If a
synchronous removal or addition callback binds another Character, the host
abandons stale template publication and coalesces the pending resets to the
latest surviving Character. Its `CharacterAdded` is delivered after the bounded
reset pass.

## Presentation and ownership

The runtime lays out and routes input to the local Player's direct PlayerGui
roots. It also preserves existing legacy screen roots outside StarterGui and
PlayerGui. Native authoring preview uses the same bounded layout and rendering
for direct StarterGui roots. It publishes its own layout and presentation
snapshots, leaves authored geometry and project revision untouched, and never
registers for gameplay input. Project image and material texture changes flow
through the editor's normal asset publisher. Preview text owns its font atlas.

The session relevance index associates each PlayerGui subtree with the trusted
Player identity. The replication coordinator independently checks that identity
along the ancestry of every publication and materialization dependency. The
canonical StarterGui tree stays server-side. A ViewportFrame branch outside an
owned PlayerGui is host-local. Ordinary legacy ScreenGui content outside that
branch keeps its existing network behavior.

A connected client learns other relevant Player identities while the
`PlayerGui` accessor only resolves for the owner. Gameplay readiness requires the
owner's container, and GRPL can deliver descendants over successive structural
frames. Use existing `ChildAdded`, `ChildRemoved`, and property signals for
content that may arrive or change after initialization. A bounded wait using
Instance lookup and the task scheduler can also handle progressive arrival:

```luau
local function WaitForChild(Parent, Name)
	for Attempt = 1, 60 do
		if Parent.Destroyed then
			return nil
		end
		local Child = Parent:FindFirstChild(Name, false)
		if Child then
			return Child
		end
		task.wait()
	end
	return nil
end
```

The server owns the lifetime and parent of replicated GUI nodes. The owner
can edit their permitted presentation properties and freely add, move, or
remove locally created descendants through the ordinary Instance and GUI APIs.
Client changes stay local and server-authored state arrives
through existing structural replication. Server-owned reset roots follow that
replication lifecycle, and an accepted replacement Character also retires
opted-in client-created roots. Disconnect retires the PlayerGui subtree, and a
subsequent join receives a fresh container from the current templates.

## Client script code

StarterGui stores code as authored templates. PlayerGui executable Scripts use
`RunContext.Client` and run only in the owning local player's runtime. A
PlayerGui ModuleScript can be resolved by the owning client through the ordinary
module loader. Runtime script and module clones retain their independent
`SourceId`. The host assigns an immutable, engine-owned template source identity
to eligible clones. The packaged client uses that identity to find trusted
local package source and checks the exact class, runtime role, owning PlayerGui
scope, source digest, and projected SourceVersion before attaching code. GRPL
carries bounded provenance and structural properties. The source bytes live
in the trusted package. A later unmatched SourceVersion clears attached
source bytes from the replica. Code executes locally after successful hydration.
The format-4 runtime-distribution planner and Player provisioning use the same
bounded set of clonable direct ScreenGui templates. The package includes only
eligible client Script and ModuleScript bindings from those templates. The
two endpoints bind the projected code identity into the existing session
contract. Distributed bindings identify the canonical service by
`StarterGui` even when the author renames it. A client Script or ModuleScript
inside a clonable ScreenGui can sit beneath another Script. The enclosing
server Script's source stays in the trusted package. A source-only legacy
distributed package keeps its original projection.
Server-created structural content without matching package provenance remains presentation
data. Existing action, input, and remote authority policies apply to gameplay
requests made from UI.

Destroying or disabling a Script retires pending task-scheduler entries for
its original coroutine, including when a disable and re-enable requests a
fresh run. Independently spawned coroutines retain their scheduler lifetime.

## Native regression coverage

`gargantuan_player_runtime_tests` covers staged provisioning, independent player
roots, later Character reset, callback reentry, teardown, and persistence
exclusion. `gargantuan_gui_foundation_tests` covers authoring preview input and
geometry isolation, shared image residency, and legacy and local-root layout.
`gargantuan_game_session_tests --player-gui` exercises two simulated peers,
late server GUI changes, packaged source hydration and denial, client-local
changes, Character replacement, and disconnect cleanup.
