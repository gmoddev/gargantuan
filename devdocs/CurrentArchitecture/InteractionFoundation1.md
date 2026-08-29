---
status: current
owner: runtime
last_verified: 2026-08-29
related_code:
  - assets/classes/ProximityPrompt.luau
  - assets/services/InteractionService.luau
  - assets/runtime/DefaultInteractionRuntime.luau
  - include/gargantuan/services/InteractionService.hpp
  - include/gargantuan/runtime/SemanticSpatialResolver.hpp
  - src/runtime/SemanticSpatialResolver.cpp
  - src/services/InteractionService.cpp
  - tests/InteractionFoundationTests.cpp
  - tests/InteractionFoundationBenchmark.cpp
related_adrs: []
---

# Interaction / proximity foundation 1

## Public object model

`ProximityPrompt` is a constructible schema-backed `Instance`. It has six
saved, future-replicated authoring properties:

| Property | Contract |
| --- | --- |
| `Enabled` | Boolean eligibility switch; defaults to `true`. |
| `ActionText` | UTF-8 presentation verb, at most 64 bytes; defaults to `Interact`. |
| `ObjectText` | Optional UTF-8 target label, at most 64 bytes. |
| `MaxActivationDistance` | Finite positive world distance in `[0.01, 64]`; defaults to 10. |
| `HoldDuration` | Finite monotonic hold time in `[0, 30]` seconds; defaults to zero. |
| `RequiresLineOfSight` | Boolean rigid-physics visibility requirement; defaults to `false` for compatibility. |

`Triggered(Player)` is the only gameplay event in Foundation 1. It fires once
after Engine revalidates a completed activation. There is no prompt-owned
position, physical key, priority, cooldown, current player, or runtime progress
property.

## Runtime authority and location

`InteractionService` is the one runtime owner. It discovers descendants,
maintains the spatial index and per-player state, chooses candidates, consumes
the `Interact` semantic action, times holds, performs final validation, fires
`Triggered`, and publishes a read-only local presentation snapshot. Gameplay
normally listens to `Prompt.Triggered`; it does not scan or call a targeting
API each frame.

A prompt resolves its anchor through the shared semantic spatial resolver.
Ordinary chains still compose zero or more `Attachment.CFrame` transforms to
the first ancestor `BasePart.CFrame`. A `JointPath`-bound Attachment beneath a
skinned MeshPart instead contributes the final renderer-independent animation
pose, owner CFrame/Size, and local offset described by [Animation Foundation 2B](AnimationFoundation2SemanticAnchors.md).
A prompt with no live BasePart ancestor is inert. Transform, transient
`WorldCFrame`, ancestry, enablement, and destruction signals dirty only that
prompt record. Renderer geometry, palettes, picking data, and physics backend
handles are not location authority.

The interaction origin is the authoritative `Player.Character`, which must be
a live `KinematicCharacter`; its current `Position` is read at query and final
activation time. Luau cannot submit an arbitrary position. Character
replacement naturally replaces the origin on the next semantic update.

## Spatial lookup and scheduling

Live enabled prompts occupy a native 16-unit spatial hash. Registration and
removal are incremental. Property changes coalesce in a dirty ObjectId set and
re-index in deterministic ObjectId order. Queries inspect the fixed 9 x 9 x 9
cell neighborhood implied by the maximum 64-unit distance. They therefore
consider nearby indexed prompts rather than every world prompt in ordinary
sparse scenes.

For each player, selection is:

1. eligible within that prompt's exact `MaxActivationDistance`;
2. shortest squared world distance;
3. lowest stable generation-safe `ObjectId` for an exact-distance tie;
4. when required, visible through the general `Workspace:Raycast` boundary.

Unordered-container iteration never decides the active prompt. Foundation 1
does not add a speculative priority property. Distance sorting happens before
LOS. Selection retains only the best eight candidates in a fixed native array,
and at most those eight are raycast for one player query.

LOS starts at the authoritative character position and ends at the exact
current resolved prompt anchor, including animated joint and Attachment
offsets. `RaycastParams` excludes
the interacting player's own character. A miss reaches the target, and a hit
on a Part in the prompt's parent/anchor hierarchy is accepted; another closest
rigid hit blocks it. `RequiresLineOfSight=false` performs no raycast. See
[Physics Query Foundation 1](PhysicsQueryFoundation1.md) for query identity,
filter, bound, and timing semantics.

Engine evaluates interaction on Main after `RunService.PostSimulation` and
before `PreRender`. Normal availability work is capped at a 33 ms interval;
input edges and dirty prompts force an immediate semantic update. Hold progress
uses `steady_clock`, not frames, and viewport/render cadence is unrelated.

## Input and activation

The shipped trusted `DefaultInteractionRuntime.luau` adds two ActionMap
bindings for the semantic `Interact` action:

- keyboard `E`;
- normalized gamepad South / `KeyCode.ButtonA`.

The physical key is policy, not `ProximityPrompt` API. SDL host input still
flows through `UserInputService` and `ActionMap`. GUI consumes input first;
focused `TextBox` key input cannot leak into interaction, and consumed release
events still retire prior ActionMap state. OS key repeats do not create a new
press edge. Focus loss cancels an active hold.

On every press, hold update/completion, and final trigger, Engine re-resolves
the Player, character, prompt, anchor, enabled state, finite distance, exact
range, and required LOS before firing.
A zero-duration prompt fires on the valid press edge and then waits for a full
release. A positive-duration prompt records monotonic start time, publishes
bounded progress, and cancels on release, active-candidate replacement, range
loss, disable/destruction, focus loss, runtime Stop, or world teardown. Size and
state are changed before `Triggered`, so a callback may disable, destroy, or
reparent the prompt, destroy its parent, or tear down runtime state without
leaving an iterator or raw pointer live.

