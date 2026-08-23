const c, t, p = require("@tools/classes").unpack()

return c({
	name = "UIListLayout",
	extends = "GuiBase",
	description = "A deterministic one-axis layout for direct GuiObject siblings",
	since = "0.2.0",

	properties = {
		FillDirection = {
			description = "The main axis used to arrange siblings",
			type = t.EnumItem("GuiFillDirection"),
			unmodified = "Enums::GuiFillDirection::Vertical",
			serializable = true,
			replicated = true,
		},
		Padding = {
			description = "Spacing between adjacent siblings along the main axis",
			type = t.UDim,
			unmodified = "gargantuan::UDim(0.0f, 0)",
			serializable = true,
			replicated = true,
		},
	},
})
