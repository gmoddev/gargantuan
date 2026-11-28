---
title: AttachmentMesh
description: Public creator reference for AttachmentMesh.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Rigid visual-only mesh at an Attachment's semantic transform; it has no physics body.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Attachment](./attachment.md).

Inherited members from [Instance](./instance.md).

## Properties

### AttachmentMesh.CastShadow

Whether this visual casts a shadow.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AttachmentMesh.Color

Linear visual color multiplier.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
  "OverrideRead": "GetColor",
  "OverrideWrite": "SetColor",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "Range": "{ minimum = 0, maximum = 1 }",
  "EditorHint": "ColorComponents"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AttachmentMesh.Material

Optional whole-mesh Material asset override.

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

### AttachmentMesh.Mesh

Strict visual Mesh asset reference.

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

### AttachmentMesh.Size

Positive local mesh scale at the semantic Attachment matrix.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(1.0f)",
  "OverrideRead": "GetSize",
  "OverrideWrite": "SetSize",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AttachmentMesh.Transparency

Visual transparency from zero through one.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetTransparency",
  "OverrideWrite": "SetTransparency",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AttachmentMesh.Visible

Whether this visual is published; variants may retain hidden authored siblings.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
