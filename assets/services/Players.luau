const c, t = require("@tools/classes").unpack()

return c({
	name = "Players",
	description = "Owns DataModel-scoped gameplay player identities and local player lifecycle.",
	since = "0.1.0",

	properties = {
		LocalPlayer = {
			description = "The client-local Player. Local Studio Play currently creates exactly one.",
			type = t.Optional(t.Instance("Player")),
			unmodified = "std::nullopt",
			overrideRead = "GetLocalPlayer",
			writable = false,
		},
		CharacterAutoLoads = {
			description = "Whether the shipped controller requests a character during startup.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
		},
		DefaultControllerEnabled = {
			description = "Whether the engine-shipped default controller module starts for LocalPlayer.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
		},
		DefaultCameraEnabled = {
			description = "Whether the engine-shipped default camera module starts for LocalPlayer.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
		},
	},

	signals = {
		PlayerAdded = {
			description = "Fired after a Player enters this DataModel player lifetime.",
			args = { t.Instance("Player") },
		},
		PlayerRemoving = {
			description = "Fired before a Player leaves this DataModel player lifetime.",
			args = { t.Instance("Player") },
		},
	},

	methods = {
		GetPlayers = {
			description = "Returns the current Players in deterministic PlayerId order.",
			returns = t.Array(t.Instance("Player")),
		},
	},
})
