const c, t, p = require("@tools/classes").unpack()

return c({
	name = "ScreenGui",
	extends = "LayerCollector",
	description = "A screen-space GUI root resolved against the runtime viewport and safe area",
	since = "0.2.0",

	properties = {
		DisplayOrder = {
			description = "Ordering of this screen root relative to other roots",
			type = t.Integer,
			unmodified = "0",
			serializable = true,
			replicated = true,
		},
		ClipToSafeArea = {
			description = "Whether descendants are clipped to the viewport safe area",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
		},
	},
})
