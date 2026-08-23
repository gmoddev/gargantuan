const c, t = require("@tools/classes").unpack()

return c({
	name = "SoftBodyAttachment",
	description = "Pins one stable deformable vertex to a world-space position when parented beneath a DeformableBody.",
	since = "0.1.0",

	properties = {
		Enabled = {
			description = "Whether this pin participates in simulation.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Behavior",
		},
		VertexIndex = {
			description = "Zero-based stable topology vertex index pinned by this attachment.",
			type = t.Integer,
			unmodified = "0",
			serializable = true,
			replicated = true,
			category = "Attachment",
			range = { minimum = 0, maximum = 65535 },
		},
		Position = {
			description = "World-space target position for the pinned vertex.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 0.0f, 0.0f)",
			serializable = true,
			replicated = true,
			category = "Attachment",
		},
	},
})
