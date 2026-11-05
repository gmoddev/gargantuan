const c, t, p = require("@tools/classes").unpack()

return c({
	name = "MeshPart",
	extends = "BasePart",
	description = "A BasePart rendered from a canonical AssetService mesh",
	since = "0.3.0",

	properties = {
		CollisionMode = {
			description = "Box collision or anchored static triangle collision from the Mesh asset. Unanchored parts use Box collision.",
			type = t.EnumItem("MeshCollisionMode"),
			unmodified = "Enums::MeshCollisionMode::Box",
			serializable = true,
			replicated = true,
			category = "Collision",
		},
		Mesh = {
			description = "Strict Mesh asset reference",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetMesh",
			overrideWrite = "SetMesh",
			serializable = true,
			replicated = true,
			category = "Appearance",
			editorHint = "AssetReference:Mesh",
		},
		Material = {
			description = "Optional whole-mesh Material asset override",
			type = t.String,
			unmodified = '""',
			overrideRead = "GetMaterial",
			overrideWrite = "SetMaterial",
			serializable = true,
			replicated = true,
			category = "Appearance",
			editorHint = "AssetReference:Material",
		},
	},
})
