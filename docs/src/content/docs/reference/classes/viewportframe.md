---
title: ViewportFrame
description: Public creator reference for ViewportFrame.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Composes a local 3D presentation scene into a GUI rectangle.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [GuiObject](./guiobject.md).

Inherited members from [GuiBase2d](./guibase2d.md).

Inherited members from [GuiBase](./guibase.md).

Inherited members from [Instance](./instance.md).

## Properties

### ViewportFrame.CurrentCamera

Camera inside this ViewportFrame's presentation subtree.

```json
{
  "Type": "t.Optional(t.Instance(\"Camera\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetCurrentCamera",
  "OverrideWrite": "SetCurrentCamera",
  "Serializable": true,
  "Replicated": true,
  "Category": "Camera"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ViewportFrame.MaxUpdateRate

Maximum presentation scene updates per second when content is changing.

```json
{
  "Type": "t.Float",
  "Unmodified": "30.0f",
  "Range": "{ minimum = 1, maximum = 60 }",
  "Serializable": true,
  "Replicated": true,
  "Category": "Behavior"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ViewportFrame.RenderScale

Requested fraction of the physical GUI size used to render the preview.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Range": "{ minimum = 0.25, maximum = 1 }",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### ViewportFrame.SpatialRegion

Live spatial domain shown by this camera. Unset uses this frame's local presentation children.

```json
{
  "Type": "t.Optional(t.Instance(\"SpatialRegion\", true))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetSpatialRegion",
  "OverrideWrite": "SetSpatialRegion",
  "Serializable": false,
  "Replicated": false,
  "Category": "Camera"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
