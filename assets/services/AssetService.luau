local c = require("@tools/classes").unpack()

return c({
	name = "AssetService",
	description = "Resolves stable project and built-in asset references through one bounded catalog.",
	since = "0.4.0",

	methods = {
		ResolveAsset = { description = "Returns the canonical available asset reference, or nil.", asLuau = true, luauSignature = "(self: AssetService, reference: string) -> string?" },
		GetAssetMetadata = { description = "Returns bounded semantic metadata for a known asset, or nil.", asLuau = true, luauSignature = "(self: AssetService, reference: string) -> { [string]: any }?" },
		IsAssetAvailable = { description = "Returns whether a reference has a valid last-known-good canonical asset.", asLuau = true, luauSignature = "(self: AssetService, reference: string) -> boolean" },
	},
})
