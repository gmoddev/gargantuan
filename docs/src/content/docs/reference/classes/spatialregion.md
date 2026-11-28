---
title: SpatialRegion
description: Public creator reference for SpatialRegion.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

An independent spatial domain. Positions, physics, and inherited world queries are local to this domain.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [WorldRoot](./worldroot.md).

Inherited members from [Instance](./instance.md).

## Properties

### SpatialRegion.Terrain

The terrain field owned by this spatial domain.

```json
{
  "Type": "t.Instance(\"Terrain\")",
  "Unmodified": "nullptr",
  "OverrideRead": "GetTerrain",
  "Writable": false,
  "WriteLevel": "Never",
  "Category": "World",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
