---
title: Workspace
description: Public creator reference for Workspace.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Provides a physical 3D workspace

Kind `Service`. Abstract `False`. Service policy `Default`.

Access through `game:GetService("Workspace")` subject to the service policy and runtime context.

Inherited members from [WorldRoot](./worldroot.md).

Inherited members from [Instance](./instance.md).

## Properties

### Workspace.CurrentCamera

The current camera.

```json
{
  "Type": "t.Instance(\"Camera\")",
  "Unmodified": "nullptr",
  "WriteLevel": "Never",
  "Category": "Camera",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Workspace.CurrentSpatialRegion

SpatialRegion shown by the main camera. Leave unset for the ordinary Workspace.

```json
{
  "Type": "t.Optional(t.Instance(\"SpatialRegion\"))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetCurrentSpatialRegion",
  "OverrideWrite": "SetCurrentSpatialRegion",
  "Serializable": false,
  "Replicated": false,
  "Category": "Camera"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Workspace.Terrain

The world-owned terrain object.

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
