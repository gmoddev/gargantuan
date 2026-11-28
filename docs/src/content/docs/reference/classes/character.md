---
title: Character
description: Public creator reference for Character.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A movable character semantic root with one authoritative motion-admission boundary.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Folder](./folder.md).

Inherited members from [Instance](./instance.md).

## Properties

### Character.CFrame

The authoritative world transform accepted by this Character's movement owner.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame(0.0f, 6.0f, 0.0f)",
  "OverrideRead": "GetCFrame",
  "OverrideWrite": "SetCFrame",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform",
  "EditorHint": "CFrameComponents"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Character.Position

The world position component of CFrame.

```json
{
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 6.0f, 0.0f)",
  "OverrideRead": "GetPosition",
  "OverrideWrite": "SetPosition",
  "Category": "Transform",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Character.RootPart

The optional spatial root synchronized to accepted Character movement.

```json
{
  "Type": "t.Optional(t.Instance(\"BasePart\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetRootPart",
  "OverrideWrite": "SetRootPart",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### Character.GetMotionWorld

Returns the live collision world of this Character's spatial root for read-only motion previews, or nil when unavailable.

```text
(self: Character) -> WorldRoot?
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Character.Move

Requests bounded collision-authoritative world movement and returns the accepted result.

```text
(self: Character, translation: Vector3, velocity: Vector3?) -> CharacterMotionResult
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
