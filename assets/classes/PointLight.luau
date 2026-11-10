const c = require("@tools/classes").unpack()

return c({
	name = "PointLight",
	extends = "LocalLight",
	description = "An omnidirectional local emitter.",
	since = "0.5.0",
})
