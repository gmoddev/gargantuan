const c, t = require("@tools/classes").unpack()

return c({
	name = "SpatialRegion",
	extends = "WorldRoot",
	description = "An independent spatial domain. Positions, physics, and inherited world queries are local to this domain.",
	since = "0.9.0",
})

