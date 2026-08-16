const c, t = require("@tools/classes").unpack()

return c({
	name = "RemoteEventBase",
	extends = "RemoteBase",
	description = "Internal base for bounded remote events",
	since = "0.1.0",
	abstract = true,
	properties = {
		OnClientEvent = {
			description = "Fires for a validated server event",
			type = t.RemoteSignal,
			unmodified = "std::shared_ptr<BaseSignal>{}",
			overrideRead = "GetOnClientEvent",
			writable = false,
			writeLevel = "Never",
		},
		OnServerEvent = {
			description = "Fires for a validated client event with provisional peer context",
			type = t.RemoteSignal,
			unmodified = "std::shared_ptr<BaseSignal>{}",
			overrideRead = "GetOnServerEvent",
			writable = false,
			writeLevel = "Never",
		},
	},
	methods = {
		FireServer = {
			description = "Sends bounded arguments from a client to its server",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, ...RemoteValue) -> ()",
		},
		FireClient = {
			description = "Sends bounded arguments from a server to one provisional peer identity",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, peerSlot: number, peerGeneration: number, ...RemoteValue) -> ()",
		},
		FireAllClients = {
			description = "Sends bounded arguments independently to every eligible peer",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, ...RemoteValue) -> ()",
		},
	},
})
