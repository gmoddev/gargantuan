---
title: Constraint
description: Public creator reference for Constraint.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for constraint-based instances.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Constraint.Active

Whether if the constraint and both of its parts are in a WorldRoot and the Constraint.Enabled property is true.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "OverrideRead": "GetActive"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Constraint.Attachment0

First attachment of this constraint.

```json
{
  "Type": "t.Optional(t.Instance(\"Attachment\"))",
  "Unmodified": "std::nullopt",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Constraint.Attachment1

Second attachment of this constraint.

```json
{
  "Type": "t.Optional(t.Instance(\"Attachment\"))",
  "Unmodified": "std::nullopt",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Constraint.Enabled

Whether this constraint is enabled.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
