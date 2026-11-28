const c = require("@tools/classes").unpack()

const Info = '{ Generation: string, State: "Idle" | "Starting" | "Connecting" | "Active" | "Leaving", World: "Absent" | "Preparing" | "Ready", Mode: ("Offline" | "Remote")?, SaveId: string?, WorldId: string?, LastError: {Code: string, Message: string}? }'
const Result = "{ Ok: true, Value: " .. Info .. " } | { Ok: false, Code: string, Message: string }"
const Operation = "{ Await: (self: any) -> (" .. Result .. "), GetResult: (self: any) -> (" .. Result .. ")?, GetState: (self: any) -> string, Cancel: (self: any) -> boolean, Completed: Signal }"
return c({
	name = "SessionService",
	description = "Controls the gameplay session owned by a standalone application.",
	since = "0.9.0",
	methods = {
		Reconnect = { asLuau = true, description = "Starts a fresh connection to the last disconnected session.",
			luauSignature = "(self: SessionService) -> " .. Operation },
		GetSession = { asLuau = true, description = "Returns a session snapshot.", luauSignature = "(self: SessionService) -> " .. Info },
		StartLocal = { asLuau = true, description = "Starts an offline session with an optional saved world.", luauSignature = "(self: SessionService, Options: { SaveId: string?, PrivateServer: boolean? }?) -> " .. Operation },
		Leave = { asLuau = true, description = "Retires the current gameplay session.", luauSignature = "(self: SessionService) -> " .. Operation },
		Join = { asLuau = true, description = "Connects to an admitted direct target.", luauSignature = '(self: SessionService, Target: {Kind: "Direct", Address: string}) -> ' .. Operation },
	},
})
