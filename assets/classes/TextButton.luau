const c, t, p = require("@tools/classes").unpack()

return c({
	name = "TextButton",
	extends = "TextLabel",
	description = "A focusable text control with pointer and keyboard activation",
	since = "0.2.0",
	defaultOverrides = {
		{ Property = "BackgroundTransparency", Value = 0 },
		{ Property = "BackgroundColor3", Value = { 0.18, 0.22, 0.30 } },
		{ Property = "Interactable", Value = true },
		{ Property = "Selectable", Value = true },
		{ Property = "InputSink", Value = "Activate" },
	},
})
