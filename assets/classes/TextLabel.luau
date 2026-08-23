const c, t, p = require("@tools/classes").unpack()

return c({
	name = "TextLabel",
	extends = "GuiObject",
	description = "A retained GUI object that presents shaped UTF-8 text",
	since = "0.2.0",

	properties = {
		Text = {
			description = "UTF-8 text content",
			type = t.String,
			unmodified = '""',
			serializable = true,
			replicated = true,
		},
		TextColor3 = {
			description = "Text color",
			type = t.Color3,
			unmodified = "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
			serializable = true,
			replicated = true,
		},
		TextTransparency = {
			description = "Text transparency from zero opaque to one transparent",
			type = t.Float,
			unmodified = "0.0f",
			serializable = true,
			replicated = true,
			range = { minimum = 0, maximum = 1 },
		},
		TextSize = {
			description = "Logical text size before DPI scaling",
			type = t.Float,
			unmodified = "16.0f",
			serializable = true,
			replicated = true,
			range = { minimum = 1, maximum = 256 },
		},
		TextWrapped = {
			description = "Whether text wraps at the arranged width",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		TextXAlignment = {
			description = "Horizontal text alignment",
			type = t.EnumItem("TextXAlignment"),
			unmodified = "Enums::TextXAlignment::Center",
			serializable = true,
			replicated = true,
		},
		TextYAlignment = {
			description = "Vertical text alignment",
			type = t.EnumItem("TextYAlignment"),
			unmodified = "Enums::TextYAlignment::Center",
			serializable = true,
			replicated = true,
		},
		FontFace = {
			description = "Logical font face identity resolved by the GUI font provider",
			type = t.String,
			unmodified = '"GargantuanSans"',
			serializable = true,
			replicated = true,
		},
	},
})
