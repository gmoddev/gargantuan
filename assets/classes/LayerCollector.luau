const c, t, p = require("@tools/classes").unpack()

return c({
	name = "LayerCollector",
	extends = "GuiBase2d",
	description = "Abstract root for an ordered two-dimensional GUI layer",
	abstract = true,
	since = "0.2.0",

	properties = {
		Enabled = {
			description = "Whether this GUI layer participates in layout, rendering, and input",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
		},
		ResetOnSpawn = {
			description = "Whether player runtime policy may reset this layer on respawn",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		ZIndexBehavior = {
			description = "How descendant ZIndex values are interpreted",
			type = t.EnumItem("ZIndexBehavior"),
			unmodified = "Enums::ZIndexBehavior::Global",
			serializable = true,
			replicated = true,
		},
	},
})
