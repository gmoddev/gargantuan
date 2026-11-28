---
title: KinematicCharacter
description: Public creator reference for KinematicCharacter.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Character specialization using the bounded native capsule movement primitive.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Character](./character.md).

Inherited members from [Folder](./folder.md).

Inherited members from [Instance](./instance.md).

## Properties

### KinematicCharacter.CapsuleHeight

Total capsule height used by the default kinematic query policy.

```json
{
  "Type": "t.Float",
  "Unmodified": "4.0f"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### KinematicCharacter.CapsuleRadius

Radius used by the default kinematic query policy.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### KinematicCharacter.FloorNormal

Latest upward-facing floor normal returned by world collision.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 1.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### KinematicCharacter.Grounded

Whether controller policy accepted the latest floor contact as walkable.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### KinematicCharacter.Velocity

Current controller-owned world-space velocity.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 0.0f, 0.0f)"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
