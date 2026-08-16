const c = require("@tools/classes").define

return c({
	name = "UnreliableRemoteEvent",
	extends = "RemoteEventBase",
	description = "Best-effort unordered bounded application event",
	since = "0.1.0",
})
