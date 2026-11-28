---
title: ImageLabel
description: Public creator reference for ImageLabel.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A retained GUI object that presents a logical image resource

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

## Declared default overrides

Derived defaults override inherited declaration defaults. Expressions below preserve the authoritative declaration.

```text
{ { Property = "BackgroundTransparency", Value = 1 }, }
```

Inherited members from [GuiObject](./guiobject.md).

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### ImageLabel.Image

Strict AssetService image reference

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetImage",
  "OverrideWrite": "SetImage",
  "Serializable": true,
  "Replicated": true,
  "EditorHint": "AssetReference:Image"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ImageLabel.ImageColor3

Image tint color

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ImageLabel.ImageTransparency

Image transparency from zero opaque to one transparent

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ImageLabel.ScaleType

Stretch fills the control; Fit centers the whole image at its original aspect ratio.

```json
{
  "Type": "t.EnumItem(\"GuiImageScaleType\")",
  "Unmodified": "Enums::GuiImageScaleType::Stretch",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
