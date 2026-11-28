---
title: InputService
description: Public creator reference for InputService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Owns persistent input contexts and local derived action evaluation.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("InputService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### InputService.AddContext

Creates and parents a canonical InputContext after validating its options.

```text
(Name: string, Options: { Priority: number?, Enabled: boolean?, Consume: boolean? }?) -> InputContext
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputService.GetRuntimeSnapshot

Returns a bounded read-only copy of local compiled state and diagnostics.

```text
() -> { [string]: any }
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
