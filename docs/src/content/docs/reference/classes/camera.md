---
title: Camera
description: Public creator reference for Camera.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Class that defines the 3D view of a world.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Camera.AspectRatio

Viewport aspect ratio

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetAspectRatio",
  "Category": "Camera"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.CFrame

World transform of this instance

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform",
  "EditorHint": "CFrameComponents"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.CameraType

Type of camera input

```json
{
  "Type": "t.EnumItem(\"CameraType\")",
  "Unmodified": "Enums::CameraType::Freecam",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.DiagonalFieldOfView

Horizontal field of view in degrees

```json
{
  "Type": "t.Float",
  "Unmodified": "70.0f",
  "OverrideRead": "GetDiagonalFieldOfView",
  "OverrideWrite": "SetDiagonalFieldOfView",
  "Category": "Camera",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.FieldOfView

Vertical field of view in degrees

```json
{
  "Type": "t.Float",
  "Unmodified": "70.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Camera",
  "Range": "{ minimum = 1, maximum = 120 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.HorizontalFieldOfView

Horizontal field of view in degrees

```json
{
  "Type": "t.Float",
  "Unmodified": "70.0f",
  "OverrideRead": "GetHorizontalFieldOfView",
  "OverrideWrite": "SetHorizontalFieldOfView",
  "Category": "Camera",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.Pitch

Local pitch of this camera

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Category": "Transform",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.Roll

Local pitch of this camera

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "WriteLevel": "Never",
  "Category": "Transform"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.ViewportSize

Viewport size in pixels

```json
{
  "Type": "t.Vector2",
  "Unmodified": "gargantuan::Vector2()",
  "Category": "Camera",
  "Editable": false,
  "WriteLevel": "Engine"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Camera.Yaw

Local yaw of this camera

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "Category": "Transform",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
