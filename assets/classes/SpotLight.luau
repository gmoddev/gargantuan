const c, t = require("@tools/classes").unpack()

return c({
	name = "SpotLight",
	extends = "LocalLight",
	description = "A local emitter directed along its LightCFrame's negative Z axis.",
	since = "0.5.0",
	properties = {
		LightConeAngles = {
			description = "Inner and outer cone half angles in degrees. The outer angle is 1 to 89 degrees and the inner angle is 0 through the outer angle.",
			type = t.Vector2,
			unmodified = "Vector2(20.0f, 35.0f)",
			overrideRead = "GetLightConeAngles",
			overrideWrite = "SetLightConeAngles",
			serializable = true,
			replicated = true,
			category = "Lighting",
		},
	},
})