Animated `WorldCFrame` changes coalesce in a reused dirty vector. They refresh
the prompt position/cell without rebuilding its ancestor subscriptions unless
topology changed. An arm moving out of range therefore cancels a hold and final
validation rejects a stale displayed prompt. Moving the animated endpoint
behind a rigid blocker changes the ray destination, but the skinned Mesh itself
does not become raycast geometry. The normal 33 ms cadence remains; a dirty
animated prompt may request immediate semantic reevaluation without tying the
service to GPU frame rate.

## Presentation boundary

Native `InteractionService` exposes read-only `ActivePrompt`, `Available`,
`ActionText`, `ObjectText`, `HoldDuration`, `HoldProgress`, `InputHint`, and
`DefaultPresentationEnabled`, plus bounded `PresentationChanged`.
`BeginActivation()` and `EndActivation()` let the trusted touch presentation
join the same native validation and hold path; neither method directly fires a
prompt.

The engine-shipped Luau module creates an unarchivable fixed screen-space
`ScreenGui` with a high-contrast `TextButton`, `[ E / A ]` hint, action/object
text, and hold progress bar. The 88-pixel panel is also the touch target. It is
ordinary GUI above renderer publication; there is no prompt renderer branch,
saved runtime widget, or `GuiRuntime` prompt authority. Fixed screen-space
presentation was selected because Gargantuan does not yet have a world-space
GUI/projection contract. The semantic snapshot leaves room for a future trusted
replacement presentation without moving validation out of native runtime.

Headless or renderer-unavailable Engine instances keep the default ScreenGui
absent while discovery, selection, hold timing, and `Triggered` remain fully
functional. Interaction has no Node, entitlement, MCP, Studio, telemetry, or
network dependency.

## Persistence, Studio, and Play

Only the six authored prompt properties persist. Candidate identity, player
state, progress, spatial cells, and presentation state are runtime-only and do
not publish authoring journal records. Stop destroys all subscriptions,
bindings, runtime UI, candidates, and holds. FirstCompleteGame's ten Play/Stop
gate proves runtime-disabled prompts do not change the authoring snapshot.

No Studio production special case was needed. Frozen runtime schema discovery
marks `Engine.ProximityPrompt` constructible and exposes all six fields as
editable. The generic EditorHost/Studio command path is tested for Insert,
typed property edit, Undo/Redo, duplicate, reparent, delete, Save, reopen, and
Play. All mutations continue through the existing authoritative gateway and
journal.

## Bounds and failure policy

The service has explicit per-world limits: 16,384 registered prompts, 64 live
player states, 16,384 considered prompts per player query, 16-unit cells,
absolute coordinates no larger than 10,000,000, 64-byte text fields, 64-unit
distance, and 30-second holds. Spatial cells, dirty IDs, subscriptions, and
active states are consequently bounded by registered prompts/players. Invalid
or non-finite anchors and origins fail closed. Additional prompts past the cap
remain inert and emit one bounded warning rather than growing state.

## Release benchmark evidence

The Release benchmark uses 1, 100, 1K, and 10K prompt worlds; sparse, dense,
occluded, far, and disabled distributions; both LOS settings; and 1, 4, and 16
synthetic query origins. It
reports registration, a 128-record dirty property batch, query/filter/selection,
bounded raycast count, and native hold/presentation updates. These local
2026-08-26 results are
regression evidence, not an SLA or an expected 10K-game workload:

| Scenario | LOS | 16-player P50 / P95 / P99 | Mean considered / rays per player |
| --- | --- | ---: | ---: |
| 100 dense | off | 0.118 / 0.123 / 0.172 ms | 100 / 0 |
| 100 dense | on | 0.503 / 0.510 / 0.575 ms | 100 / 8 |
| 1K dense | off | 0.450 / 0.470 / 0.579 ms | 1,000 / 0 |
| 1K dense | on | 1.992 / 2.489 / 2.556 ms | 1,000 / 8 |
| 10K sparse | off | 0.112 / 0.134 / 0.153 ms | 16 / 0 |
| 10K sparse | on | 0.145 / 0.170 / 0.189 ms | 16 / 1 |
| 10K dense stress | off | 5.820 / 6.768 / 7.709 ms | 10,000 / 0 |
| 10K dense stress | on | 8.500 / 10.185 / 13.252 ms | 10,000 / 4.5 |
| 10K occluded stress | on | 7.063 / 9.492 / 10.954 ms | 10,000 / 7.875 |

Dense 10K is intentionally the worst bounded case because every prompt occupies
the queried cells. Its LOS-on hold/presentation update P99 was 0.418 ms. Sparse query
cost remains governed by nearby candidates; the large registration numbers
also include creation and authoritative DataModel parenting for every Part and
prompt, not only hash insertion.

FirstCompleteGame previously ran a Luau `PreSimulation` callback that compared
the character position with all three collectibles every frame. It now has
three authored prompts and three `Triggered` connections. No Luau distance loop
remains; native availability evaluates at semantic cadence and final activation
has Engine validation. The ordinary game script still owns collection, round
state, obstacle motion, and reset.

## Future authority and explicit deferrals

Foundation 1 is local/single-process. Its seam separates local presentation
eligibility from semantic activation validation: a future network client can
request an activation by prompt identity while authoritative server runtime
re-resolves player position, anchor, range, enabled state, and timing. Client
world coordinates will not be accepted as proximity proof.

Deferred work includes custom/rebindable/device-aware presentation,
server/client request replication,
prompt priority, shown/hidden/start/end gameplay signals, interaction trees,
dialogue, inventory/quests, SurfaceGui/world-space UI, theming, analytics,
accessibility platform integration, cones/gaze/VR, radial menus, NPC activation,
and arbitrary script scoring callbacks.
