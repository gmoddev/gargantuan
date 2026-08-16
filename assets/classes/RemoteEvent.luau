const c = require("@tools/classes").define

return c({
	name = "RemoteEvent",
	extends = "RemoteEventBase",
	description = "Reliable ordered bounded application event",
	since = "0.1.0",
})
