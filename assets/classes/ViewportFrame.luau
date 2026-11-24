const c, t = require("@tools/classes").unpack()

return c({
	name = "ViewportFrame",
	extends = "GuiObject",
	description = "Composes a local 3D presentation scene into a GUI rectangle.",
	since = "0.9.0",
	properties = {
		CurrentCamera = {
			description = "Camera inside this ViewportFrame's presentation subtree.",
			type = t.Optional(t.Instance("Camera")),
			unmodified = "std::nullopt",
			overrideRead = "GetCurrentCamera",
			overrideWrite = "SetCurrentCamera",
			serializable = true,
			replicated = true,
			category = "Camera",
		},
		RenderScale = {
			description = "Requested fraction of the physical GUI size used to render the preview.",
			type = t.Float,
			unmodified = "1.0f",
			range = { minimum = 0.25, maximum = 1 },
			serializable = true,
			replicated = true,
			category = "Appearance",
		},
		MaxUpdateRate = {
			description = "Maximum presentation scene updates per second when content is changing.",
			type = t.Float,
			unmodified = "30.0f",
			range = { minimum = 1, maximum = 60 },
			serializable = true,
			replicated = true,
			category = "Behavior",
		},
	},
})
