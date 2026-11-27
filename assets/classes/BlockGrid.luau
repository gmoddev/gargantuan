const c, t = require("@tools/classes").unpack()

const function Native(Signature: string, Description: string, Edit: boolean?)
	return {asLuau = true, luauSignature = Signature, description = Description, requiredCapability = if Edit then "MutateDataModel" else "ReadDataModel"}
end
const Rotation = '{ Up: string, Front: string }'
const Block = '{ Definition: string, Orientation: ' .. Rotation .. ' }'
const Definition = '{ Shape: ("Cube" | "Slab" | "Wedge" | "Stairs" | "Mesh")?, Color: Color3?, Material: string?, Mesh: string?, Collision: ("Shape" | "None" | "Asset")?, CollisionAsset: string?, Queryable: boolean?, Ports: {string}? }'
const Bounds = '{ Min: Vector3, MaxExclusive: Vector3 }'
const Filter = '{ Definitions: {string}?, Where: { Channel: string, Equals: boolean | number }? }'
const Packed = '{ Size: Vector3, Bindings: {[string]: string}, Palette: {' .. Block .. '}, Cells: buffer }'
const Edit = '{ SetBlock: (unknown, Vector3, string, ' .. Rotation .. '?) -> (), RemoveBlock: (unknown, Vector3) -> (), Fill: (unknown, Vector3, Vector3, string, ' .. Rotation .. '?) -> (), Clear: (unknown, Vector3, Vector3) -> (), SetValue: (unknown, Vector3, string, boolean | number) -> (), CopyRegion: (unknown, Vector3, Vector3, Vector3) -> (), MoveRegion: (unknown, Vector3, Vector3, Vector3) -> (), WritePacked: (unknown, Vector3, Vector3, ' .. Packed .. ') -> (), Commit: (unknown) -> string, Cancel: (unknown) -> () }'
const Cursor = '{ Step: (unknown, number) -> { Status: "More" | "Complete" | "Found" | "Limit" | "Unavailable" | "Expired" | "Cancelled", Cells: {Vector3}, Revision: string, Match: Vector3? }, Cancel: (unknown) -> () }'
return c({
	name = "BlockGrid",
	description = "A discrete construction grid with native, automatically partitioned cells.",
	since = "0.2.0",
	properties = {
		SpatialRegion = {type = t.Optional(t.Instance("SpatialRegion", true)), unmodified = "std::nullopt",
			overrideRead = "GetSpatialRegion", overrideWrite = "SetSpatialRegion", serializable = true, replicated = true,
			materializationDependency = "Hard", category = "World", description = "Optional independent spatial domain. Without an assignment the grid uses its enclosing world."},
		CellSize = {type = t.Float, unmodified = "1.0f", overrideRead = "GetCellSize", overrideWrite = "SetCellSize",
			serializable = true, replicated = true, category = "Grid", description = "Uniform world units per cell. Choose 0.5, 1, or 2 while empty."},
		CFrame = {type = t.CFrame, unmodified = "gargantuan::CFrame()", overrideRead = "GetCFrame", overrideWrite = "SetCFrame",
			serializable = true, replicated = true, category = "Transform", editorHint = "CFrameComponents", description = "Rigid grid-local to world transform."},
		Visible = {type = t.Bool, unmodified = "true", overrideRead = "GetVisible", overrideWrite = "SetVisible",
			serializable = true, replicated = true, category = "Appearance", description = "Participate in rendering."},
		CanCollide = {type = t.Bool, unmodified = "true", overrideRead = "GetCanCollide", overrideWrite = "SetCanCollide",
			serializable = true, replicated = true, category = "Collision", description = "Participate in physical collision."},
		CanQuery = {type = t.Bool, unmodified = "true", overrideRead = "GetCanQuery", overrideWrite = "SetCanQuery",
			serializable = true, replicated = true, category = "Collision", description = "Participate in opt-in world queries."},
		BlockRevision = {type = t.String, unmodified = '""', overrideRead = "GetBlockRevision", writable = false,
			editable = false, category = "Grid", description = "Opaque scoped revision of this grid. Read it after a change."},
		MotionMode = {type = t.String, unmodified = '"Static"', overrideRead = "GetMotionMode", writable = false,
			editable = false, category = "Physics", description = "Current authoritative Static or Dynamic simulation mode."},
	},
	methods = {
		GetRevision = Native("(self: BlockGrid) -> string", "Returns an opaque scoped grid revision."),
		ExtractRegion = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Expected: string) -> BlockGrid", "Transfers up to 4096 cells into a new static sibling and preserves world positions and channel values.", true),
		SplitRegion = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Expected: string) -> BlockGrid", "Splits a moving convex grid into two nonempty moving siblings with inherited rigid-body velocities.", true),
		MergeFrom = Native("(self: BlockGrid, Source: BlockGrid, Expected: string, ExpectedSource: string) -> string", "Moves an aligned sibling's occupied cells, preserving grid identity and conserving momentum when both are dynamic.", true),
		SetMotionMode = Native('(self: BlockGrid, Mode: "Static" | "Dynamic", Expected: string) -> string', "Switches between static construction and a bounded dynamic convex assembly.", true),
		SetAssemblyVelocity = Native("(self: BlockGrid, Linear: Vector3, Angular: Vector3, Expected: string) -> string", "Sets world-space linear and angular velocity on a dynamic construction.", true),
		GetAssemblyState = Native('(self: BlockGrid) -> { Mode: "Static" | "Dynamic", CFrame: CFrame, LinearVelocity: Vector3, AngularVelocity: Vector3, Mass: number, CenterOfMass: Vector3, LocalCenterOfMass: Vector3, LocalInertia: {Vector3} }', "Reads current native mass, center, inertia columns, pose and velocity."),
		SetBlock = Native("(self: BlockGrid, Cell: Vector3, Definition: string, Rotation: " .. Rotation .. "?) -> string", "Places a built-in or registered definition at an integer cell.", true),
		DefineBlock = Native("(self: BlockGrid, Key: string, Spec: " .. Definition .. ") -> string", "Registers a project definition and returns the current grid revision.", true),
		ReplaceDefinition = Native("(self: BlockGrid, Key: string, Spec: " .. Definition .. ", Expected: string) -> string", "Replaces a registered definition at the expected grid revision.", true),
		GetDefinitionDigest = Native("(self: BlockGrid, Key: string) -> string", "Returns the canonical SHA-256 digest of a definition."),
		RemoveBlock = Native("(self: BlockGrid, Cell: Vector3) -> string", "Clears one cell.", true),
		GetBlock = Native("(self: BlockGrid, Cell: Vector3) -> " .. Block .. "?", "Reads an occupied cell, or nil for air."),
		Fill = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Definition: string, Rotation: " .. Rotation .. "?) -> string", "Fills a half-open grid-local box atomically.", true),
		Clear = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> string", "Clears a half-open grid-local box atomically.", true),
		ReadRegion = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3, Selector: " .. Filter .. "?) -> { Revision: string, Cells: {{ Position: Vector3, Block: " .. Block .. " }} }", "Returns matching occupied cells in Z, Y, X order with X varying fastest."),
		DefineChannel = Native('(self: BlockGrid, Name: string, Spec: { Kind: "Bool" | "UInt8" | "Int32" | "Float32", Default: boolean | number, Persistent: boolean?, Audience: ("Server" | "RelevantPeers")?, Indexed: boolean?, OnCopy: ("Default" | "Value")? }) -> string', "Declares a typed channel with an implicit shared default.", true),
		GetValue = Native("(self: BlockGrid, Cell: Vector3, Channel: string) -> (boolean | number)", "Reads a typed value from an occupied cell."),
		BeginEdit = Native("(self: BlockGrid, Expected: string?) -> " .. Edit, "Starts a bounded edit guarded by the current or supplied revision.", true),
		ReadPacked = Native("(self: BlockGrid, Min: Vector3, MaxExclusive: Vector3) -> (" .. Packed .. " & { Revision: string })", "Reads a bounded X-fastest uint16 palette buffer with pinned definition digests."),
		FindConnected = Native('(self: BlockGrid, Starts: {Vector3}, Options: {Bounds: ' .. Bounds .. ', Filter: ' .. Filter .. '?, Adjacency: ("Face" | "Edge" | "Corner")?, UsePorts: boolean?, StopAt: {string}?, MaxVisited: number?}) -> ' .. Cursor, "Starts incremental native traversal of a pinned grid view."),
		FindActive = Native("(self: BlockGrid, Channel: string, Bounds: " .. Bounds .. ") -> " .. Cursor, "Traverses nondefault members of an indexed channel."),
		CellToWorld = Native("(self: BlockGrid, Cell: Vector3) -> Vector3", "Returns the world position of a cell center."),
		WorldToCell = Native("(self: BlockGrid, Position: Vector3) -> Vector3", "Returns the local cell containing the world point."),
		Raycast = Native("(self: BlockGrid, Origin: Vector3, Direction: Vector3) -> { Cell: Vector3, Block: " .. Block .. ", Position: Vector3, Normal: Vector3, Revision: string }?", "Returns the closest exact current cell hit on the world segment."),
	},
})
