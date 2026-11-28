---
title: Cloth
description: Public creator reference for Cloth.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Rectangular fixed-topology deformable cloth grid.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [DeformableBody](./deformablebody.md).

Inherited members from [Instance](./instance.md).

## Properties

### Cloth.ResolutionX

Horizontal simulation vertex count.

```json
{
  "Type": "t.Integer",
  "Unmodified": "16",
  "Serializable": true,
  "Replicated": true,
  "Category": "Topology",
  "Range": "{ minimum = 2, maximum = 256 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Cloth.ResolutionY

Vertical simulation vertex count.

```json
{
  "Type": "t.Integer",
  "Unmodified": "16",
  "Serializable": true,
  "Replicated": true,
  "Category": "Topology",
  "Range": "{ minimum = 2, maximum = 256 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
