---
title: TweenService
description: Public creator reference for TweenService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Creates runtime tweens of writable properties on live Instances.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("TweenService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### TweenService.Create

Validates and captures a bounded table of end values. Playback snapshots initial values when Play starts.

```text
(self: TweenService, target: Instance, info: TweenInfo, goals: { [string]: any }) -> Tween
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
