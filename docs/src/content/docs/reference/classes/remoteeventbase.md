---
title: RemoteEventBase
description: Public creator reference for RemoteEventBase.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Internal base for bounded remote events

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [RemoteBase](./remotebase.md).

Inherited members from [Instance](./instance.md).

## Properties

### RemoteEventBase.OnClientEvent

Fires for a validated server event

```json
{
  "Type": "t.RemoteSignal",
  "Unmodified": "std::shared_ptr<BaseSignal>{}",
  "OverrideRead": "GetOnClientEvent",
  "Writable": false,
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.OnPlayerEvent

Fires on the server with the current sending Player followed by the arguments

```json
{
  "Type": "t.RemoteSignal",
  "Unmodified": "std::shared_ptr<BaseSignal>{}",
  "OverrideRead": "GetOnPlayerEvent",
  "Writable": false,
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.OnServerEvent

Fires on the server with the sender's peer context followed by the arguments

```json
{
  "Type": "t.RemoteSignal",
  "Unmodified": "std::shared_ptr<BaseSignal>{}",
  "OverrideRead": "GetOnServerEvent",
  "Writable": false,
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### RemoteEventBase.FireAllClients

Sends bounded arguments independently to every eligible peer

```text
(self: RemoteEventBase, ...RemoteValue) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.FireClient

Submits to one current Player or to an explicit peer slot and generation

```text
((self: RemoteEventBase, Target: Player, ...RemoteValue) -> ()) & ((self: RemoteEventBase, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> ())
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.FireServer

Sends bounded arguments from a client to its server

```text
(self: RemoteEventBase, ...RemoteValue) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.TryFireAllClients

Returns accepted and rejected counts plus rejected counts by reason code

```text
(self: RemoteEventBase, ...RemoteValue) -> (number, number, {[string]: number})
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.TryFireClient

Returns local admission and a reason code for submission to one Player or explicit peer

```text
((self: RemoteEventBase, Target: Player, ...RemoteValue) -> (boolean, string)) & ((self: RemoteEventBase, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> (boolean, string))
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteEventBase.TryFireServer

Returns local admission and a reason code for submission to the current server

```text
(self: RemoteEventBase, ...RemoteValue) -> (boolean, string)
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
