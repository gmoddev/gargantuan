---
title: ActionMap
description: Public creator reference for ActionMap.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Maps bounded physical input bindings to semantic gameplay actions.

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("ActionMap")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### ActionMap.BindKey

Adds a keyboard binding and returns its runtime binding identity.

```json
{
  "Params": "{ p(\"ActionName\", t.String), p(\"KeyCode\", t.EnumItem(\"KeyCode\")), p(\"Scale\", t.Float), p(\"Priority\", t.Integer), p(\"Consume\", t.Bool), }",
  "Returns": "t.Integer"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.BindMouseButton

Adds a mouse-button binding and returns its runtime binding identity.

```json
{
  "Params": "{ p(\"ActionName\", t.String), p(\"InputType\", t.EnumItem(\"UserInputType\")), p(\"Scale\", t.Float), p(\"Priority\", t.Integer), p(\"Consume\", t.Bool), }",
  "Returns": "t.Integer"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.BindPointerDelta

Adds a relative-pointer delta binding and returns its runtime binding identity.

```json
{
  "Params": "{ p(\"ActionName\", t.String), p(\"Scale\", t.Vector2), p(\"Priority\", t.Integer), p(\"Consume\", t.Bool), }",
  "Returns": "t.Integer"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.GetBindingCount

Returns the current bounded binding count.

```json
{
  "Returns": "t.Integer"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.GetValue

Returns the clamped scalar value of a digital action.

```json
{
  "Params": "{ p(\"ActionName\", t.String) }",
  "Returns": "t.Float"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.GetVector

Returns the frame-accumulated vector value of an action.

```json
{
  "Params": "{ p(\"ActionName\", t.String) }",
  "Returns": "t.Vector2"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.IsDown

Returns whether any digital binding currently activates the action.

```json
{
  "Params": "{ p(\"ActionName\", t.String) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.Unbind

Removes one binding by runtime identity.

```json
{
  "Params": "{ p(\"BindingId\", t.Integer) }",
  "Returns": "t.Bool"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.UnbindAction

Removes every binding for one semantic action.

```json
{
  "Params": "{ p(\"ActionName\", t.String) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### ActionMap.ActionBegan

Fired when a digital semantic action becomes active.

```json
{
  "Args": "{ t.String }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.ActionChanged

Fired when the current scalar or vector value of an action changes.

```json
{
  "Args": "{ t.String, t.Vector2 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ActionMap.ActionEnded

Fired when a digital semantic action becomes inactive.

```json
{
  "Args": "{ t.String }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
