const c = require("@tools/classes").unpack()

const Info = '{ Generation: string, State: "Idle" | "Starting" | "Active" | "Leaving", World: "Absent" | "Ready", Mode: "Offline"? }'
const Result = "{ Ok: true, Value: " .. Info .. " } | { Ok: false, Code: string, Message: string }"
const Operation = "{ Await: (self: any) -> (" .. Result .. "), GetResult: (self: any) -> (" .. Result .. ")?, GetState: (self: any) -> string, Cancel: (self: any) -> boolean, Completed: Signal }"
return c({
	name = "SessionService",
	description = "Controls the gameplay session owned by a standalone application.",
	since = "0.9.0",
	methods = {
		GetSession = { asLuau = true, description = "Returns a session snapshot.", luauSignature = "(self: SessionService) -> " .. Info },
		StartLocal = { asLuau = true, description = "Starts an offline session at the next host boundary.", luauSignature = "(self: SessionService) -> " .. Operation },
		Leave = { asLuau = true, description = "Retires the current gameplay session.", luauSignature = "(self: SessionService) -> " .. Operation },
	},
})
