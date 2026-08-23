const c, t, p = require("@tools/classes").unpack()

return c({
	name = "FileLink",
	description = "Compatibility class that imports a project-relative source directory through SourceMount",
	since = "0.1.0",

	properties = {
		Path = {
			description = "Project-relative path of the source directory to import",
			type = t.String,
			unmodified = '""',
			serializable = true,
			replicated = true,
		},
		IgnoreErrors = {
			description = "Whether to ignore errors when deserializing the directory's contents",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		IgnoreUnknownDescendants = {
			description = "Whether to ignore unknown descendants that don't match the directory's contents",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
	},
})
