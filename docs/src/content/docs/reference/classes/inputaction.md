---
title: InputAction
description: Public creator reference for InputAction.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A persistent semantic action whose local runtime state is inspectable.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### InputAction.Enabled

Whether this action may activate.

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

### InputAction.Type

The semantic action value type.

```json
{
  "Type": "t.EnumItem(\"InputActionType\")",
  "Unmodified": "Enums::InputActionType::Bool",
  "OverrideRead": "GetType",
  "OverrideWrite": "SetType",
  "Serializable": true,
  "Replicated": true,
  "Category": "Input"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### InputAction.AddBinding

Creates a canonical binding with up to four modifier keys.

```text
(KeyCode: Enum.KeyCode, Options: { Modifiers: { Enum.KeyCode }?, Enabled: boolean?, ExactModifiers: boolean?, Scale: number?, Priority: number?, Consume: boolean? }?) -> InputBinding
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.GetValue

Returns the local scalar value.

```json
{
  "Returns": "t.Float"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.GetVector

Returns the local two-component value.

```json
{
  "Returns": "t.Vector2"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.IsDown

Returns the local active state.

```json
{
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### InputAction.Canceled

Fired with a bounded reason when activation is canceled.

```json
{
  "Args": "{ t.String }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.Changed

Fired after the current action value changes.

```json
{
  "Args": "{ t.Vector2 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.Pressed

Fired once when the action becomes active.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputAction.Released

Fired once when an active action ends.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
