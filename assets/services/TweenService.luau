local c = require("@tools/classes").unpack()

return c({
	name = "TweenService",
	description = "Creates runtime tweens of writable properties on live Instances.",
	since = "0.6.0",
	methods = {
		Create = {
			description = "Validates and captures a bounded table of end values. Playback snapshots initial values when Play starts.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = "(self: TweenService, target: Instance, info: TweenInfo, goals: { [string]: any }) -> Tween",
		},
	},
})
