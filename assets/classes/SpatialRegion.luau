const c, t = require("@tools/classes").unpack()

return c({
	name = "SpatialRegion",
	extends = "WorldRoot",
	description = "An independent spatial domain. Positions, physics, and inherited world queries are local to this domain.",
	since = "0.9.0",
	properties = {
		Terrain = {
			description = "The terrain field owned by this spatial domain.",
			type = t.Instance("Terrain"),
			unmodified = "nullptr",
			overrideRead = "GetTerrain",
			writable = false,
			writeLevel = "Never",
			category = "World",
			editable = false,
		},
	},
})

