---
title: SpotLight
description: Public creator reference for SpotLight.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A local emitter directed along its LightCFrame's negative Z axis.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [LocalLight](./locallight.md).

Inherited members from [Instance](./instance.md).

## Properties

### SpotLight.LightConeAngles

Inner and outer cone half angles in degrees. The outer angle is 1 to 89 degrees and the inner angle is 0 through the outer angle.

```json
{
  "Type": "t.Vector2",
  "Unmodified": "Vector2(20.0f, 35.0f)",
  "OverrideRead": "GetLightConeAngles",
  "OverrideWrite": "SetLightConeAngles",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
