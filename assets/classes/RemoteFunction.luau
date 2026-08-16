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
			luauSignature = "(self: RemoteFunction, peerSlot: number, peerGeneration: number, ...RemoteValue) -> ...RemoteValue",
		},
		InvokeClientWithTimeout = {
			description = "Invokes one client with a finite caller-selected deadline",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, peerSlot: number, peerGeneration: number, timeoutSeconds: number, ...RemoteValue) -> ...RemoteValue",
		},
		SetServerHandler = {
			description = "Installs the server request callback for this runtime-only session",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, callback: (RemotePeer, ...RemoteValue) -> ...RemoteValue) -> ()",
		},
		SetClientHandler = {
			description = "Installs the client request callback for this runtime-only session",
			asLuau = true,
			luauSignature = "(self: RemoteFunction, callback: (...RemoteValue) -> ...RemoteValue) -> ()",
		},
	},
})
