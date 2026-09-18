local Overrides = require("@tools/defaultOverrides")
local Types = require("@tools/types")
local Count = 0
local Root = { name = "Root", root = true, properties = {
	Transparency = { type = Types.Float, serializable = true, range = { minimum = 0, maximum = 1 } },
	Color = { type = Types.Color3, serializable = true },
	Enabled = { type = Types.Bool, serializable = true },
	Sink = { type = Types.EnumItem("InputSink"), serializable = true },
	Transient = { type = Types.Bool },
	ReadOnly = { type = Types.Bool, serializable = true, writeLevel = "Never" },
	Handwritten = { type = Types.Bool, serializable = true, overrideWrite = "SetHandwritten" },
} }
local Base = { name = "Base", extends = "Root" }
local function Resolve(Entries)
	return Overrides.Resolve({ name = "Child", extends = "Base", defaultOverrides = Entries },
		function(Name) return if Name == "Root" then Root elseif Name == "Base" then Base else nil end)
end
local function Reject(Entries)
	local Ok, Error = pcall(Resolve, Entries)
	assert(not Ok and string.find(tostring(Error), "[Classgen:DefaultOverrides]", 1, true), tostring(Error))
	Count += 1
end
assert(#Resolve(nil) == 0)
local Valid = Resolve({ { Property = "Transparency", Value = 1 }, { Property = "Enabled", Value = true },
	{ Property = "Color", Value = { 0.08, 0.10, 0.14 } }, { Property = "Sink", Value = "All" } })
assert(#Valid == 4 and Valid[1].DeclaringClass == "Root")
Reject({ { Property = "Unknown", Value = 0 } })
Reject({ { Property = "Transparency", Value = "1.0f" } })
Reject({ { Property = "Enabled", Value = 1 } })
Reject({ { Property = "Transparency", Value = 0 }, { Property = "Transparency", Value = 1 } })
Reject({ { Property = "Transparency" } })
Reject({ { Property = "Transparency", Value = 0, Replicated = false } })
Reject({ Transparency = 1 })
Reject({ { Property = "Transparency", Value = 0 / 0 } })
Reject({ { Property = "Transparency", Value = math.huge } })
Reject({ { Property = "Transparency", Value = 2 } })
Reject({ { Property = "Color", Value = { 1, 2 } } })
Reject({ { Property = "Color", Value = { 1, 2, "3" } } })
Reject({ { Property = "Color", Value = { 1, 2, 3, Extra = 4 } } })
Reject({ { Property = "Sink", Value = "All; executable()" } })
Reject({ { Property = "Transient", Value = true } })
Reject({ { Property = "ReadOnly", Value = true } })
Reject({ { Property = "Handwritten", Value = true } })
Reject(table.create(65, { Property = "Enabled", Value = true }))
local Own = { name = "Own", root = true, properties = Root.properties,
	defaultOverrides = { { Property = "Enabled", Value = true } } }
assert(not pcall(Overrides.Resolve, Own, function() return nil end))
print(`[Classgen:DefaultOverrides] passed {Count + 4} cases`)
