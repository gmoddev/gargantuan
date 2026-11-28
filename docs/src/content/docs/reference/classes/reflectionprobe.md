---
title: ReflectionProbe
description: Public creator reference for ReflectionProbe.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Bounded local environment source with a spatial influence volume; the global Sky remains independent.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### ReflectionProbe.ProbeBlendDistance

Inner boundary fade distance in world units.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetProbeBlendDistance",
  "OverrideWrite": "SetProbeBlendDistance",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeBoxProjection

Correct specular parallax against the oriented influence box.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeCFrame

Rigid influence pose relative to the nearest BasePart, otherwise world-relative.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "OverrideRead": "GetProbeCFrame",
  "OverrideWrite": "SetProbeCFrame",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeCaptureOffset

Captured cube origin in influence-box local coordinates.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f)",
  "OverrideRead": "GetProbeCaptureOffset",
  "OverrideWrite": "SetProbeCaptureOffset",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeEnabled

Participates in local environment lighting.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeIntensity

Linear multiplier for this probe radiance.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetProbeIntensity",
  "OverrideWrite": "SetProbeIntensity",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeNegativeX

Strict Image asset reference in cube-local NegativeX direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbeNegativeX",
  "OverrideWrite": "SetProbeNegativeX",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeNegativeY

Strict Image asset reference in cube-local NegativeY direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbeNegativeY",
  "OverrideWrite": "SetProbeNegativeY",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeNegativeZ

Strict Image asset reference in cube-local NegativeZ direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbeNegativeZ",
  "OverrideWrite": "SetProbeNegativeZ",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbePositiveX

Strict Image asset reference in cube-local PositiveX direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbePositiveX",
  "OverrideWrite": "SetProbePositiveX",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbePositiveY

Strict Image asset reference in cube-local PositiveY direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbePositiveY",
  "OverrideWrite": "SetProbePositiveY",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbePositiveZ

Strict Image asset reference in cube-local PositiveZ direction.

```json
{
  "Type": "t.String",
  "Unmodified": "\"\"",
  "OverrideRead": "GetProbePositiveZ",
  "OverrideWrite": "SetProbePositiveZ",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbePriority

Higher priorities consume local influence before lower priorities.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "OverrideRead": "GetProbePriority",
  "OverrideWrite": "SetProbePriority",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ReflectionProbe.ProbeSize

Full oriented influence-box dimensions in world units.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(16.0f)",
  "OverrideRead": "GetProbeSize",
  "OverrideWrite": "SetProbeSize",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
