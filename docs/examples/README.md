# Luau examples

Author these sources through Studio, or place recognized `.luau`, `.client.luau`, and `.server.luau` files in a trusted FileLink source directory for startup-time import. FileLink creates a `ModuleScript` for `.luau` and a `Script` for the context-specific suffixes. It is not a live sync or write API. Set `RunContext` as noted in each script. The examples are parser-checked; only the animation policy has a separate mock-driven Luau behavior test. Each script checks missing setup or recoverable API errors and uses `[System:SubSystem]` diagnostics.

| File | Setup |
| --- | --- |
| `part-and-tags.luau` | Server Script in a running DataModel. |
| `math-filter-signal.luau` | Server Script with `Workspace`; demonstrates VM-local `RaycastParams`, `Vector3`, and `Signal`. |
| `local-light.luau` | Server Script with `Workspace`; creates a point light on an anchored Part. |
| `world-query.luau` | Server Script with `Workspace`; run after the example part or other collidable geometry exists. |
| `remote-server.luau` and `remote-client.luau` | A real client/server session, with a pre-authored `RemoteEvent` named `Ping` under `ReplicatedStorage`; place the scripts in Server and Client contexts respectively. Local Studio Play alone has no peer topology. |
| `CharacterAnimationPolicy.luau` and `CharacterAnimationDriver.client.luau` | A project `ModuleScript` named `CharacterAnimationPolicy` under `ReplicatedStorage`, and a Client Script. See the setup below. |
| `CharacterActionServer.server.luau` and `CharacterActionClient.client.luau` | A real client/server session with the same ready Animation reference on both sides and one server-owned action admission policy. See the action setup below. |

The [API reference](../src/content/docs/api/index.mdx) explains authority, failure, and lifecycle limits. Copies of an `Instance` or `Remote` in a local variable remain valid only during the owning DataModel/session lifetime.

## Character animation policy setup

Import a **skinned glTF Mesh** with at least two clips that share its canonical skeleton. Copy the Mesh reference and the `Idle` and `Move` Animation references from the asset catalog. In your game's Character assembly, make `Character.RootPart` a `MeshPart` with that Mesh reference, keep the root anchored and non-collidable as in the shipped assembly, and put an `Animator` directly under it before assigning the Character to a Player. The shipped default assembly creates a plain `Part`, so this is an explicit game change. Keep Character as the semantic actor; the visual policy never writes its transform.

Place `CharacterAnimationPolicy.luau` in `ReplicatedStorage` as a `ModuleScript` named `CharacterAnimationPolicy`. Place `CharacterAnimationDriver.client.luau` as a Client Script, and set its `IdleReference` and `MoveReference` string attributes to the two Animation references. The driver requires `@game/ReplicatedStorage/CharacterAnimationPolicy`, attaches on `Player.CharacterAdded`, and chooses `Idle` or `Move` from the existing ActionMap movement actions. Those choices are ordinary game policy; call `Controller:Request(StateName, FadeSeconds)` from your own input, combat, or AI policy for other states. `Policy.New(Animator, RunService, Clips)` accepts one to eight named clip entries. Each entry has `Name`, `Reference`, optional finite `Speed` (0–16), and optional `Looped`. Fades must be finite 0–5 seconds. A new request during a fade starts from the current weights; completion stops the old tracks. Repeating the current request leaves playback alone. `Close()` disconnects the tick and stops every loaded track. A non-looping state remains selected after its natural end until the caller selects another state.

`Animator:LoadAnimation` can fail for missing content, an incompatible skeleton, or the Animator's track capacity. The driver catches construction errors and reports them. The policy has no gameplay movement authority and never enables `AnimationTrack.RootMotionEnabled`.

## Network action and root-motion request setup

The two `CharacterAction` scripts demonstrate a separate authoritative action route. Set the same `AnimationReference` string attribute on both scripts to a ready imported Animation asset, run the server and client contexts in a real session, and integrate the server callback with your **one** game-owned `SetActionPolicy` callback. This standalone server sample installs its own callback and therefore should be adapted if your game already has one. The client binds `F` as `CreatorLunge`; remove or change that binding to fit your ActionMap. Both sides register the same action name, pinned animation reference, 0.5-second duration, and requested 0.9-stud translation. These values are explicit action policy parameters; they are not extracted from clip keys. The native action path evaluates the requested motion through Character authority and collision admission. `RequestAction` returning true means submitted. `ActionResolved` reports server acceptance or denial; neither script sets Character position or treats submission as acceptance. The sample sets `Predictable` to false, so it requests server-authoritative presentation.
