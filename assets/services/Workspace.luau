const c, t = require("@tools/classes").unpack()

return c({
	name = "Workspace",
	extends = "WorldRoot",
	description = "Provides a physical 3D workspace",
	since = "0.1.0",

	properties = {
		CurrentCamera = {
			description = "The current camera.",
			type = t.Instance("Camera"),
			unmodified = "nullptr",
			writeLevel = "Never",
			category = "Camera",
			editable = false,
		},
		Terrain = {
			description = "The world-owned terrain object.",
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
