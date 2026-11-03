const c, t, p = require("@tools/classes").unpack()

return c({
	-- "oh im deadnaming Luau" ok in this case its justified because of
	-- GlslSourceContainer, plus the interface we use is Lua. Plus what if we
	-- wanted to support more than 1 Lua dialect??

	name = "LuaSourceContainer",
	description = "Abstract class for compiling Lua bytecode",
	abstract = true,
	since = "0.1.0",

	properties = {
		SourceId = {
			description = "Engine-owned persistent Script resource identity; not a live ObjectId or authority token",
			type = t.String,
			unmodified = "std::string()",
			overrideRead = "GetSourceId",
			overrideWrite = "SetSourceId",
			serializable = true,
			editable = false,
			readLevel = "Engine",
			writeLevel = "Engine",
		},
		Source = {
			description = "Authoritative Luau source code",
			type = t.String,
			unmodified = "std::string()",
			overrideRead = "GetSource",
			overrideWrite = "SetSource",
			serializable = true,
			editable = false,
			readLevel = "Engine",
			writeLevel = "Engine",
		},
		SourceVersion = {
			description = "Transient source invalidation token",
			type = t.Integer,
			unmodified = "1",
			overrideRead = "GetSourceVersion",
			overrideWrite = "SetSourceVersion",
			replicated = true,
			editable = false,
			writeLevel = "Engine",
		},
	},
})
