---
title: WorldRoot
description: Public creator reference for WorldRoot.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Abstract class for a physical 3D world.

Kind `Class`. Abstract `True`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### WorldRoot.Gravity

Determines the acceleration due to gravity applied to falling BaseParts.

```json
{
  "Type": "t.Float",
  "Unmodified": "196.2f"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### WorldRoot.BoxCast

Sweeps an oriented box along Direction.

```text
(self: WorldRoot, Transform: CFrame, Size: Vector3, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart | Terrain | BlockGrid, GridHit: { Cell: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } }, Position: Vector3, Normal: Vector3, Revision: string }?, Position: Vector3, Normal: Vector3, Distance: number }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.CapsuleCast

Sweeps a capsule along Direction. Height includes both caps; the local axis is Y.

```text
(self: WorldRoot, Transform: CFrame, Radius: number, Height: number, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart | Terrain | BlockGrid, GridHit: { Cell: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } }, Position: Vector3, Normal: Vector3, Revision: string }?, Position: Vector3, Normal: Vector3, Distance: number }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetDeformableInfo

Returns bounded read-only counts and current simulation status for a descendant deformable body, or nil when unavailable.

```text
(self: WorldRoot, Body: DeformableBody) -> { VertexCount: number, ConstraintCount: number, AttachmentCount: number, TopologyRevision: number, VertexRevision: number, Enabled: boolean, Simulated: boolean }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetDeformableVertex

Returns one zero-based world-space vertex position from the latest accepted state, or nil when unavailable or out of bounds.

```text
(self: WorldRoot, Body: DeformableBody, VertexIndex: number) -> Vector3?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetPartsInBox

Returns overlaps for an oriented box, with bounded immutable results.

```text
(self: WorldRoot, Transform: CFrame, Size: Vector3, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean, Terrain: Terrain?, BlockGrids: {BlockGrid}?, GridCells: {{ Grid: BlockGrid, Cell: Vector3, Revision: string }}? }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetPartsInCapsule

Returns overlaps for a local-Y capsule; Height includes both caps.

```text
(self: WorldRoot, Transform: CFrame, Radius: number, Height: number, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean, Terrain: Terrain?, BlockGrids: {BlockGrid}?, GridCells: {{ Grid: BlockGrid, Cell: Vector3, Revision: string }}? }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetPartsInSphere

Returns collidable overlapping parts in stable identity order with explicit truncation; MaxResults is 1 through 256.

```text
(self: WorldRoot, Origin: Vector3, Radius: number, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean, Terrain: Terrain?, BlockGrids: {BlockGrid}?, GridCells: {{ Grid: BlockGrid, Cell: Vector3, Revision: string }}? }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.GetSoftBodyStepInfo

Returns bounded read-only counters from the last completed deformable fixed step for this world.

```text
(self: WorldRoot) -> { SimulatedBodies: number, SimulatedVertices: number, JobsDispatched: number, ColliderQueries: number, CandidateColliders: number, FrozenBodies: number, BacklogDrops: number, StaleResults: number }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.MoveKinematicCapsule

Moves a bounded capsule through real world geometry and returns neutral collision resolution data.

```text
(self: WorldRoot, Position: Vector3, Radius: number, Height: number, Translation: Vector3, Velocity: Vector3) -> { Position: Vector3, AppliedTranslation: Vector3, Velocity: Vector3, ContactNormal: Vector3, FloorNormal: Vector3, Collided: boolean, HasFloor: boolean, PlanesTruncated: boolean }
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.Raycast

Casts a bounded rigid-physics ray whose Direction magnitude is its maximum distance.

```text
(self: WorldRoot, Origin: Vector3, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart | Terrain | BlockGrid, GridHit: { Cell: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } }, Position: Vector3, Normal: Vector3, Revision: string }?, Position: Vector3, Normal: Vector3, Distance: number }?
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.RequestCollisionArea

Holds an observation of applied collision for this world in an authorized running server Script. Each Script execution owns its handles. Seconds defaults to five.

```text
[=[(self: WorldRoot, Minimum: Vector3, Maximum: Vector3, Seconds: number?, Priority: ("Background" | "Normal" | "Interactive")?) -> ({ GetStatus: (self: unknown) -> { Life: "Active" | "Released" | "Expired" | "Stale", Collision: "Pending" | "Ready" | "Unavailable", Revision: string }, SetArea: (self: unknown, Minimum: Vector3, Maximum: Vector3) -> boolean, SetPriority: (self: unknown, Priority: "Background" | "Normal" | "Interactive") -> boolean, Renew: (self: unknown, Seconds: number) -> boolean, Release: (self: unknown) -> () }?, string?)]=]
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### WorldRoot.SphereCast

Sweeps a sphere; Direction magnitude is distance. Initial overlaps are ignored.

```text
(self: WorldRoot, Origin: Vector3, Radius: number, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart | Terrain | BlockGrid, GridHit: { Cell: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } }, Position: Vector3, Normal: Vector3, Revision: string }?, Position: Vector3, Normal: Vector3, Distance: number }?
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
