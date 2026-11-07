const c, t, p = require("@tools/classes").unpack()

return c({
	name = "WorldRoot",
	description = "Abstract class for a physical 3D world.",
	abstract = true,
	since = "0.1.0",

	properties = {
		Gravity = {
			description = "Determines the acceleration due to gravity applied to falling BaseParts.",
			type = t.Float,
			unmodified = "196.2f",
		},
	},

	methods = {
		SphereCast = {
			description = "Sweeps a sphere; Direction magnitude is distance. Initial overlaps are ignored.", asLuau = true,
			luauSignature = "(self: WorldRoot, Origin: Vector3, Radius: number, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart, Position: Vector3, Normal: Vector3, Distance: number }?",
		},
		CapsuleCast = {
			description = "Sweeps a capsule along Direction. Height includes both caps; the local axis is Y.", asLuau = true,
			luauSignature = "(self: WorldRoot, Transform: CFrame, Radius: number, Height: number, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart, Position: Vector3, Normal: Vector3, Distance: number }?",
		},
		BoxCast = {
			description = "Sweeps an oriented box along Direction.", asLuau = true,
			luauSignature = "(self: WorldRoot, Transform: CFrame, Size: Vector3, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart, Position: Vector3, Normal: Vector3, Distance: number }?",
		},
		GetPartsInSphere = {
			description = "Returns collidable overlapping parts in stable identity order with explicit truncation; MaxResults is 1 through 256.", asLuau = true,
			luauSignature = "(self: WorldRoot, Origin: Vector3, Radius: number, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean }",
		},
		GetPartsInCapsule = {
			description = "Returns overlaps for a local-Y capsule; Height includes both caps.", asLuau = true,
			luauSignature = "(self: WorldRoot, Transform: CFrame, Radius: number, Height: number, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean }",
		},
		GetPartsInBox = {
			description = "Returns overlaps for an oriented box, with bounded immutable results.", asLuau = true,
			luauSignature = "(self: WorldRoot, Transform: CFrame, Size: Vector3, Params: RaycastParams?, MaxResults: number?) -> { Parts: {BasePart}, Truncated: boolean }",
		},
		GetDeformableInfo = {
			description = "Returns bounded read-only counts and current simulation status for a descendant deformable body, or nil when unavailable.",
			asLuau = true,
			luauSignature = "(self: WorldRoot, Body: DeformableBody) -> { VertexCount: number, ConstraintCount: number, AttachmentCount: number, TopologyRevision: number, VertexRevision: number, Enabled: boolean, Simulated: boolean }?",
		},
		GetDeformableVertex = {
			description = "Returns one zero-based world-space vertex position from the latest accepted state, or nil when unavailable or out of bounds.",
			asLuau = true,
			luauSignature = "(self: WorldRoot, Body: DeformableBody, VertexIndex: number) -> Vector3?",
		},
		GetSoftBodyStepInfo = {
			description = "Returns bounded read-only counters from the last completed deformable fixed step for this world.",
			asLuau = true,
			luauSignature = "(self: WorldRoot) -> { SimulatedBodies: number, SimulatedVertices: number, JobsDispatched: number, ColliderQueries: number, CandidateColliders: number, FrozenBodies: number, BacklogDrops: number, StaleResults: number }?",
		},
		Raycast = {
			description = "Casts a bounded rigid-physics ray whose Direction magnitude is its maximum distance.",
			asLuau = true,
			luauSignature = "(self: WorldRoot, Origin: Vector3, Direction: Vector3, Params: RaycastParams?) -> { Instance: BasePart, Position: Vector3, Normal: Vector3, Distance: number }?",
		},
		MoveKinematicCapsule = {
			description = "Moves a bounded capsule through real world geometry and returns neutral collision resolution data.",
			asLuau = true,
			luauSignature = "(self: WorldRoot, Position: Vector3, Radius: number, Height: number, Translation: Vector3, Velocity: Vector3) -> { Position: Vector3, AppliedTranslation: Vector3, Velocity: Vector3, ContactNormal: Vector3, FloorNormal: Vector3, Collided: boolean, HasFloor: boolean, PlanesTruncated: boolean }",
		},
		StepPhysics = {
			description = "Advances the physics simulation by the specified delta time. Defaults to simulating all descendant BaseParts.",
			params = { p("deltaTime", t.Double), p("instances", t.Optional(t.Array(t.Instance()))) },
		},
	},
})
