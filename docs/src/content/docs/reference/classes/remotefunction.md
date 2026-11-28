---
title: RemoteFunction
description: Public creator reference for RemoteFunction.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Bounded reliable request and terminal response

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [RemoteBase](./remotebase.md).

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### RemoteFunction.InvokeClient

Suspends only the calling server coroutine until one client returns

```text
((self: RemoteFunction, Target: Player, ...RemoteValue) -> ...RemoteValue) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> ...RemoteValue)
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.InvokeClientWithTimeout

Invokes one client with a finite caller-selected deadline

```text
((self: RemoteFunction, Target: Player, TimeoutSeconds: number, ...RemoteValue) -> ...RemoteValue) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, TimeoutSeconds: number, ...RemoteValue) -> ...RemoteValue)
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.InvokeServer

Suspends only the calling client coroutine until a terminal result

```text
(self: RemoteFunction, ...RemoteValue) -> ...RemoteValue
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.InvokeServerWithTimeout

Invokes the server with a finite caller-selected deadline

```text
(self: RemoteFunction, timeoutSeconds: number, ...RemoteValue) -> ...RemoteValue
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.SetClientHandler

Installs the client request callback for this runtime-only session

```text
(self: RemoteFunction, callback: (...RemoteValue) -> ...RemoteValue) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.SetPlayerHandler

Installs the server request callback with the current Player as its first argument

```text
(self: RemoteFunction, Callback: (Player, ...RemoteValue) -> ...RemoteValue) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.SetServerHandler

Installs the server request callback with peer context as its first argument

```text
(self: RemoteFunction, callback: (RemotePeer, ...RemoteValue) -> ...RemoteValue) -> ()
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.TryInvokeClient

Invokes one Player or explicit peer with a deadline and a boolean before results or failure details

```text
((self: RemoteFunction, Target: Player, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue)) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue))
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RemoteFunction.TryInvokeServer

Invokes the server with an explicit deadline and a boolean before results or failure details

```text
(self: RemoteFunction, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue)
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
