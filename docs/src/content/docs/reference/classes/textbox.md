---
title: TextBox
description: Public creator reference for TextBox.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A bounded UTF-8 editable text control with code-point-safe caret and selection state

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

## Declared default overrides

Derived defaults override inherited declaration defaults. Expressions below preserve the authoritative declaration.

```text
{ { Property = "BackgroundTransparency", Value = 0 }, { Property = "BackgroundColor3", Value = { 0.08, 0.10, 0.14 } }, { Property = "TextXAlignment", Value = "Left" }, { Property = "Interactable", Value = true }, { Property = "Selectable", Value = true }, { Property = "InputSink", Value = "All" }, }
```

Inherited members from [TextLabel](./textlabel.md).

Inherited members from [GuiObject](./guiobject.md).

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### TextBox.CaretPosition

Zero-based Unicode code-point boundary for the runtime caret

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.CompositionText

Bounded transient IME preedit text

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.MaxLength

Maximum editable Unicode code points, capped by the runtime hard limit

```json
{
  "Type": "t.Integer",
  "Unmodified": "4096",
  "Serializable": true,
  "Replicated": true,
  "Range": "{ minimum = 0, maximum = 4096 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.MultiLine

Whether Return inserts a newline instead of submitting

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.PlaceholderColor3

Color used for placeholder text

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(0.55f, 0.55f, 0.55f)",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.PlaceholderText

Text shown when the editable value is empty

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.ReadOnly

Whether editing operations are rejected while selection and copying remain possible

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.SecureTextEntry

Whether presentation and accessibility mask the editable value

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.SelectionLength

Unicode code-point length of the runtime selection

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### TextBox.SelectionStart

Zero-based lower Unicode code-point boundary of the runtime selection

```json
{
  "Type": "t.Integer",
  "Unmodified": "0",
  "WriteLevel": "Never"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

### TextBox.Submitted

Fired when Return submits a focused single-line field

```json
{
  "Args": "{ t.String }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
