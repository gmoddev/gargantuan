const c, t, p = require("@tools/classes").unpack()

return c({
	name = "ActionMap",
	description = "Maps bounded physical input bindings to semantic gameplay actions.",
	since = "0.1.0",

	signals = {
		ActionBegan = {
			description = "Fired when a digital semantic action becomes active.",
			args = { t.String },
		},
		ActionChanged = {
			description = "Fired when the current scalar or vector value of an action changes.",
			args = { t.String, t.Vector2 },
		},
		ActionEnded = {
			description = "Fired when a digital semantic action becomes inactive.",
			args = { t.String },
		},
	},

	methods = {
		BindKey = {
			description = "Adds a keyboard binding and returns its runtime binding identity.",
			params = {
				p("ActionName", t.String),
				p("KeyCode", t.EnumItem("KeyCode")),
				p("Scale", t.Float),
				p("Priority", t.Integer),
				p("Consume", t.Bool),
			},
			returns = t.Integer,
		},
		BindMouseButton = {
			description = "Adds a mouse-button binding and returns its runtime binding identity.",
			params = {
				p("ActionName", t.String),
				p("InputType", t.EnumItem("UserInputType")),
				p("Scale", t.Float),
				p("Priority", t.Integer),
				p("Consume", t.Bool),
			},
			returns = t.Integer,
		},
		BindPointerDelta = {
			description = "Adds a relative-pointer delta binding and returns its runtime binding identity.",
			params = {
				p("ActionName", t.String),
				p("Scale", t.Vector2),
				p("Priority", t.Integer),
				p("Consume", t.Bool),
			},
			returns = t.Integer,
		},
		Unbind = {
			description = "Removes one binding by runtime identity.",
			params = { p("BindingId", t.Integer) },
			returns = t.Bool,
		},
		UnbindAction = {
			description = "Removes every binding for one semantic action.",
			params = { p("ActionName", t.String) },
		},
		IsDown = {
			description = "Returns whether any digital binding currently activates the action.",
			params = { p("ActionName", t.String) },
			returns = t.Bool,
		},
		GetValue = {
			description = "Returns the clamped scalar value of a digital action.",
			params = { p("ActionName", t.String) },
			returns = t.Float,
		},
		GetVector = {
			description = "Returns the frame-accumulated vector value of an action.",
			params = { p("ActionName", t.String) },
			returns = t.Vector2,
		},
		GetBindingCount = {
			description = "Returns the current bounded binding count.",
			returns = t.Integer,
		},
	},
})
