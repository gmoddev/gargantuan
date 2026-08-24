local c = require("@tools/classes").unpack()

return c({
	name = "EntitlementService",
	description = "Checks provider-neutral semantic entitlements for runtime-owned Player identities.",
	since = "0.4.0",

	methods = {
		CheckAsync = {
			description = "Checks one semantic entitlement for a Player without accepting caller-supplied identity.",
			asLuau = true,
			luauSignature = "(self: EntitlementService, player: Player, entitlementId: string) -> { Status: \"Granted\" | \"Denied\" | \"Unavailable\", EntitlementId: string, Identity: { Provider: string, Subject: string }, ExpiresUnixMilliseconds: number? }",
		},
		CheckManyAsync = {
			description = "Checks up to 32 semantic entitlements for a Player.",
			asLuau = true,
			luauSignature = "(self: EntitlementService, player: Player, entitlementIds: { string }) -> { { Status: \"Granted\" | \"Denied\" | \"Unavailable\", EntitlementId: string, Identity: { Provider: string, Subject: string }, ExpiresUnixMilliseconds: number? } }",
		},
	},
})
