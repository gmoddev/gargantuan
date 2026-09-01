local c, t = require("@tools/classes").unpack()

return c({
	name = "Character",
	extends = "Folder",
	description = "A movable character semantic root with one authoritative motion-admission boundary.",
	since = "0.1.0",

	properties = {
		CFrame = {
			description = "The authoritative world transform accepted by this Character's movement owner.",
			type = t.CFrame,
			unmodified = "gargantuan::CFrame(0.0f, 6.0f, 0.0f)",
			overrideRead = "GetCFrame",
			overrideWrite = "SetCFrame",
			serializable = true,
			replicated = true,
			category = "Transform",
			editorHint = "CFrameComponents",
		},
		Position = {
			description = "The world position component of CFrame.",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 6.0f, 0.0f)",
			overrideRead = "GetPosition",
			overrideWrite = "SetPosition",
			category = "Transform",
			editable = false,
		},
		RootPart = {
			description = "The optional spatial root synchronized to accepted Character movement.",
			type = t.Optional(t.Instance("BasePart")),
			unmodified = "std::nullopt",
			overrideRead = "GetRootPart",
			overrideWrite = "SetRootPart",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
	},

	methods = {
		Move = {
			description = "Requests bounded collision-authoritative world movement and returns the accepted result.",
			asLuau = true,
			luauSignature = "(self: Character, translation: Vector3, velocity: Vector3?) -> CharacterMotionResult",
		},
	},
})
