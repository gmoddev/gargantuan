const c = require("@tools/classes").define

return c({
	name = "ImageButton",
	extends = "ImageLabel",
	description = "A focusable image control with pointer and keyboard activation",
	since = "0.6.0",
	defaultOverrides = {
		{ Property = "BackgroundTransparency", Value = 0 },
		{ Property = "BackgroundColor3", Value = { 0.18, 0.22, 0.30 } },
		{ Property = "Interactable", Value = true },
		{ Property = "Selectable", Value = true },
		{ Property = "InputSink", Value = "Activate" },
	},
})
