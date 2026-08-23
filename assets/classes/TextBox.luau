local c, t, p = require("@tools/classes").unpack()

return c({
	name = "TextBox",
	extends = "TextLabel",
	description = "A bounded UTF-8 editable text control with code-point-safe caret and selection state",
	since = "0.3.0",

	properties = {
		PlaceholderText = {
			description = "Text shown when the editable value is empty",
			type = t.String,
			unmodified = '""',
			serializable = true,
			replicated = true,
		},
		PlaceholderColor3 = {
			description = "Color used for placeholder text",
			type = t.Color3,
			unmodified = "gargantuan::Color3(0.55f, 0.55f, 0.55f)",
			serializable = true,
			replicated = true,
		},
		ReadOnly = {
			description = "Whether editing operations are rejected while selection and copying remain possible",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		MultiLine = {
			description = "Whether Return inserts a newline instead of submitting",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		SecureTextEntry = {
			description = "Whether presentation and accessibility mask the editable value",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		MaxLength = {
			description = "Maximum editable Unicode code points, capped by the runtime hard limit",
			type = t.Integer,
			unmodified = "4096",
			serializable = true,
			replicated = true,
			range = { minimum = 0, maximum = 4096 },
		},
		CaretPosition = {
			description = "Zero-based Unicode code-point boundary for the runtime caret",
			type = t.Integer,
			unmodified = "0",
			writeLevel = "Never",
		},
		SelectionStart = {
			description = "Zero-based lower Unicode code-point boundary of the runtime selection",
			type = t.Integer,
			unmodified = "0",
			writeLevel = "Never",
		},
		SelectionLength = {
			description = "Unicode code-point length of the runtime selection",
			type = t.Integer,
			unmodified = "0",
			writeLevel = "Never",
		},
		CompositionText = {
			description = "Bounded transient IME preedit text",
			type = t.String,
			unmodified = '""',
			writeLevel = "Never",
		},
	},

	signals = {
		Submitted = {
			description = "Fired when Return submits a focused single-line field",
			args = { t.String },
		},
	},
})
