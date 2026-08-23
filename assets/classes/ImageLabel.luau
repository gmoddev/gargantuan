const c, t, p = require("@tools/classes").unpack()

return c({
	name = "ImageLabel",
	extends = "GuiObject",
	description = "A retained GUI object that presents a logical image resource",
	since = "0.2.0",

	properties = {
		Image = {
			description = "Strict AssetService image reference",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetImage",
			overrideWrite = "SetImage",
			serializable = true,
			replicated = true,
		},
		ImageColor3 = {
			description = "Image tint color",
			type = t.Color3,
			unmodified = "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
			serializable = true,
			replicated = true,
		},
		ImageTransparency = {
			description = "Image transparency from zero opaque to one transparent",
			type = t.Float,
			unmodified = "0.0f",
			serializable = true,
			replicated = true,
			range = { minimum = 0, maximum = 1 },
		},
		ScaleType = {
			description = "Image fitting policy; Foundation 1 supports Stretch",
			type = t.EnumItem("GuiImageScaleType"),
			unmodified = "Enums::GuiImageScaleType::Stretch",
			serializable = true,
			replicated = true,
		},
	},
})
