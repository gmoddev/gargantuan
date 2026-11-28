---
title: Sky
description: Public creator reference for Sky.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Declares one six-face visual skybox; it is effective only as a direct Lighting child.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Sky.Enabled

Whether this Sky participates in deterministic effective-Sky selection.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "OverrideRead": "GetEnabled",
  "OverrideWrite": "SetEnabled",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxNegativeX

Strict Image asset reference displayed toward world -X.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxNegativeX",
  "OverrideWrite": "SetSkyboxNegativeX",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxNegativeY

Strict Image asset reference displayed toward world -Y.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxNegativeY",
  "OverrideWrite": "SetSkyboxNegativeY",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxNegativeZ

Strict Image asset reference displayed toward world -Z.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxNegativeZ",
  "OverrideWrite": "SetSkyboxNegativeZ",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxPositiveX

Strict Image asset reference displayed toward world +X.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxPositiveX",
  "OverrideWrite": "SetSkyboxPositiveX",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxPositiveY

Strict Image asset reference displayed toward world +Y.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxPositiveY",
  "OverrideWrite": "SetSkyboxPositiveY",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sky.SkyboxPositiveZ

Strict Image asset reference displayed toward world +Z.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSkyboxPositiveZ",
  "OverrideWrite": "SetSkyboxPositiveZ",
  "Serializable": true,
  "Replicated": true,
  "Category": "Sky",
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
