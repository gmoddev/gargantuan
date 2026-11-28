---
title: InputBinding
description: Public creator reference for InputBinding.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A persistent physical binding with four normalized modifier slots.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### InputBinding.Consume

Consume this physical event even when the context does not consume.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Enabled

Whether this binding may activate its action.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.ExactModifiers

Reject additional held modifier keys.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Modifiers"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.InputType

The mouse input kind for mouse bindings.

```json
{
  "Type": "t.EnumItem(\"UserInputType\")",
  "Unmodified": "Enums::UserInputType::None",
  "OverrideRead": "GetInputType",
  "OverrideWrite": "SetInputType",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.KeyCode

The primary key or supported gamepad button; None is incomplete.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None",
  "OverrideRead": "GetKeyCode",
  "OverrideWrite": "SetKeyCode",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Modifier1

First required left/right modifier, or None.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None",
  "OverrideRead": "GetModifier1",
  "OverrideWrite": "SetModifier1",
  "Serializable": true,
  "Replicated": true,
  "Category": "Modifiers"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Modifier2

Second required left/right modifier, or None.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None",
  "OverrideRead": "GetModifier2",
  "OverrideWrite": "SetModifier2",
  "Serializable": true,
  "Replicated": true,
  "Category": "Modifiers"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Modifier3

Third required left/right modifier, or None.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None",
  "OverrideRead": "GetModifier3",
  "OverrideWrite": "SetModifier3",
  "Serializable": true,
  "Replicated": true,
  "Category": "Modifiers"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Modifier4

Fourth required left/right modifier, or None.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None",
  "OverrideRead": "GetModifier4",
  "OverrideWrite": "SetModifier4",
  "Serializable": true,
  "Replicated": true,
  "Category": "Modifiers"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Priority

Priority within the context.

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Scale

Finite scalar contribution.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input",
  "Range": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.Type

The physical binding kind.

```json
{
  "Type": "t.EnumItem(\"InputBindingType\")",
  "Unmodified": "Enums::InputBindingType::Key",
  "OverrideRead": "GetType",
  "OverrideWrite": "SetType",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputBinding.VectorScale

Finite relative-pointer contribution scale.

```json
{
  "Type": "t.Vector2",
  "Unmodified": "Vector2(1.0f, 1.0f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input",
  "Range": "{}"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
