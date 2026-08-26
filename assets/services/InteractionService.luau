const c, t = require("@tools/classes").unpack()

return c({
	name = "InteractionService",
	description = "Owns bounded proximity discovery, activation validation, hold state, and presentation state.",
	since = "0.2.0",

	properties = {
		ActivePrompt = {
			description = "Current deterministic prompt candidate for the local Player.",
			type = t.Optional(t.Instance("ProximityPrompt")),
			unmodified = "std::nullopt",
			overrideRead = "GetActivePrompt",
			writable = false,
		},
		Available = {
			description = "Whether the local Player currently has a valid interaction candidate.",
			type = t.Bool,
			unmodified = "false",
			overrideRead = "GetAvailable",
			writable = false,
		},
		ActionText = {
			description = "Bounded presentation snapshot of the current prompt action text.",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetActionText",
			writable = false,
		},
		ObjectText = {
			description = "Bounded presentation snapshot of the current prompt object text.",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetObjectText",
			writable = false,
		},
		HoldDuration = {
			description = "Presentation snapshot of the current prompt hold duration.",
			type = t.Float,
			unmodified = "0.0f",
			overrideRead = "GetHoldDuration",
			writable = false,
		},
		HoldProgress = {
			description = "Current local hold progress from zero through one.",
			type = t.Float,
			unmodified = "0.0f",
			overrideRead = "GetHoldProgress",
			writable = false,
			range = { minimum = 0, maximum = 1 },
		},
		InputHint = {
			description = "Short default-platform input hint for presentation only.",
			type = t.String,
			unmodified = '"E / A"',
			overrideRead = "GetInputHint",
			writable = false,
		},
		DefaultPresentationEnabled = {
			description = "Whether this runtime owns the shipped screen-space prompt presentation.",
			type = t.Bool,
			unmodified = "false",
			overrideRead = "GetDefaultPresentationEnabled",
			writable = false,
		},
	},

	signals = {
		PresentationChanged = {
			description = "Fired at a bounded semantic cadence when the local presentation snapshot changes.",
		},
	},

	methods = {
		BeginActivation = {
			description = "Begins the local presentation activation path; Engine still validates the active prompt.",
		},
		EndActivation = {
			description = "Ends the local presentation activation path and cancels an incomplete hold.",
		},
	},
})
