---
title: MeshPart
description: Public creator reference for MeshPart.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A BasePart rendered from a canonical AssetService mesh

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [BasePart](./basepart.md).

Inherited members from [Instance](./instance.md).

## Properties

### MeshPart.CollisionMode

Box collision or anchored static triangle collision from the Mesh asset. Unanchored parts use Box collision.

```json
{
  "Type": "t.EnumItem(\"MeshCollisionMode\")",
  "Unmodified": "Enums::MeshCollisionMode::Box",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### MeshPart.Material

Optional whole-mesh Material asset override

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetMaterial",
  "OverrideWrite": "SetMaterial",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "EditorHint": "AssetReference:Material"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### MeshPart.Mesh

Strict Mesh asset reference

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetMesh",
  "OverrideWrite": "SetMesh",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "EditorHint": "AssetReference:Mesh"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
