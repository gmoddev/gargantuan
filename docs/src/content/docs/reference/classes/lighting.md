---
title: Lighting
description: Public creator reference for Lighting.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Owns the authored global environment and lighting semantics for one DataModel.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("Lighting")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

### Lighting.Ambient

Per-channel ambient contribution applied to lit world surfaces.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(0.2f, 0.2f, 0.2f)",
  "OverrideRead": "GetAmbient",
  "OverrideWrite": "SetAmbient",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting",
  "EditorHint": "ColorComponents",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.Brightness

Bounded multiplier for direct sun irradiance; it does not scale Ambient or Sky pixels.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetBrightness",
  "OverrideWrite": "SetBrightness",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting",
  "Range": "{ minimum = 0, maximum = 8 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.ClockTime

Authored cyclic time in hours within the strict range 0 <= ClockTime < 24.

```json
{
  "Type": "t.Float",
  "Unmodified": "12.0f",
  "OverrideRead": "GetClockTime",
  "OverrideWrite": "SetClockTime",
  "Serializable": true,
  "Replicated": true,
  "Category": "Time",
  "Range": "{ minimum = 0, maximum = 24 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.EnvironmentColor

Visual fallback background color used when no coherent Sky is available.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(0.08f, 0.12f, 0.2f)",
  "OverrideRead": "GetEnvironmentColor",
  "OverrideWrite": "SetEnvironmentColor",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "EditorHint": "ColorComponents",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.EnvironmentRotation

Yaw in degrees around world +Y for the visible Sky and its image-based lighting.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetEnvironmentRotation",
  "OverrideWrite": "SetEnvironmentRotation",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "Range": "{ minimum = -180, maximum = 180 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.ExposureCompensation

Photographic exposure compensation in stops; the renderer applies 2 raised to this value.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetExposureCompensation",
  "OverrideWrite": "SetExposureCompensation",
  "Serializable": true,
  "Replicated": true,
  "Category": "Exposure",
  "Range": "{ minimum = -8, maximum = 8 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.FogColor

Color approached by world geometry at FogEnd.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(0.5f, 0.6f, 0.7f)",
  "OverrideRead": "GetFogColor",
  "OverrideWrite": "SetFogColor",
  "Serializable": true,
  "Replicated": true,
  "Category": "Fog",
  "EditorHint": "ColorComponents",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.FogEnabled

Whether simple camera-distance linear fog affects world geometry.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "OverrideRead": "GetFogEnabled",
  "OverrideWrite": "SetFogEnabled",
  "Serializable": true,
  "Replicated": true,
  "Category": "Fog"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.FogEnd

Camera distance at or above which world geometry is fully FogColor.

```json
{
  "Type": "t.Float",
  "Unmodified": "1000.0f",
  "OverrideRead": "GetFogEnd",
  "OverrideWrite": "SetFogEnd",
  "Serializable": true,
  "Replicated": true,
  "Category": "Fog",
  "Range": "{ minimum = 0, maximum = 100000 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.FogStart

Camera distance at or below which no fog is applied.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetFogStart",
  "OverrideWrite": "SetFogStart",
  "Serializable": true,
  "Replicated": true,
  "Category": "Fog",
  "Range": "{ minimum = 0, maximum = 100000 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.IblIntensity

Linear multiplier for diffuse and specular image-based lighting.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetIblIntensity",
  "OverrideWrite": "SetIblIntensity",
  "Serializable": true,
  "Replicated": true,
  "Category": "Environment",
  "Range": "{ minimum = 0, maximum = 8 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.ShadowCascadeCount

Camera-relative directional cascade count; zero retains the existing fixed shadow volume.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Range": "{ minimum = 0, maximum = 4 }",
  "Serializable": true,
  "Replicated": true,
  "Category": "Shadows"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.ShadowDistance

Maximum camera-relative directional shadow coverage in world units.

```json
{
  "Type": "t.Float",
  "Unmodified": "250.0f",
  "Range": "{ minimum = 1, maximum = 4096 }",
  "Serializable": true,
  "Replicated": true,
  "Category": "Shadows"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.SunColor

Art-directable color of the global directional sun.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "OverrideRead": "GetSunColor",
  "OverrideWrite": "SetSunColor",
  "Serializable": true,
  "Replicated": true,
  "Category": "Lighting",
  "EditorHint": "ColorComponents",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Lighting.ToneMapping

Tone-mapping operator applied to exposed world color before GUI composition.

```json
{
  "Type": "t.EnumItem(\"ToneMappingMode\")",
  "Unmodified": "Enums::ToneMappingMode::Filmic",
  "OverrideRead": "GetToneMapping",
  "OverrideWrite": "SetToneMapping",
  "Serializable": true,
  "Replicated": true,
  "Category": "Exposure"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
