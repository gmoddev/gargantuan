---
title: GuiBase2d
description: Public creator reference for GuiBase2d.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Intermediate abstract class for instances that compose 2D graphical user interface

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### GuiBase2d.AbsolutePosition

Screen position of this instance in logical pixels

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.AbsoluteRotation

Screen rotation of this instance in degrees

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.AbsoluteSize

Screen size of this instance in logical pixels

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.AccessibilityRole

Explicit semantic accessibility role, or Automatic for class semantics

```json
{
  "Type": "t.EnumItem(\"AccessibilityRole\")",
  "Unmodified": "Enums::AccessibilityRole::Automatic",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.AccessibilitySelected

Whether this semantic accessibility node is selected

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.AccessibleName

Renderer-independent accessible label for this object

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.Opacity

Local opacity multiplied through descendants

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

### GuiBase2d.Activated

Fired when this semantic control is activated

```json
{
  "Args": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.FocusLost

Fired when this object loses focus

```json
{
  "Args": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.Focused

Fired when this object receives focus

```json
{
  "Args": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.PointerDown

Fired during capture, target, and bubble pointer-down routing

```json
{
  "Args": "{ t.Instance(\"GuiInputEvent\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.PointerEnter

Fired when a pointer begins hovering this object

```json
{
  "Args": "{ t.Instance(\"GuiInputEvent\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.PointerLeave

Fired when a pointer stops hovering this object

```json
{
  "Args": "{ t.Instance(\"GuiInputEvent\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.PointerMoved

Fired during routed pointer movement

```json
{
  "Args": "{ t.Instance(\"GuiInputEvent\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiBase2d.PointerUp

Fired during capture, target, and bubble pointer-up routing

```json
{
  "Args": "{ t.Instance(\"GuiInputEvent\") }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
