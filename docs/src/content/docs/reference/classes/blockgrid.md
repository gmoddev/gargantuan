---
title: BlockGrid
description: Public creator reference for BlockGrid.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

A discrete construction grid with native, automatically partitioned cells.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### BlockGrid.BlockRevision

Opaque scoped revision of this grid. Read it after a change.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetBlockRevision",
  "Writable": false,
  "Editable": false,
  "Category": "Grid"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.CFrame

Rigid grid-local to world transform.

```json
{
  "Type": "t.CFrame",
  "Unmodified": "gargantuan::CFrame()",
  "OverrideRead": "GetCFrame",
  "OverrideWrite": "SetCFrame",
  "Serializable": true,
  "Replicated": true,
  "Category": "Transform",
  "EditorHint": "CFrameComponents"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.CanCollide

Participate in physical collision.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "OverrideRead": "GetCanCollide",
  "OverrideWrite": "SetCanCollide",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.CanQuery

Participate in opt-in world queries.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "OverrideRead": "GetCanQuery",
  "OverrideWrite": "SetCanQuery",
  "Serializable": true,
  "Replicated": true,
  "Category": "Collision"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.CellSize

Uniform world units per cell. Choose 0.5, 1, or 2 while empty.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "OverrideRead": "GetCellSize",
  "OverrideWrite": "SetCellSize",
  "Serializable": true,
  "Replicated": true,
  "Category": "Grid"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.MotionMode

Current authoritative Static or Dynamic simulation mode.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"Static\"'",
  "OverrideRead": "GetMotionMode",
  "Writable": false,
  "Editable": false,
  "Category": "Physics"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.SpatialRegion

Optional independent spatial domain. Without an assignment the grid uses its enclosing world.

```json
{
  "Type": "t.Optional(t.Instance(\"SpatialRegion\", true))",
  "Unmodified": "std::nullopt",
  "OverrideRead": "GetSpatialRegion",
  "OverrideWrite": "SetSpatialRegion",
  "Serializable": true,
  "Replicated": true,
  "MaterializationDependency": "Hard",
  "Category": "World"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.Visible

Participate in rendering.

```json
{
  "Type": "t.Bool",
  "Unmodified": "true",
  "OverrideRead": "GetVisible",
  "OverrideWrite": "SetVisible",
  "Serializable": true,
  "Replicated": true,
  "Category": "Appearance"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### BlockGrid.BeginEdit

Starts a bounded edit guarded by the current or supplied revision.

```text
(self: BlockGrid, Expected: string?) -> { SetBlock: (unknown, Vector3, string, { Up: string, Front: string }?) -> (), RemoveBlock: (unknown, Vector3) -> (), Fill: (unknown, Vector3, Vector3, string, { Up: string, Front: string }?) -> (), Clear: (unknown, Vector3, Vector3) -> (), SetValue: (unknown, Vector3, string, boolean | number) -> (), CopyRegion: (unknown, Vector3, Vector3, Vector3) -> (), MoveRegion: (unknown, Vector3, Vector3, Vector3) -> (), WritePacked: (unknown, Vector3, Vector3, { Size: Vector3, Bindings: {[string]: string}, Palette: {{ Definition: string, Orientation: { Up: string, Front: string } }}, Cells: buffer }) -> (), Commit: (unknown) -> string, Cancel: (unknown) -> () }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.CellToWorld

Returns the world position of a cell center.

```text
(self: BlockGrid, Cell: Vector3) -> Vector3
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.Clear

Clears a half-open grid-local box atomically.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.DefineBlock

Registers a project definition and returns the current grid revision.

```text
(self: BlockGrid, Key: string, Spec: { Shape: ("Cube" | "Slab" | "Wedge" | "Stairs" | "Mesh")?, Color: Color3?, Material: string?, Mesh: string?, Collision: ("Shape" | "None" | "Asset")?, CollisionAsset: string?, Queryable: boolean?, Ports: {string}? }) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.DefineChannel

Declares a typed channel with an implicit shared default.

```text
(self: BlockGrid, Name: string, Spec: { Kind: "Bool" | "UInt8" | "Int32" | "Float32", Default: boolean | number, Persistent: boolean?, Audience: ("Server" | "RelevantPeers")?, Indexed: boolean?, OnCopy: ("Default" | "Value")? }) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.ExtractRegion

Transfers up to 4096 cells into a new static sibling and preserves world positions and channel values.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Expected: string) -> BlockGrid
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.Fill

Fills a half-open grid-local box atomically.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Definition: string, Rotation: { Up: string, Front: string }?) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.FindActive

Traverses nondefault members of an indexed channel.

```text
(self: BlockGrid, Channel: string, Bounds: { Min: Vector3, MaxExclusive: Vector3 }) -> { Step: (unknown, number) -> { Status: "More" | "Complete" | "Found" | "Limit" | "Unavailable" | "Expired" | "Cancelled", Cells: {Vector3}, Revision: string, Match: Vector3? }, Cancel: (unknown) -> () }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.FindConnected

Starts incremental native traversal of a pinned grid view.

```text
(self: BlockGrid, Starts: {Vector3}, Options: {Bounds: { Min: Vector3, MaxExclusive: Vector3 }, Filter: { Definitions: {string}?, Where: { Channel: string, Equals: boolean | number }? }?, Adjacency: ("Face" | "Edge" | "Corner")?, UsePorts: boolean?, StopAt: {string}?, MaxVisited: number?}) -> { Step: (unknown, number) -> { Status: "More" | "Complete" | "Found" | "Limit" | "Unavailable" | "Expired" | "Cancelled", Cells: {Vector3}, Revision: string, Match: Vector3? }, Cancel: (unknown) -> () }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.GetAssemblyState

Reads current native mass, center, inertia columns, pose and velocity.

```text
(self: BlockGrid) -> { Mode: "Static" | "Dynamic", CFrame: CFrame, LinearVelocity: Vector3, AngularVelocity: Vector3, Mass: number, CenterOfMass: Vector3, LocalCenterOfMass: Vector3, LocalInertia: {Vector3} }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.GetBlock

Returns a block or nil for verified air. Unavailable replica cells raise an error.

```text
(self: BlockGrid, Cell: Vector3) -> { Definition: string, Orientation: { Up: string, Front: string } }?
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.GetDefinitionDigest

Returns the canonical SHA-256 digest of a definition.

```text
(self: BlockGrid, Key: string) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.GetRevision

Returns an opaque scoped grid revision.

```text
(self: BlockGrid) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.GetValue

Reads a typed value from an occupied cell.

```text
(self: BlockGrid, Cell: Vector3, Channel: string) -> (boolean | number)
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.IsRegionAvailable

Reports whether every cell in a half-open local region has installed content, including verified air.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> boolean
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.MergeFrom

Moves an aligned sibling's occupied cells, preserving grid identity and conserving momentum when both are dynamic.

```text
(self: BlockGrid, Source: BlockGrid, Expected: string, ExpectedSource: string) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.Raycast

Returns the closest exact current cell hit on the world segment.

```text
(self: BlockGrid, Origin: Vector3, Direction: Vector3) -> { Cell: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } }, Position: Vector3, Normal: Vector3, Revision: string }?
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.ReadPacked

