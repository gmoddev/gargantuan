const c, t, p = require("@tools/classes").unpack()

return c({
	name = "BasePart",
	description = "Abstract class for workspace objects that physically interact.",
	abstract = true,
	since = "0.1.0",

	properties = {
		Anchored = {
			description = "Whether to not interact with physics",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
			category = "Behavior",
		},
		CanCollide = {
			description = "Whether other BaseParts can collide with this instance",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Collision",
		},
		CastShadow = {
			description = "Whether to cast a shadow from this instance",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Appearance",
		},
		CanTouch = {
			description = "Whether this instance can register touched events.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Collision",
		},
		CFrame = {
			description = "The world transform of this instance",
			type = t.CFrame,
			unmodified = "gargantuan::CFrame()",
			serializable = true,
			replicated = true,
			category = "Transform",
			editorHint = "CFrameComponents",
		},
		Color = {
			description = "The visual color of this instance",
			type = t.Color3,
			unmodified = "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
			serializable = true,
			replicated = true,
			category = "Appearance",
			editorHint = "ColorComponents",
			range = { minimum = 0, maximum = 1 },
		},
		Size = {
			description = "The world size of this instance",
			type = t.Vector3,
			unmodified = "glm::vec3(1.0f, 1.0f, 1.0f)",
			serializable = true,
			replicated = true,
			category = "Transform",
		},
		Position = {
			description = "The world position of this instance",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 0.0f, 0.0f)",
			overrideRead = "GetPosition",
			overrideWrite = "SetPosition",
			category = "Transform",
			editable = false,
		},
		Rotation = {
			description = "The world rotation of this instance",
			type = t.Vector3,
			unmodified = "glm::vec3(0.0f, 0.0f, 0.0f)",
			overrideRead = "GetRotation",
			overrideWrite = "SetRotation",
			category = "Transform",
			editable = false,
		},
		Transparency = {
			description = "The visual transparency of this instance",
			type = t.Float,
			unmodified = "0.0f",
			serializable = true,
			replicated = true,
			category = "Appearance",
			range = { minimum = 0, maximum = 1 },
		},
	},

	signals = {
		Touched = {
			description = "Fired when another BasePart touches this instance.",
			args = { t.Instance("BasePart") },
		},
		TouchEnded = {
			description = "Fired when another BasePart has stopped touching this instance.",
			args = { t.Instance("BasePart") },
		},
	},

	methods = {
		ApplyImpulse = {
			description = "Applies an impulse onto this instance.",
			params = { p("impulse", t.Vector3) },
		},
	},
})
