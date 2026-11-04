const c, t, p = require("@tools/classes").unpack()

return c({
	name = "ModuleScript",
	extends = "LuaSourceContainer",
	description = "Class that contains a Lua module which can be required at runtime",
	since = "0.1.0",
	properties = {
		ReplicatedSource = {
			description = "Engine-only projection of source explicitly stored in a replicated service",
			type = t.String,
			unmodified = "std::string()",
			overrideRead = "GetReplicatedSource",
			overrideWrite = "SetReplicatedSource",
			replicated = true,
			editable = false,
			readLevel = "Engine",
			writeLevel = "Engine",
		},
	},
})
