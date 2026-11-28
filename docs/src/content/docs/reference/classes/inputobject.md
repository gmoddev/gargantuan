---
title: InputObject
description: Public creator reference for InputObject.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Class that describes a user input at an instant.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### InputObject.Delta

Describes the delta between input movements.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputObject.KeyCode

Describes the kind of input used.

```json
{
  "Type": "t.EnumItem(\"KeyCode\")",
  "Unmodified": "Enums::KeyCode::None"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputObject.Position

Describes the positional value of this input.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputObject.UserInputState

Describes the state of an input being performed.

```json
{
  "Type": "t.EnumItem(\"UserInputState\")",
  "Unmodified": "Enums::UserInputState::None"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### InputObject.UserInputType

Describes the kind of input being performed.

```json
{
  "Type": "t.EnumItem(\"UserInputType\")",
  "Unmodified": "Enums::UserInputType::None"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### InputObject.IsModifierKeyDown

Whether the specified modifier key is being pressed.

```json
{
  "Params": "{ p(\"modifierKey\", t.EnumItem(\"ModifierKey\")) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
