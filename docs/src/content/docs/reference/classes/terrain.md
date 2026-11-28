---
title: Terrain
description: Public creator reference for Terrain.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

The protected volumetric terrain of the Workspace.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### Terrain.CancelEdit

Requests cancellation of a pending edit ticket before commit.

```text
(self: Terrain, Ticket: string) -> boolean
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.ClearRegion

Admits the replacement of a bounded lattice region with air.

```text
(self: Terrain, Min: Vector3, MaxExclusive: Vector3) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.FillBall

Admits an asynchronous solid sphere brush and returns a ticket.

```text
(self: Terrain, Center: Vector3, Radius: number, Material: number) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.FillBlock

Admits an axis-aligned solid box brush and returns a ticket.

```text
(self: Terrain, Center: Vector3, Size: Vector3, Material: number) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.GetEditStatus

Returns the pending or terminal receipt of a recent edit ticket.

```text
(self: Terrain, Ticket: string) -> { Status: string, Revision: string?, CollisionReady: boolean }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.GetGridInfo

Returns the sampling configuration and committed field revision of this world.

```text
(self: Terrain) -> { SampleSpacing: number?, Revision: string?, FieldAvailable: boolean, EditingAvailable: boolean, MaximumSampleCoordinate: number, MaximumRegionSamples: number }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.PaintBall

Admits an asynchronous material brush and returns a ticket.

```text
(self: Terrain, Center: Vector3, Radius: number, Material: number) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.ReadRegion

Reads a bounded half-open region addressed by integer lattice coordinates. Samples are packed in X, Y, Z order with X varying fastest.

```text
(self: Terrain, Min: Vector3, MaxExclusive: Vector3) -> (buffer, string)
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.RemoveBall

Admits an asynchronous excavation brush and returns a ticket.

```text
(self: Terrain, Center: Vector3, Radius: number) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.SmoothRegion

Admits one bounded signed-field smoothing pass at the given strength.

```text
(self: Terrain, Min: Vector3, MaxExclusive: Vector3, Strength: number) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Terrain.WriteRegion

Admits a bounded lattice region replacement from a packed buffer.

```text
(self: Terrain, Min: Vector3, MaxExclusive: Vector3, Samples: buffer, ExpectedRevision: string?) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
