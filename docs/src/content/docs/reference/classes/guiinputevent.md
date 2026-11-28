---
title: GuiInputEvent
description: Public creator reference for GuiInputEvent.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Runtime-only pointer routing data with explicit propagation consumption

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### GuiInputEvent.Button

Pointer button semantic

```json
{
  "Type": "t.EnumItem(\"GuiPointerButton\")",
  "Unmodified": "Enums::GuiPointerButton::Primary",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.CurrentTarget

Object currently receiving this route phase

```json
{
  "Type": "t.Instance()",
  "Unmodified": "std::shared_ptr<gargantuan::Instance>{}",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.Handled

Whether propagation was consumed

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.Phase

Current propagation phase

```json
{
  "Type": "t.EnumItem(\"GuiEventPhase\")",
  "Unmodified": "Enums::GuiEventPhase::Target",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.PointerId

Stable pointer contact identity for the event

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.PointerType

Kind of pointer producing the event

```json
{
  "Type": "t.EnumItem(\"GuiPointerType\")",
  "Unmodified": "Enums::GuiPointerType::Mouse",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.Position

Logical viewport position

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2(0.0f, 0.0f)",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### GuiInputEvent.Target

Original routed target

```json
{
  "Type": "t.Instance()",
  "Unmodified": "std::shared_ptr<gargantuan::Instance>{}",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### GuiInputEvent.Consume

Consumes this route and prevents later propagation phases

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
