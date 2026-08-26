const c, t = require("@tools/classes").unpack()

return c({
	name = "ProximityPrompt",
	description = "Declares a bounded world interaction anchored by an ancestor BasePart or Attachment.",
	since = "0.2.0",

	properties = {
		Enabled = {
			description = "Whether this prompt can become eligible for activation.",
			type = t.Bool,
			unmodified = "true",
			serializable = true,
			replicated = true,
			category = "Interaction",
		},
		ActionText = {
			description = "Short verb presented for the interaction.",
			type = t.String,
			unmodified = '"Interact"',
			overrideRead = "GetActionText",
			overrideWrite = "SetActionText",
			serializable = true,
			replicated = true,
			category = "Interaction",
		},
		ObjectText = {
			description = "Optional short label identifying the interaction target.",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetObjectText",
			overrideWrite = "SetObjectText",
			serializable = true,
			replicated = true,
			category = "Interaction",
		},
		MaxActivationDistance = {
			description = "Maximum world-space distance at which activation is valid.",
			type = t.Float,
			unmodified = "10.0f",
			serializable = true,
			replicated = true,
			category = "Interaction",
			range = { minimum = 0.01, maximum = 64 },
		},
		HoldDuration = {
			description = "Monotonic hold duration in seconds; zero activates on the press edge.",
			type = t.Float,
			unmodified = "0.0f",
			serializable = true,
			replicated = true,
			category = "Interaction",
			range = { minimum = 0, maximum = 30 },
		},
	},

	signals = {
		Triggered = {
			description = "Fired exactly once after Engine validates a completed activation.",
			args = { t.Instance("Player") },
		},
	},
})
