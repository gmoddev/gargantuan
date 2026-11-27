const c, t = require("@tools/classes").unpack()

return c({
	name = "Player",
	description = "A gameplay player identity whose lifetime is independent from transport connections.",
	since = "0.1.0",

	properties = {
		PlayerGui = {
			description = "The runtime-owned screen interface container for this player.",
			type = t.Optional(t.Instance("PlayerGui")),
			unmodified = "std::nullopt",
			writable = false,
			overrideRead = "GetPlayerGui",
		},
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
		PossessedEntity = {
			description = "The current server-assigned gameplay entity, or nil if no target is available in this view.",
			type = t.Optional(t.Instance("Instance")),
			unmodified = "std::nullopt",
			overrideRead = "GetPossessedEntity",
			overrideWrite = "SetReplicatedPossessedEntity",
			writeLevel = "Engine",
			editable = false,
			replicated = true,
			materializationDependency = "Soft",
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
		RequestSpatialTransfer = {
			description = "Requests admission for the player's controlled root to a destination WorldRoot. A running server Script owns the resulting status handle.",
			asLuau = true,
			requiredCapability = "MutateDataModel",
			luauSignature = [=[(self: Player, Destination: WorldRoot, DestinationPose: CFrame, Minimum: Vector3, Maximum: Vector3) -> ({ GetStatus: (self: unknown) -> string, Cancel: (self: unknown) -> string }?, string?)]=],
		},
		SetPossession = {
			description = "Assigns an unoccupied live Workspace entity, or releases the current target with nil. Server only.",
			requiredCapability = "MutateDataModel",
			params = { { name = "Entity", type = t.Optional(t.Instance("Instance")) } },
		},
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
