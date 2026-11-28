---
title: RunService
description: Public creator reference for RunService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Provides access to the runtime.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("RunService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

## Signals

### RunService.PostSimulation

Fired after simulating WorldRoots. Listeners receive the delta time

```json
{
  "Args": "{ t.Float }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RunService.PreRender

Fired before rendering the scene. Listeners receive the delta time.

```json
{
  "Args": "{ t.Float }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### RunService.PreSimulation

Fired before simulating WorldRoots. Listeners receive the delta time

```json
{
  "Args": "{ t.Float }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
