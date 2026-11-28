const c = require("@tools/classes").unpack()
return c({
	name = "ApplicationService",
	description = "Controls the enclosing standalone application.",
	since = "0.9.0",
	methods = {
		Quit = { asLuau = true, description = "Requests application shutdown after the current callback.",
			luauSignature = "(self: ApplicationService) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }" },
	},
})
