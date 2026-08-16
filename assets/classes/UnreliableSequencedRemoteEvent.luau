const c = require("@tools/classes").define

return c({
	name = "UnreliableSequencedRemoteEvent",
	extends = "RemoteEventBase",
	description = "Best-effort newest-wins bounded application state event",
	since = "0.1.0",
})
