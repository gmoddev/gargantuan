const c = require("@tools/classes").define

return c({
	name = "RemoteFunction",
	extends = "RemoteBase",
	description = "Bounded reliable request and terminal response",
	since = "0.1.0",
	methods = {
		InvokeServer = {
			description = "Suspends only the calling client coroutine until a terminal result",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, ...RemoteValue) -> ...RemoteValue",
		},
		InvokeServerWithTimeout = {
			description = "Invokes the server with a finite caller-selected deadline",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, timeoutSeconds: number, ...RemoteValue) -> ...RemoteValue",
		},
		InvokeClient = {
			description = "Suspends only the calling server coroutine until one client returns",
			asLuau = true,
			luauSignature = "((self: RemoteFunction, Target: Player, ...RemoteValue) -> ...RemoteValue) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, ...RemoteValue) -> ...RemoteValue)",
		},
		InvokeClientWithTimeout = {
			description = "Invokes one client with a finite caller-selected deadline",
			asLuau = true,
			luauSignature = "((self: RemoteFunction, Target: Player, TimeoutSeconds: number, ...RemoteValue) -> ...RemoteValue) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, TimeoutSeconds: number, ...RemoteValue) -> ...RemoteValue)",
		},
		TryInvokeServer = {
			description = "Invokes the server with an explicit deadline and a boolean before results or failure details",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue)",
		},
		TryInvokeClient = {
			description = "Invokes one Player or explicit peer with a deadline and a boolean before results or failure details",
			asLuau = true,
			luauSignature = "((self: RemoteFunction, Target: Player, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue)) & ((self: RemoteFunction, PeerSlot: number, PeerGeneration: number, TimeoutSeconds: number, ...RemoteValue) -> (boolean, ...RemoteValue))",
		},
		SetServerHandler = {
			description = "Installs the server request callback with peer context as its first argument",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, callback: (RemotePeer, ...RemoteValue) -> ...RemoteValue) -> ()",
		},
		SetPlayerHandler = {
			description = "Installs the server request callback with the current Player as its first argument",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, Callback: (Player, ...RemoteValue) -> ...RemoteValue) -> ()",
		},
		SetClientHandler = {
			description = "Installs the client request callback for this runtime-only session",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, callback: (...RemoteValue) -> ...RemoteValue) -> ()",
		},
	},
})
