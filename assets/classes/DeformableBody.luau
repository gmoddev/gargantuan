const c, t, p = require("@tools/classes").unpack()

return c({
	name = "DeformableBody",
	description = "Abstract engine-owned deformable body with backend-neutral material and budget controls.",
	abstract = true,
	since = "0.1.0",

	properties = {
		Enabled = {
			description = "Whether this body is eligible for deformable simulation.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Behavior",
		},
		Position = {
			description = "World-space center used when constructing or resetting this body.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 8.0f, 0.0f)",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
		Size = {
			description = "World-space rest dimensions of this body.",
			type = t.Vector3,
			unmodified = "glm::vec3(8.0f, 8.0f, 0.5f)",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
		Color = {
			description = "Base color used by the deformable render mesh.",
			type = t.Color3,
			unmodified = "gargantuan::Color3(0.8f, 0.2f, 0.2f)",
			serializable = true,
			replicated = true,
			category = "Appearance",
		},
		Transparency = {
			description = "Visual transparency of this deformable body.",
			type = t.Float,
			unmodified = "0.0f",
			serializable = true,
			replicated = true,
			category = "Appearance",
			range = { minimum = 0, maximum = 1 },
		},
		Visible = {
			description = "Whether the current deformation state is rendered.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Appearance",
		},
		CastShadow = {
			description = "Whether this deformable mesh participates in shadow rendering.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Appearance",
		},
		Quality = {
			description = "Hard simulation tier used for iteration and active-vertex budgeting.",
			type = t.EnumItem("DeformableQuality"),
			unmodified = "Enums::DeformableQuality::Automatic",
			serializable = true,
			replicated = true,
			category = "Performance",
		},
		CollisionMode = {
			description = "Semantic collision approximation used by the deformable solver.",
			type = t.EnumItem("DeformableCollisionMode"),
			unmodified = "Enums::DeformableCollisionMode::RigidPrimitives",
			serializable = true,
			replicated = true,
			category = "Collision",
		},
		Material = {
			description = "Optional serializable material definition; defaults are used when absent.",
			type = t.Optional(t.Instance("SoftBodyMaterial")),
			unmodified = "std::nullopt",
			serializable = true,
			replicated = true,
			category = "Material",
		},
	},

	methods = {
		ApplyForce = {
			description = "Queues a finite world-space force for the next fixed deformable step.",
			params = { p("force", t.Vector3) },
		},
		ApplyImpulse = {
			description = "Queues a finite world-space impulse for the next fixed deformable step.",
			params = { p("impulse", t.Vector3) },
		},
		ApplyImpulseAtPosition = {
			description = "Queues a finite world-space impulse at a world-space point for the next fixed deformable step.",
			params = { p("impulse", t.Vector3), p("position", t.Vector3) },
		},
	},
})
