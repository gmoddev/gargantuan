const c, t = require("@tools/classes").unpack()

return c({
	name = "KinematicCharacter",
	extends = "Folder",
	description = "Runtime character state driven by a Luau kinematic controller.",
	since = "0.1.0",

	properties = {
		RootPart = {
			description = "Optional visible root assembled for this character.",
			type = t.Optional(t.Instance("BasePart")),
			unmodified = "std::nullopt",
		},
		Position = {
			description = "World-space center of the character capsule.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 6.0f, 0.0f)",
		},
		Velocity = {
			description = "Current controller-owned world-space velocity.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 0.0f, 0.0f)",
		},
		Grounded = {
			description = "Whether controller policy accepted the latest floor contact as walkable.",
			type = t.Bool,
			unmodified = "false",
		},
		FloorNormal = {
			description = "Latest upward-facing floor normal returned by world collision.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 1.0f, 0.0f)",
		},
		CapsuleRadius = {
			description = "Radius used by the default kinematic query policy.",
			type = t.Float,
			unmodified = "1.0f",
		},
		CapsuleHeight = {
			description = "Total capsule height used by the default kinematic query policy.",
			type = t.Float,
			unmodified = "4.0f",
		},
	},
})
