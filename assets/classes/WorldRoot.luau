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
