const c = require("@tools/classes").unpack()

return c({
	name = "InputService",
	description = "Owns persistent input contexts and local derived action evaluation.",
	since = "0.2.0",
	methods = {
		AddContext = {
			description = "Creates and parents a canonical InputContext after validating its options.",
			asLuau = true,
			luauSignature = "(Name: string, Options: { Priority: number?, Enabled: boolean?, Consume: boolean? }?) -> InputContext",
		},
		GetRuntimeSnapshot = {
			description = "Returns a bounded read-only copy of local compiled state and diagnostics.",
			asLuau = true,
			luauSignature = "() -> { [string]: any }",
		},
	},
})
