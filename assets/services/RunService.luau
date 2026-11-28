const c, t, p = require("@tools/classes").unpack()
const PauseResult = "{ Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"
const PauseOperation = "{ Await: (self: any) -> (" .. PauseResult .. "), GetResult: (self: any) -> (" .. PauseResult .. ")?, GetState: (self: any) -> string, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"

return c({
	name = "RunService",
	description = "Provides access to the runtime.",
	since = "0.1.0",
	methods = {
		PauseGameplay = { asLuau = true, description = "Requests a pause at the next simulation boundary.",
			luauSignature = "(self: RunService) -> " .. PauseOperation },
		ResumeGameplay = { asLuau = true, description = "Releases the caller's gameplay pause.",
			luauSignature = "(self: RunService) -> " .. PauseOperation },
		IsGameplayPaused = { asLuau = true, description = "Returns the committed gameplay pause state.",
			luauSignature = "(self: RunService) -> boolean" },
	},

	signals = {
		GameplayPauseChanged = { description = "Fired in presentation when the effective gameplay hold changes.",
			args = { t.Bool } },
		PreSimulation = {
			description = "Fired before simulating WorldRoots. Listeners receive the delta time",
			args = { t.Float },
		},
		PostSimulation = {
			description = "Fired after simulating WorldRoots. Listeners receive the delta time",
			args = { t.Float },
		},
		PreRender = {
			description = "Fired before rendering the scene. Listeners receive the delta time.",
			args = { t.Float },
		},
	},
})
