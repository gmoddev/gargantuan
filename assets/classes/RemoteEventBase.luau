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
			description = "Fires on the server with the sender's peer context followed by the arguments",
			type = t.RemoteSignal,
			unmodified = "std::shared_ptr<BaseSignal>{}",
			overrideRead = "GetOnServerEvent",
			writable = false,
			writeLevel = "Never",
		},
		OnPlayerEvent = {
			description = "Fires on the server with the current sending Player followed by the arguments",
			type = t.RemoteSignal,
			unmodified = "std::shared_ptr<BaseSignal>{}",
			overrideRead = "GetOnPlayerEvent",
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
			description = "Submits to one current Player or to an explicit peer slot and generation",
			asLuau = true,
			luauSignature = "((self: RemoteEventBase, Target: Player, ...RemoteValue) -> ()) & ((self: RemoteEventBase, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> ())",
		},
		FireAllClients = {
			description = "Sends bounded arguments independently to every eligible peer",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, ...RemoteValue) -> ()",
		},
		TryFireServer = {
			description = "Returns local admission and a reason code for submission to the current server",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, ...RemoteValue) -> (boolean, string)",
		},
		TryFireClient = {
			description = "Returns local admission and a reason code for submission to one Player or explicit peer",
			asLuau = true,
			luauSignature = "((self: RemoteEventBase, Target: Player, ...RemoteValue) -> (boolean, string)) & ((self: RemoteEventBase, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> (boolean, string))",
		},
		TryFireAllClients = {
			description = "Returns accepted and rejected counts plus rejected counts by reason code",
			asLuau = true,
			luauSignature = "(self: RemoteEventBase, ...RemoteValue) -> (number, number, {[string]: number})",
		},
	},
})
