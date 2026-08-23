const c, t = require("@tools/classes").unpack()

return c({
	name = "RubberBody",
	extends = "DeformableBody",
	description = "Fixed-topology elastic lattice that recovers its translated rest shape.",
	since = "0.1.0",

	properties = {
		ResolutionX = {
			description = "Simulation vertex count along the X axis.",
			type = t.Integer,
			unmodified = "6",
			serializable = true,
			replicated = true,
			category = "Topology",
			range = { minimum = 2, maximum = 40 },
		},
		ResolutionY = {
			description = "Simulation vertex count along the Y axis.",
			type = t.Integer,
			unmodified = "6",
			serializable = true,
			replicated = true,
			category = "Topology",
			range = { minimum = 2, maximum = 40 },
		},
		ResolutionZ = {
			description = "Simulation vertex count along the Z axis.",
			type = t.Integer,
			unmodified = "6",
			serializable = true,
			replicated = true,
			category = "Topology",
			range = { minimum = 2, maximum = 40 },
		},
	},
})
