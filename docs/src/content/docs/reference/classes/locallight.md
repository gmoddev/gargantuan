---
title: LocalLight
description: Public creator reference for LocalLight.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract emitter with a rigid pose relative to its nearest BasePart or WorldRoot.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### LocalLight.LightCFrame

Rigid emitter pose relative to the nearest BasePart, or in WorldRoot coordinates without a BasePart.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "OverrideRead": "GetLightCFrame",
  "OverrideWrite": "SetLightCFrame",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightColor

Linear RGB color of the emitted light.

```json
{
  "Type": "t.Color3",
  "Unmodified": "Color3(1.0f, 1.0f, 1.0f)",
  "OverrideRead": "GetLightColor",
  "OverrideWrite": "SetLightColor",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightEnabled

Whether the emitter contributes to the world lighting.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightIntensity

Candela-like scale for inverse-square falloff, from 0 to 4096.

```json
{
  "Type": "t.Float",
  "Unmodified": "16.0f",
  "OverrideRead": "GetLightIntensity",
  "OverrideWrite": "SetLightIntensity",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightRange

World-unit radius at which attenuation reaches zero, from 0.01 to 1024.

```json
{
  "Type": "t.Float",
  "Unmodified": "16.0f",
  "OverrideRead": "GetLightRange",
  "OverrideWrite": "SetLightRange",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightShadowPriority

Priority within the finite local-shadow budget, with stable identity ties.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Range": "{ minimum = -1024, maximum = 1024 }",
  "Serializable": true,
  "Replicated": true,
  "Category": "Shadows"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### LocalLight.LightShadows

Requests bounded dynamic shadows; unsupported or unadmitted lights remain unshadowed.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Shadows"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
