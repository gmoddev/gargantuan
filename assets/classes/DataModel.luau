const c, t, p = require("@tools/classes").unpack()
const MenuResult = "{ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"

return c({
	name = "DataModel",
	extends = "ServiceProvider",
	description = "Class that provides the root hierachy and services",
	since = "0.1.0",
	properties = {
		ApplicationEntry = {
			description = "Project-local durable SourceId of the standalone client entry. Empty uses direct startup.",
			type = t.String,
			unmodified = "std::string()",
			serializable = true,
			replicated = false,
			readLevel = "Engine",
			writeLevel = "Engine",
			category = "Application",
		},
	},
	methods = {
		SetMainMenu = {
			description = "Registers the application's ScreenGui. Pass nil to clear the selection.",
			asLuau = true,
			luauSignature = "(self: DataModel, Menu: ScreenGui?) -> " .. MenuResult,
		},
		SetMainMenuVisible = {
			description = "Shows or hides the registered application menu.",
			asLuau = true,
			luauSignature = "(self: DataModel, Visible: boolean) -> " .. MenuResult,
		},
		IsMainMenuVisible = {
			description = "Reads the current application's menu visibility.",
			asLuau = true,
			luauSignature = "(self: DataModel) -> boolean",
		},
	},
})
