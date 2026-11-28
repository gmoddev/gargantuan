---
title: TextLabel
description: Public creator reference for TextLabel.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A retained GUI object that presents shaped UTF-8 text

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

### TextLabel.FontFace

Strict AssetService font reference

```json
{
  "Type": "t.String",
  "Unmodified": "'\"builtin://font/default\"'",
  "OverrideRead": "GetFontFace",
  "OverrideWrite": "SetFontFace",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.Text

UTF-8 text content

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.TextColor3

Text color

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.TextSize

Logical text size before DPI scaling

```json
{
  "Type": "t.Float",
  "Unmodified": "16.0f",
  "Serializable": true,
  "Replicated": true,
  "Range": "{ minimum = 1, maximum = 256 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.TextTransparency

Text transparency from zero opaque to one transparent

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

### TextLabel.TextWrapped

Whether text wraps at the arranged width

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.TextXAlignment

Horizontal text alignment

```json
{
  "Type": "t.EnumItem(\"TextXAlignment\")",
  "Unmodified": "Enums::TextXAlignment::Center",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextLabel.TextYAlignment

Vertical text alignment

```json
{
  "Type": "t.EnumItem(\"TextYAlignment\")",
  "Unmodified": "Enums::TextYAlignment::Center",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
