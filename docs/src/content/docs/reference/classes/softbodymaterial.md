---
title: SoftBodyMaterial
description: Public creator reference for SoftBodyMaterial.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Serializable material controls shared by deformable bodies.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### SoftBodyMaterial.BendCompliance

XPBD bend compliance used by cloth.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0001f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.Damping

Fraction of velocity removed during each fixed simulation step.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.04f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.Friction

Fraction of tangential motion removed after rigid-primitive contact.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.2f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.ParticleMass

Mass assigned to each simulation vertex.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.1f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0.0001, maximum = 1000 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.ShapeCompliance

Rotation-aware rest-shape compliance used by rubber bodies.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.00001f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.StretchCompliance

XPBD stretch compliance; zero is maximally stiff.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.000001f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.Thickness

Collision thickness around each simulation vertex.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.05f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision",
  "Range": "{ minimum = 0, maximum = 10 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### SoftBodyMaterial.VolumeCompliance

XPBD local-volume compliance used by rubber bodies; zero is maximally preserving.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.00001f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Material",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
