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
			writable = true,
			overrideRead = "GetPlayerId",
			overrideWrite = "SetReplicatedPlayerId",
			replicated = true,
		},
		Character = {
			description = "The current runtime character, or nil while no character is spawned.",
			type = t.Optional(t.Instance("Character")),
			unmodified = "std::nullopt",
			overrideRead = "GetCharacter",
			overrideWrite = "SetCharacter",
			replicated = true,
			materializationDependency = "Hard",
		},
	},

	signals = {
		CharacterAdded = {
			description = "Fired after a new character relationship is committed.",
			args = { t.Instance("Character") },
		},
		CharacterRemoving = {
			description = "Fired before the old character is destroyed or externally removed.",
			args = { t.Instance("Character") },
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
