const c = require("@tools/classes").unpack()

return c({
	name = "Animator",
	description = "Owns bounded runtime animation tracks for the skinned MeshPart that directly contains it.",
	since = "0.5.0",

	methods = {
		LoadAnimation = {
			description = "Loads one canonical Animation asset into an immutable runtime-only AnimationTrack.",
			asLuau = true,
			luauSignature = "(self: Animator, animationId: string) -> AnimationTrack",
		},
	},
})
