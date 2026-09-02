const c, t = require("@tools/classes").unpack()

return c({
	name = "CharacterControlService",
	description = "A narrow runtime bridge between replaceable Character policy and native network authority.",
	since = "0.1.0",

	signals = {
		ActionResolved = {
			description = "Fired when the server accepts or rejects a requested semantic Character action.",
			args = { t.Instance("Character"), t.String, t.Bool },
		},
		ActionEnded = {
			description = "Fired when a previously authoritative semantic Character action ends or is replaced.",
			args = { t.Instance("Character"), t.String },
		},
	},

	methods = {
		SubmitMoveIntent = {
			description = "Submits bounded semantic movement intent for the trusted local Character.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, MoveIntent: Vector2, FacingYawRadians: number, JumpRequested: boolean) -> boolean",
		},
		RequestAction = {
			description = "Requests a registered semantic action for the trusted local Character.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, ActionName: string) -> boolean",
		},
		SetMovementPolicy = {
			description = "Installs the runtime-local command-to-motion policy used by authority or prediction.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, Callback: (Character, number, Vector2, number, boolean) -> (Vector3, Vector3, number)) -> ()",
		},
		SetActionPolicy = {
			description = "Installs server gameplay admission policy for registered semantic actions.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, Callback: (Player, Character, string) -> boolean) -> ()",
		},
		RegisterAction = {
			description = "Registers bounded runtime action policy and pinned animation identity.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, ActionName: string, AnimationReference: string, DurationSeconds: number, RootTranslation: Vector3?, RootYawRadians: number?, Predictable: boolean?) -> boolean",
		},
		SetPredictionEnabled = {
			description = "Selects local prediction or server-authoritative presentation for custom policy.",
			asLuau = true,
			luauSignature = "(self: CharacterControlService, Enabled: boolean) -> ()",
		},
	},
})
