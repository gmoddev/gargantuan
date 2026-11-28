---
title: ScrollingFrame
description: Public creator reference for ScrollingFrame.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A retained clipped viewport whose children are translated by a semantic canvas position

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

## Declared default overrides

Derived defaults override inherited declaration defaults. Expressions below preserve the authoritative declaration.

```text
{ { Property = "Interactable", Value = true }, { Property = "Selectable", Value = true }, { Property = "InputSink", Value = "All" }, }
```

Inherited members from [Frame](./frame.md).

Inherited members from [GuiObject](./guiobject.md).

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### ScrollingFrame.AutomaticCanvasSize

Axes whose canvas extent grows to include arranged children

```json
{
  "Type": "t.EnumItem(\"AutomaticSize\")",
  "Unmodified": "Enums::AutomaticSize::XY",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ScrollingFrame.CanvasPosition

Current logical scroll offset; persisted as intentional game state

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ScrollingFrame.CanvasSize

Authored canvas extent resolved relative to the viewport size

```json
{
  "Type": "t.UDim2",
  "Unmodified": "gargantuan::UDim2(0.0f, 0, 0.0f, 0)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ScrollingFrame.ContentExtent

Committed logical canvas extent

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ScrollingFrame.ScrollBarThickness

Logical thickness of the renderer-neutral scrollbar indicator

```json
{
  "Type": "t.Float",
  "Unmodified": "6.0f",
  "Serializable": true,
  "Replicated": true,
  "Range": "{ minimum = 0, maximum = 64 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ScrollingFrame.ScrollingDirection

Axes on which pointer, wheel, and keyboard scrolling are accepted

```json
{
  "Type": "t.EnumItem(\"ScrollingDirection\")",
  "Unmodified": "Enums::ScrollingDirection::Y",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
