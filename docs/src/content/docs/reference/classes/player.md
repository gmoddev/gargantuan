---
title: Player
description: Public creator reference for Player.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A gameplay player identity whose lifetime is independent from transport connections.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Player.Character

The current runtime character, or nil while no character is spawned.

```json
{
  "Type": "t.Optional(t.Instance(\"Character\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetCharacter",
  "OverrideWrite": "SetCharacter",
  "Replicated": true,
  "MaterializationDependency": "Hard"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.PlayerGui

The runtime-owned screen interface container for this player.

```json
{
  "Type": "t.Optional(t.Instance(\"PlayerGui\"))",
  "Unmodified": "std::nullopt",
  "Writable": false,
  "OverrideRead": "GetPlayerGui"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.PlayerId

Nonzero runtime player identity independent of ConnectionId.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Writable": true,
  "OverrideRead": "GetPlayerId",
  "OverrideWrite": "SetReplicatedPlayerId",
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.PossessedEntity

The current server-assigned gameplay entity, or nil if no target is available in this view.

```json
{
  "Type": "t.Optional(t.Instance(\"Instance\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetPossessedEntity",
  "OverrideWrite": "SetReplicatedPossessedEntity",
  "WriteLevel": "Engine",
  "Editable": false,
  "Replicated": true,
  "MaterializationDependency": "Soft"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### Player.LoadCharacter

Removes the current character and requests a fresh character assembly.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.RemoveCharacter

Clears and deterministically destroys the current character.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.RequestSpatialTransfer

Requests admission for the player's controlled root to a destination WorldRoot. A running server Script owns the resulting status handle.

```text
[=[(self: Player, Destination: WorldRoot, DestinationPose: CFrame, Minimum: Vector3, Maximum: Vector3) -> ({ GetStatus: (self: unknown) -> ("Pending" | "Applied" | "CommittedUnconfirmed" | "Canceled" | "Stale" | "Denied" | "Busy" | "Expired"), Cancel: (self: unknown) -> ("Pending" | "Applied" | "CommittedUnconfirmed" | "Canceled" | "Stale" | "Denied" | "Busy" | "Expired") }?, string?)]=]
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.ResetCharacter

Resets the current character through the same spawn contract.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.SetPossession

Assigns an unoccupied live Workspace entity, or releases the current target with nil. Server only.

```json
{
  "RequiredCapability": "MutateDataModel",
  "Params": "{ { name = \"Entity\", type = t.Optional(t.Instance(\"Instance\")) } }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### Player.CharacterAdded

Fired after a new character relationship is committed.

```json
{
  "Args": "{ t.Instance(\"Character\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.CharacterRemoving

Fired before the old character is destroyed or externally removed.

```json
{
  "Args": "{ t.Instance(\"Character\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Player.CharacterSpawnRequested

Fired synchronously so replaceable Luau assembly policy can create a character.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
