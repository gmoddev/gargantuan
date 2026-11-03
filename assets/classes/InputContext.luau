const c, t = require("@tools/classes").unpack()

return c({
	name = "InputContext",
	description = "A persistent enabled input-action context with deterministic priority.",
	since = "0.2.0",
	properties = {
		Enabled = { description = "Whether this context may activate actions.", type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Input" },
		Priority = { description = "Higher priorities evaluate before lower priorities.", type = t.Integer, unmodified = "0", serializable = true, replicated = true, category = "Input" },
		Consume = { description = "Whether matching actions suppress lower priority contexts.", type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Input" },
	},
	methods = {
		AddAction = {
			description = "Creates and parents a canonical InputAction after validating its options.",
			asLuau = true,
			luauSignature = "(Name: string, Options: { Type: Enum.InputActionType?, Enabled: boolean? }?) -> InputAction",
		},
	},
})
