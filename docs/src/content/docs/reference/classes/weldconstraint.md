---
title: WeldConstraint
description: Public creator reference for WeldConstraint.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

	Connects two BaseParts together such that their relative position and orientation remain the same.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Constraint](./constraint.md).

Inherited members from [Instance](./instance.md).

## Properties

### WeldConstraint.Part0

First part of this constraint.

```json
{
  "Type": "t.Optional(t.Instance(\"BasePart\"))",
  "Unmodified": "std::nullopt",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WeldConstraint.Part1

Second part of this constraint.

```json
{
  "Type": "t.Optional(t.Instance(\"BasePart\"))",
  "Unmodified": "std::nullopt",
  "Serializable": true,
  "Replicated": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
