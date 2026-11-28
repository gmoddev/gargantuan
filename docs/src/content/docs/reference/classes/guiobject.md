---
title: GuiObject
description: Public creator reference for GuiObject.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for instances that compose 2D graphical user interface

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### GuiObject.Active

Whether this instance is actively being interacted with

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.AnchorPoint

Origin point of this instance relative to it's absolute size

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.AutomaticSize

Whether resizing occurs based on child content

```json
{
  "Type": "t.EnumItem(\"AutomaticSize\")",
  "Unmodified": "Enums::AutomaticSize::None",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.BackgroundColor3

Background color of this instance

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.BackgroundTransparency

Background transparency of this instance

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.ClipsDescendants

Whether overflowing descendants are clipped by this instance

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.GuiState

Whether this instance is being actively pressed

```json
{
  "Type": "t.EnumItem(\"GuiState\")",
  "Unmodified": "Enums::GuiState::NonInteractable",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.InputSink

Whether this instance absorbs input events

```json
{
  "Type": "t.EnumItem(\"InputSink\")",
  "Unmodified": "Enums::InputSink::None",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Interactable

Whether this instance can be interacted with

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.LayoutOrder

Controls the sort order when used with UI layouts

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Position

Relative position of this instance

```json
{
  "Type": "t.UDim2",
  "Unmodified": "gargantuan::UDim2(0.0f, 0, 0.0f, 0)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Rotation

Relative rotation of this instance

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Selectable

Whether this object can participate in focus traversal

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Size

Relative size of this instance

```json
{
  "Type": "t.UDim2",
  "Unmodified": "gargantuan::UDim2(0.0f, 0, 0.0f, 0)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.Visible

Whether this instance should be rendered

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.ZIndex

The order in which this instance renders relative to others

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### GuiObject.CaptureFocus

Requests focus in this object's screen GUI scope

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.CapturePointer

Captures the specified pointer identity to this object

```json
{
  "Params": "{ p(\"pointerId\", t.Integer) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.ReleaseFocus

Releases focus if this object currently owns it

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiObject.ReleasePointer

Releases the specified pointer identity from this object

```json
{
  "Params": "{ p(\"pointerId\", t.Integer) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
