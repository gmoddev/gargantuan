const c, t = require("@tools/classes").unpack()
return c({
	name = "ApplicationService",
	description = "Controls the enclosing standalone application.",
	since = "0.9.0",
	signals = {
		Closing = { description = "Fires once when the local host commits to closing the application.", args = {t.Bool} },
	},
	methods = {
		GetState = { asLuau = true, description = "Returns the state of the local presentation, when available.",
			luauSignature = '(self: ApplicationService) -> ("Starting" | "Running" | "Closing")?' },
		Quit = { asLuau = true, description = "Requests application shutdown after the current callback.",
			luauSignature = "(self: ApplicationService) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }" },
	},
})
