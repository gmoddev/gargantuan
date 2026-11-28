---
title: CharacterControlService
description: Public creator reference for CharacterControlService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A narrow runtime bridge between replaceable Character policy and native network authority.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("CharacterControlService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### CharacterControlService.GetPossessionInput

Consumes the authoritative possession input for a Player once per simulation tick. Server only.

```text
(self: CharacterControlService, Player: Player) -> { Movement: Vector2, FacingYawRadians: number, JumpRequested: boolean, Tick: number, AcknowledgedInput: number, AgeTicks: number, Fresh: boolean }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.MovePossessedPart

Commits one bounded server-chosen pose per simulation tick for a possessed anchored BasePart.

```text
(self: CharacterControlService, Player: Player, Transform: CFrame) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.RegisterAction

Registers bounded runtime action policy and pinned animation identity.

```text
(self: CharacterControlService, ActionName: string, AnimationReference: string, DurationSeconds: number, RootTranslation: Vector3?, RootYawRadians: number?, Predictable: boolean?) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.RequestAction

Requests a registered semantic action for the trusted local Character.

```text
(self: CharacterControlService, ActionName: string) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.SetActionPolicy

Installs server gameplay admission policy for registered semantic actions.

```text
(self: CharacterControlService, Callback: (Player, Character, string) -> boolean) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.SetMovementPolicy

Installs the runtime-local command-to-motion policy used by authority or prediction.

```text
(self: CharacterControlService, Callback: (Character, number, Vector2, number, boolean) -> (Vector3, Vector3, number)) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.SetPredictionEnabled

Selects local prediction or server-authoritative presentation for custom policy.

```text
(self: CharacterControlService, Enabled: boolean) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.SubmitMoveIntent

Submits bounded semantic movement intent for the trusted local Character.

```text
(self: CharacterControlService, MoveIntent: Vector2, FacingYawRadians: number, JumpRequested: boolean) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.SubmitPossessionIntent

Submits one bounded movement intent for the local Player's current possessed entity.

```text
(self: CharacterControlService, MoveIntent: Vector2, FacingYawRadians: number, JumpRequested: boolean) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### CharacterControlService.ActionEnded

Fired when a previously authoritative semantic Character action ends or is replaced.

```json
{
  "Args": "{ t.Instance(\"Character\"), t.String }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### CharacterControlService.ActionResolved

Fired when the server accepts or rejects a requested semantic Character action.

```json
{
  "Args": "{ t.Instance(\"Character\"), t.String, t.Bool }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
