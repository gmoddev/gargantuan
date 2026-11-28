---
title: Players
description: Public creator reference for Players.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Owns DataModel-scoped gameplay player identities and local player lifecycle.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("Players")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

### Players.CharacterAutoLoads

Whether shipped offline/server policy requests a Character for each authoritative Player.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Players.DefaultCameraEnabled

Whether the engine-shipped default camera module starts for LocalPlayer.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Players.DefaultControllerEnabled

Whether the engine-shipped default controller module starts for LocalPlayer.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Players.LocalPlayer

The trusted runtime-local Player selected by offline bootstrap or accepted network session identity.

```json
{
  "Type": "t.Optional(t.Instance(\"Player\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetLocalPlayer",
  "Writable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### Players.GetPlayers

Returns the current Players in deterministic PlayerId order.

```json
{
  "Returns": "t.Array(t.Instance(\"Player\"))"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### Players.PlayerAdded

Fired after a Player enters this DataModel player lifetime.

```json
{
  "Args": "{ t.Instance(\"Player\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Players.PlayerRemoving

Fired before a Player leaves this DataModel player lifetime.

```json
{
  "Args": "{ t.Instance(\"Player\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
