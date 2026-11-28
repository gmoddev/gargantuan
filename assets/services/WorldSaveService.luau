const c = require("@tools/classes").unpack()

local Slot = "{ SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string }"
local function Operation(Value)
	local Result = "{ Ok: true, Value: " .. Value .. " } | { Ok: false, Code: string, Message: string, RecoveryId: string? }"
	return "{ Await: (self: any) -> (" .. Result .. "), GetResult: (self: any) -> (" .. Result .. ")?, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
end
return c({
	name = "WorldSaveService", description = "Manages world slots in the local project profile.", since = "0.9.0",
	methods = {
		GetCurrentWorld = { asLuau = true, description = "Reads the selected save in authoritative gameplay.", luauSignature = "(self: WorldSaveService) -> { Ok: true, Value: " .. Slot .. " } | { Ok: false, Code: string, Message: string }" },
		List = { asLuau = true, description = "Lists a page of saved worlds.", luauSignature = "(self: WorldSaveService, Cursor: string?) -> " .. Operation("{ Items: {" .. Slot .. "}, NextCursor: string? }") },
		Create = { asLuau = true, description = "Commits a world slot with an optional seed.", luauSignature = "(self: WorldSaveService, Options: {Name: string, Seed: string?}) -> " .. Operation(Slot) },
		ResolveCreate = { asLuau = true, description = "Reads a world slot after an uncertain create outcome.", luauSignature = "(self: WorldSaveService, RecoveryId: string) -> " .. Operation(Slot) },
	},
})
