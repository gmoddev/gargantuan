local c = require("@tools/classes").unpack()

return c({
	name = "DataStoreService",
	description = "Submits bounded persistence work to the server's configured provider.",
	since = "0.6.0",
	servicePolicy = "ServerOnly",
	methods = {
		GetKeyValueStore = {
			description = "Gets a named store for native buffer values.",
			asLuau = true,
			requiredCapability = "PersistenceAccess",
			luauSignature = "(self: DataStoreService, Name: string) -> { GetAsync: (any, string, number?) -> any, PutAsync: (any, string, buffer, string?, number?) -> any, DeleteAsync: (any, string, string?, number?) -> any, GetCapabilities: (any) -> { KeyValue: boolean, Documents: boolean, DocumentQueries: boolean } }",
		},
		GetDocumentStore = {
			description = "Gets a named collection for structured documents.",
			asLuau = true,
			requiredCapability = "PersistenceAccess",
			luauSignature = "(self: DataStoreService, Name: string) -> { GetAsync: (any, string, number?) -> any, PutAsync: (any, string, { [string]: any }, string?, number?) -> any, DeleteAsync: (any, string, string?, number?) -> any, QueryAsync: (any, { [string]: any }, number?) -> any, GetCapabilities: (any) -> { KeyValue: boolean, Documents: boolean, DocumentQueries: boolean } }",
		},
		GetNull = {
			description = "Returns the sentinel for a stored document null.",
			asLuau = true,
			requiredCapability = "PersistenceAccess",
			luauSignature = "(self: DataStoreService) -> unknown",
		},
		CreateArray = {
			description = "Creates a document array that retains its type when empty.",
			asLuau = true,
			requiredCapability = "PersistenceAccess",
			luauSignature = "(self: DataStoreService) -> { any }",
		},
	},
})
