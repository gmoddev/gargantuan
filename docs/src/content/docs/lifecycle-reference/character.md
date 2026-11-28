---
title: Character lifecycle candidate
description: Candidate-only reference for Character.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

A movable character semantic root with one authoritative motion-admission boundary.

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## Character.CFrame

The authoritative world transform accepted by this Character's movement owner.

```json
{
  "Name": "CFrame",
  "Description": "The authoritative world transform accepted by this Character's movement owner.",
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

## Character.Position

The world position component of CFrame.

```json
{
  "Name": "Position",
  "Description": "The world position component of CFrame.",
  "Type": "t.Vector3",
  "Unmodified": "glm::vec3(0.0f, 6.0f, 0.0f)",
  "OverrideRead": "GetPosition",
  "OverrideWrite": "SetPosition",
  "Category": "Transform",
  "Editable": false
}
```

## Character.RootPart

The optional spatial root synchronized to accepted Character movement.

```json
{
  "Name": "RootPart",
  "Description": "The optional spatial root synchronized to accepted Character movement.",
  "Type": "t.Optional(t.Instance(\"BasePart\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetRootPart",
  "OverrideWrite": "SetRootPart",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform"
}
```

## Character.Move

Requests bounded collision-authoritative world movement and returns the accepted result.

```json
{
  "Name": "Move",
  "Description": "Requests bounded collision-authoritative world movement and returns the accepted result.",
  "AsLuau": true,
  "LuauSignature": "(self: Character, translation: Vector3, velocity: Vector3?) -> CharacterMotionResult"
}
```

