const c, t, p = require("@tools/classes").unpack()

return c({
	name = "Attachment",
	description = "Defines a point and orientation relative to an ancestor BasePart, optional canonical skeleton joint, or another Attachment.",
	since = "0.1.0",

	properties = {
		CFrame = {
			description = "Relative transform of this attachment.",
			type = t.CFrame,
			unmodified = "gargantuan::CFrame()",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
		JointPath = {
			description = "Optional canonical skeleton joint path used by the semantic spatial runtime.",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetJointPath",
			overrideWrite = "SetJointPath",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
		WorldCFrame = {
			description = "Current read-only world transform, including a resolved animated joint pose when bound.",
			type = t.CFrame,
			unmodified = "gargantuan::CFrame()",
			overrideRead = "GetWorldCFrame",
			writable = false,
			editable = false,
			category = "Transform",
		},
	},
})
