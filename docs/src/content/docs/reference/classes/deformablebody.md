---
title: DeformableBody
description: Public creator reference for DeformableBody.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract engine-owned deformable body with backend-neutral material and budget controls.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### DeformableBody.CastShadow

Whether this deformable mesh participates in shadow rendering.

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

### DeformableBody.CollisionMode

Semantic collision approximation used by the deformable solver.

```json
{
  "Type": "t.EnumItem(\"DeformableCollisionMode\")",
  "Unmodified": "Enums::DeformableCollisionMode::RigidPrimitives",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Color

Base color used by the deformable render mesh.

```json
{
  "Type": "t.Color3",
  "Unmodified": "gargantuan::Color3(0.8f, 0.2f, 0.2f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Enabled

Whether this body is eligible for deformable simulation.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Material

Optional serializable material definition; defaults are used when absent.

```json
{
  "Type": "t.Optional(t.Instance(\"SoftBodyMaterial\"))",
  "Unmodified": "std::nullopt",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Position

World-space center used when constructing or resetting this body.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 8.0f, 0.0f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Quality

Hard simulation tier used for iteration and active-vertex budgeting.

```json
{
  "Type": "t.EnumItem(\"DeformableQuality\")",
  "Unmodified": "Enums::DeformableQuality::Automatic",
  "Serializable": true,
  "Replicated": true,
  "Category": "Performance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Size

World-space rest dimensions of this body.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(8.0f, 8.0f, 0.5f)",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Transparency

Visual transparency of this deformable body.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.Visible

Whether the current deformation state is rendered.

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

### DeformableBody.ApplyForce

Queues a finite world-space force for the next fixed deformable step.

```json
{
  "Params": "{ p(\"force\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.ApplyImpulse

Queues a finite world-space impulse for the next fixed deformable step.

```json
{
  "Params": "{ p(\"impulse\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### DeformableBody.ApplyImpulseAtPosition

Queues a finite world-space impulse at a world-space point for the next fixed deformable step.

```json
{
  "Params": "{ p(\"impulse\", t.Vector3), p(\"position\", t.Vector3) }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
