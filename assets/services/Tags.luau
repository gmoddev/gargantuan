local c = require("@tools/classes").unpack()

return c({
	name = "Tags",
	description = "Provides bounded, DataModel-scoped Instance tag membership and indexed queries.",
	since = "0.1.0",

	methods = {
		Add = { description = "Adds a tag through authoritative mutation.", asLuau = true, luauSignature = "(self: Tags, instance: Instance, tag: string) -> ()" },
		Remove = { description = "Removes a tag through authoritative mutation.", asLuau = true, luauSignature = "(self: Tags, instance: Instance, tag: string) -> ()" },
		Has = { description = "Returns whether an Instance has a tag.", asLuau = true, luauSignature = "(self: Tags, instance: Instance, tag: string) -> boolean" },
		GetTags = { description = "Returns an Instance's tags in deterministic name order.", asLuau = true, luauSignature = "(self: Tags, instance: Instance) -> { string }" },
		GetTagged = { description = "Returns live tagged Instances in ObjectId order.", asLuau = true, luauSignature = "(self: Tags, tag: string) -> { Instance }" },
		GetTaggedAll = { description = "Returns the intersection of explicit tags in ObjectId order.", asLuau = true, luauSignature = "(self: Tags, tags: { string }) -> { Instance }" },
	},
})
