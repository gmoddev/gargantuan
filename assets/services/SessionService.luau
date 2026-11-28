const c = require("@tools/classes").unpack()

const Info = '{ Generation: string, State: "Idle" | "Starting" | "Connecting" | "Active" | "Leaving", World: "Absent" | "Preparing" | "Ready", Mode: ("Offline" | "PrivateHost" | "Remote")?, SaveId: string?, WorldId: string?, LastError: {Code: string, Message: string}? }'
local function OperationType(Value)
	const Result = "{ Ok: true, Value: " .. Value .. " } | { Ok: false, Code: string, Message: string }"
	return "{ Await: (self: any) -> (" .. Result .. "), GetResult: (self: any) -> (" .. Result .. ")?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
end
const Operation = OperationType(Info)
return c({
	name = "SessionService",
	description = "Controls the gameplay session owned by a standalone application.",
	since = "0.9.0",
	methods = {
		FindServers = { asLuau = true, description = "Lists current sessions supplied by the native launcher for the selected source.",
			luauSignature = '(self: SessionService, Source: "LAN" | "Node", Cursor: string?) -> ' .. OperationType("{Items: {{Id: string, Name: string}}, NextCursor: string?}") },
		Reconnect = { asLuau = true, description = "Starts a fresh connection to the last disconnected session.",
			luauSignature = "(self: SessionService) -> " .. Operation },
		GetSession = { asLuau = true, description = "Returns a session snapshot.", luauSignature = "(self: SessionService) -> " .. Info },
		StartLocal = { asLuau = true, description = "Starts an offline world or its provisioned private Server.", luauSignature = "(self: SessionService, Options: { SaveId: string?, PrivateServer: boolean? }?) -> " .. Operation },
		Leave = { asLuau = true, description = "Retires the current gameplay session.", luauSignature = "(self: SessionService) -> " .. Operation },
		Join = { asLuau = true, description = "Connects to an admitted direct target.", luauSignature = '(self: SessionService, Target: ({Kind: "Direct", Address: string} | {Kind: "Listing", Id: string})) -> ' .. Operation },
	},
})
