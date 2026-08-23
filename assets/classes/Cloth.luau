const c, t = require("@tools/classes").unpack()

return c({
	name = "Cloth",
	extends = "DeformableBody",
	description = "Rectangular fixed-topology deformable cloth grid.",
	since = "0.1.0",

	properties = {
		ResolutionX = {
			description = "Horizontal simulation vertex count.",
			type = t.Integer,
			unmodified = "16",
			serializable = true,
			replicated = true,
			category = "Topology",
			range = { minimum = 2, maximum = 256 },
		},
		ResolutionY = {
			description = "Vertical simulation vertex count.",
			type = t.Integer,
			unmodified = "16",
			serializable = true,
			replicated = true,
			category = "Topology",
			range = { minimum = 2, maximum = 256 },
		},
	},
})
