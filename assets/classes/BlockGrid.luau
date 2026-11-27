const c, t = require("@tools/classes").unpack()

const function Native(Signature: string, Description: string, Edit: boolean?)
	return {asLuau = true, luauSignature = Signature, description = Description, requiredCapability = if Edit then "MutateDataModel" else "ReadDataModel"}
end
const Rotation = '{ Up: string, Front: string }'
const Block = '{ Definition: string, Orientation: ' .. Rotation .. ' }'
const Definition = '{ Shape: ("Cube" | "Slab" | "Wedge" | "Stairs")?, Color: Color3?, Collision: ("Shape" | "None")?, Queryable: boolean?, Ports: {string}? }'
return c({
	name = "BlockGrid",
	description = "A discrete construction grid with native, automatically partitioned cells.",
	since = "0.2.0",
	properties = {
		SpatialRegion = {type = t.Optional(t.Instance("SpatialRegion", true)), unmodified = "std::nullopt",
			overrideRead = "GetSpatialRegion", overrideWrite = "SetSpatialRegion", serializable = true, replicated = true,
			materializationDependency = "Hard", category = "World", description = "Optional independent spatial domain. Without an assignment the grid uses its enclosing world."},
		CellSize = {type = t.Float, unmodified = "1.0f", overrideRead = "GetCellSize", overrideWrite = "SetCellSize",
			serializable = true, category = "Grid", description = "Uniform world units per cell. Choose 0.5, 1, or 2 while empty."},
		CFrame = {type = t.CFrame, unmodified = "gargantuan::CFrame()", overrideRead = "GetCFrame", overrideWrite = "SetCFrame",
			serializable = true, category = "Transform", editorHint = "CFrameComponents", description = "Rigid grid-local to world transform."},
		Visible = {type = t.Bool, unmodified = "true", overrideRead = "GetVisible", overrideWrite = "SetVisible",
			serializable = true, category = "Appearance", description = "Participate in rendering."},
		CanCollide = {type = t.Bool, unmodified = "true", overrideRead = "GetCanCollide", overrideWrite = "SetCanCollide",
			serializable = true, category = "Collision", description = "Participate in physical collision."},
		CanQuery = {type = t.Bool, unmodified = "true", overrideRead = "GetCanQuery", overrideWrite = "SetCanQuery",
			serializable = true, category = "Collision", description = "Participate in opt-in world queries."},
		BlockRevision = {type = t.String, unmodified = '""', overrideRead = "GetBlockRevision", writable = false,
			editable = false, category = "Grid", description = "Opaque scoped revision of this grid. Read it after a change."},
	},
	methods = {
		GetRevision = Native("(self: BlockGrid) -> string", "Returns an opaque scoped grid revision."),
		SetBlock = Native("(self: BlockGrid, Cell: Vector3, Definition: string, Rotation: " .. Rotation .. "?) -> string", "Places a built-in or registered definition at an integer cell.", true),
		DefineBlock = Native("(self: BlockGrid, Key: string, Spec: " .. Definition .. ") -> string", "Registers a project definition and returns the current grid revision.", true),
		ReplaceDefinition = Native("(self: BlockGrid, Key: string, Spec: " .. Definition .. ", Expected: string) -> string", "Replaces a registered definition at the expected grid revision.", true),
		GetDefinitionDigest = Native("(self: BlockGrid, Key: string) -> string", "Returns the canonical SHA-256 digest of a definition."),
		RemoveBlock = Native("(self: BlockGrid, Cell: Vector3) -> string", "Clears one cell.", true),
		GetBlock = Native("(self: BlockGrid, Cell: Vector3) -> " .. Block .. "?", "Reads an occupied cell, or nil for air."),
		Fill = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Definition: string, Rotation: " .. Rotation .. "?) -> string", "Fills a half-open grid-local box atomically.", true),
		Clear = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> string", "Clears a half-open grid-local box atomically.", true),
		ReadRegion = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> { Revision: string, Cells: {{ Position: Vector3, Block: " .. Block .. " }} }", "Returns occupied cells in Z, Y, X order with X varying fastest."),
		CellToWorld = Native("(self: BlockGrid, Cell: Vector3) -> Vector3", "Returns the world position of a cell center."),
		WorldToCell = Native("(self: BlockGrid, Position: Vector3) -> Vector3", "Returns the local cell containing the world point."),
		Raycast = Native("(self: BlockGrid, Origin: Vector3, Direction: Vector3) -> { Cell: Vector3, Block: " .. Block .. ", Position: Vector3, Normal: Vector3, Revision: string }?", "Returns the closest exact current cell hit on the world segment."),
	},
})
