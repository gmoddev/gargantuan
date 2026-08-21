const c, t = require("@tools/classes").unpack()

return c({
	name = "Player",
	description = "A gameplay player identity whose lifetime is independent from transport connections.",
	since = "0.1.0",

	properties = {
		PlayerId = {
			description = "Nonzero runtime player identity independent of ConnectionId.",
			type = t.Integer,
			unmodified = "0",
			writable = false,
		},
		Character = {
			description = "The current runtime character, or nil while no character is spawned.",
			type = t.Optional(t.Instance("KinematicCharacter")),
			unmodified = "std::nullopt",
			overrideRead = "GetCharacter",
			overrideWrite = "SetCharacter",
		},
	},

	signals = {
		CharacterAdded = {
			description = "Fired after a new character relationship is committed.",
			args = { t.Instance("KinematicCharacter") },
		},
		CharacterRemoving = {
			description = "Fired before the old character is destroyed or externally removed.",
			args = { t.Instance("KinematicCharacter") },
		},
		CharacterSpawnRequested = {
			description = "Fired synchronously so replaceable Luau assembly policy can create a character.",
		},
	},

	methods = {
		LoadCharacter = {
			description = "Removes the current character and requests a fresh character assembly.",
		},
		ResetCharacter = {
			description = "Resets the current character through the same spawn contract.",
		},
		RemoveCharacter = {
			description = "Clears and deterministically destroys the current character.",
		},
	},
})
