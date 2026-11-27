const c = require("@tools/classes").unpack()

return c({
	name = "Terrain",
	description = "The protected volumetric terrain of the Workspace.",
	since = "0.2.0",
	abstract = true,

	methods = {
		GetGridInfo = {
			description = "Returns the sampling configuration and committed field revision of this world.",
			asLuau = true,
			luauSignature = "(self: Terrain) -> { SampleSpacing: number?, Revision: string?, FieldAvailable: boolean, EditingAvailable: boolean, MaximumSampleCoordinate: number, MaximumRegionSamples: number }",
		},
		ReadRegion = {
			description = "Reads a bounded half-open region addressed by integer lattice coordinates. Samples are packed in X, Y, Z order with X varying fastest.",
			asLuau = true,
			luauSignature = "(self: Terrain, Min: Vector3, MaxExclusive: Vector3) -> (buffer, string)",
		},
		FillBall = {
			description = "Admits an asynchronous solid sphere brush and returns a ticket.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Center: Vector3, Radius: number, Material: number) -> string",
		},
		RemoveBall = {
			description = "Admits an asynchronous excavation brush and returns a ticket.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Center: Vector3, Radius: number) -> string",
		},
		PaintBall = {
			description = "Admits an asynchronous material brush and returns a ticket.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Center: Vector3, Radius: number, Material: number) -> string",
		},
		FillBlock = {
			description = "Admits an axis-aligned solid box brush and returns a ticket.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Center: Vector3, Size: Vector3, Material: number) -> string",
		},
		WriteRegion = {
			description = "Admits a bounded lattice region replacement from a packed buffer.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Min: Vector3, MaxExclusive: Vector3, Samples: buffer, ExpectedRevision: string?) -> string",
		},
		ClearRegion = {
			description = "Admits the replacement of a bounded lattice region with air.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Min: Vector3, MaxExclusive: Vector3) -> string",
		},
		SmoothRegion = {
			description = "Admits one bounded signed-field smoothing pass at the given strength.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Min: Vector3, MaxExclusive: Vector3, Strength: number) -> string",
		},
		GetEditStatus = {
			description = "Returns the pending or terminal receipt of a recent edit ticket.",
			asLuau = true,
			luauSignature = "(self: Terrain, Ticket: string) -> { Status: string, Revision: string?, CollisionReady: boolean }?",
		},
		CancelEdit = {
			description = "Requests cancellation of a pending edit ticket before commit.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: Terrain, Ticket: string) -> boolean",
		},
	},
})
