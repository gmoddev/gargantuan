const c, t = require("@tools/classes").unpack()

return c({
	name = "AttachmentMesh",
	extends = "Attachment",
	description = "Rigid visual-only mesh at an Attachment's semantic transform; it has no physics body.",
	since = "0.6.0",
	properties = {
		Mesh = {
			description = "Strict visual Mesh asset reference.", type = t.String, unmodified = '""',
			overrideRead = "GetMesh", overrideWrite = "SetMesh",
			serializable = true, replicated = true, category = "Appearance", editorHint = "AssetReference:Mesh",
		},
		Material = {
			description = "Optional whole-mesh Material asset override.", type = t.String, unmodified = '""',
			overrideRead = "GetMaterial", overrideWrite = "SetMaterial",
			serializable = true, replicated = true, category = "Appearance", editorHint = "AssetReference:Material",
		},
		Size = {
			description = "Positive local mesh scale at the semantic Attachment matrix.",
			type = t.Vector3, unmodified = "glm::vec3(1.0f)",
			overrideRead = "GetSize", overrideWrite = "SetSize",
			serializable = true, replicated = true, category = "Transform",
		},
		Color = {
			description = "Linear visual color multiplier.", type = t.Color3,
			unmodified = "gargantuan::Color3(1.0f, 1.0f, 1.0f)",
			overrideRead = "GetColor", overrideWrite = "SetColor",
			serializable = true, replicated = true, category = "Appearance",
			range = { minimum = 0, maximum = 1 }, editorHint = "ColorComponents",
		},
		Transparency = {
			description = "Visual transparency from zero through one.", type = t.Float,
			unmodified = "0.0f", overrideRead = "GetTransparency", overrideWrite = "SetTransparency",
			serializable = true, replicated = true, category = "Appearance", range = { minimum = 0, maximum = 1 },
		},
		Visible = {
			description = "Whether this visual is published; variants may retain hidden authored siblings.",
			type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Appearance",
		},
		CastShadow = {
			description = "Whether this visual casts a shadow.",
			type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Appearance",
		},
	},
})