Reads a bounded X-fastest uint16 geometry buffer with pinned definition digests. The buffer excludes channel overrides.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> ({ Size: Vector3, Bindings: {[string]: string}, Palette: {{ Definition: string, Orientation: { Up: string, Front: string } }}, Cells: buffer } & { Revision: string })
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.ReadRegion

Returns matching occupied cells in Z, Y, X order with X varying fastest.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Selector: { Definitions: {string}?, Where: { Channel: string, Equals: boolean | number }? }?) -> { Revision: string, Cells: {{ Position: Vector3, Block: { Definition: string, Orientation: { Up: string, Front: string } } }} }
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.RemoveBlock

Clears one cell.

```text
(self: BlockGrid, Cell: Vector3) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.ReplaceDefinition

Replaces a registered definition at the expected grid revision.

```text
(self: BlockGrid, Key: string, Spec: { Shape: ("Cube" | "Slab" | "Wedge" | "Stairs" | "Mesh")?, Color: Color3?, Material: string?, Mesh: string?, Collision: ("Shape" | "None" | "Asset")?, CollisionAsset: string?, Queryable: boolean?, Ports: {string}? }, Expected: string) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.SetAssemblyVelocity

Sets world-space linear and angular velocity on a dynamic construction.

```text
(self: BlockGrid, Linear: Vector3, Angular: Vector3, Expected: string) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.SetBlock

Places a built-in or registered definition at an integer cell.

```text
(self: BlockGrid, Cell: Vector3, Definition: string, Rotation: { Up: string, Front: string }?) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.SetMotionMode

Switches between static construction and a bounded dynamic convex assembly.

```text
(self: BlockGrid, Mode: "Static" | "Dynamic", Expected: string) -> string
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.SplitRegion

Splits a moving convex grid into two nonempty moving siblings with inherited rigid-body velocities.

```text
(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Expected: string) -> BlockGrid
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### BlockGrid.WorldToCell

Returns the local cell containing the world point.

```text
(self: BlockGrid, Position: Vector3) -> Vector3
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "ReadDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
