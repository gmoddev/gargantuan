-- One closed literal declaration feeds both native initialization and metadata.
local Overrides = {}

local function Fail(Message)
	error("[Classgen:DefaultOverrides] " .. Message, 0)
end

local function Number(Value)
	if type(Value) ~= "number" or Value ~= Value or math.abs(Value) > 3.402823466e38 then
		Fail("expected a finite float literal")
	end
	return string.format("%.17e", Value) .. "f"
end

local function Expression(Property, Value)
	local Type = Property.type
	if Type.editorDataType == "Bool" then
		if type(Value) ~= "boolean" then Fail("expected a boolean literal") end
		return tostring(Value)
	elseif Type.wireType == "Float" then
		local Literal = Number(Value)
		if Property.range and (Property.range.minimum and Value < Property.range.minimum
			or Property.range.maximum and Value > Property.range.maximum) then Fail("default outside property range") end
		return Literal
	elseif Type.editorDataType == "Color3" then
		if type(Value) ~= "table" or #Value ~= 3 then Fail("expected three Color3 components") end
		for Key in Value do
			if type(Key) ~= "number" or Key % 1 ~= 0 or Key < 1 or Key > 3 then Fail("malformed Color3 literal") end
		end
		return `gargantuan::Color3({Number(Value[1])}, {Number(Value[2])}, {Number(Value[3])})`
	elseif Type.editorDataType == "NativeEnum" then
		if type(Value) ~= "string" or not string.match(Value, "^[A-Za-z_][A-Za-z0-9_]*$") then
			Fail("expected a native enum item identifier")
		end
		-- C++ compilation checks the item belongs to exactly this enum type.
		return `{Type.cType}::{Value}`
	end
	Fail("override literal type is not supported: " .. tostring(Type.editorDataType))
end

function Overrides.Resolve(Class, FindClass)
	local Entries = Class.defaultOverrides or {}
	if type(Entries) ~= "table" or #Entries > 64 then Fail("expected at most 64 override records") end
	for Key in Entries do
		if type(Key) ~= "number" or Key % 1 ~= 0 or Key < 1 or Key > #Entries then Fail("expected a dense override list") end
	end
	local Seen, Result = {}, {}
	for _, Entry in Entries do
		if type(Entry) ~= "table" or type(Entry.Property) ~= "string" or Entry.Value == nil then Fail("malformed override record") end
		for Key in Entry do
			if Key ~= "Property" and Key ~= "Value" then Fail("override cannot change property metadata") end
		end
		if Seen[Entry.Property] then Fail("duplicate override: " .. Entry.Property) end
		Seen[Entry.Property] = true
		if Class.properties and Class.properties[Entry.Property] then Fail("override must name an inherited property") end
		local BaseName = if Class.root then nil else Class.extends or "Instance"
		local Owner, Property, Visited = nil, nil, {}
		while BaseName do
			if Visited[BaseName] then Fail("cyclic base chain") end
			Visited[BaseName] = true
			local Base = FindClass(BaseName)
			if not Base then Fail("unknown base: " .. BaseName) end
			Property = Base.properties and Base.properties[Entry.Property]
			if Property then Owner = Base; break end
			BaseName = if Base.root then nil else Base.extends or "Instance"
		end
		if not Property then Fail("unknown inherited property: " .. Entry.Property) end
		if not Property.serializable or Property.writable == false or Property.editable == false
			or Property.writeLevel == "Never" or Property.overrideRead or Property.overrideWrite then
			Fail("override requires a persistent generated writable property")
		end
		table.insert(Result, { Property = Entry.Property, DeclaringClass = Owner.name,
			NativeType = Property.type.cType, Expression = Expression(Property, Entry.Value) })
	end
	table.sort(Result, function(A, B) return A.Property < B.Property end)
	return Result
end

return table.freeze(Overrides)
