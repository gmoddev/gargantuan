local c, t, p = require("@tools/classes").unpack()

return c({
	name = "ScrollingFrame",
	extends = "Frame",
	description = "A retained clipped viewport whose children are translated by a semantic canvas position",
	since = "0.3.0",

	properties = {
		CanvasPosition = {
			description = "Current logical scroll offset; persisted as intentional game state",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2(0.0f, 0.0f)",
			serializable = true,
			replicated = true,
		},
		CanvasSize = {
			description = "Authored canvas extent resolved relative to the viewport size",
			type = t.UDim2,
			unmodified = "gargantuan::UDim2(0.0f, 0, 0.0f, 0)",
			serializable = true,
			replicated = true,
		},
		AutomaticCanvasSize = {
			description = "Axes whose canvas extent grows to include arranged children",
			type = t.EnumItem("AutomaticSize"),
			unmodified = "Enums::AutomaticSize::XY",
			serializable = true,
			replicated = true,
		},
		ScrollingDirection = {
			description = "Axes on which pointer, wheel, and keyboard scrolling are accepted",
			type = t.EnumItem("ScrollingDirection"),
			unmodified = "Enums::ScrollingDirection::Y",
			serializable = true,
			replicated = true,
		},
		ScrollBarThickness = {
			description = "Logical thickness of the renderer-neutral scrollbar indicator",
			type = t.Float,
			unmodified = "6.0f",
			serializable = true,
			replicated = true,
			range = { minimum = 0, maximum = 64 },
		},
		ContentExtent = {
			description = "Committed logical canvas extent",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2(0.0f, 0.0f)",
			writeLevel = "Never",
		},
	},
})
