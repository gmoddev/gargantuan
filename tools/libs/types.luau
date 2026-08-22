local tableext = require("@std/tableext")
export type Type = {
	read kind: "c" | "class" | "datatype" | "enum" | "optional",
	read cType: string,
	read cIncludes: { string }?,
	read cForwardStatement: string?,
	read luauType: string,
	read editorDataType: string?,
	read wireType: string?,
	read compoundType: string?,
	read enumName: string?,
	read objectClassName: string?,
	read nullable: boolean?,
	read optionalValue: Type?,
}

const function type(x: Type): Type
	return x
end

const function literal<T>(x: T | ""): T
	return x
end


const EDITOR_DATATYPES = table.freeze({
	CFrame = true,
	Color3 = true,
	UDim = true,
	UDim2 = true,
	Vector2 = true,
	Vector3 = true,
})

const function datatype(typename: string, cType: string?): Type
	const editorDataType = if EDITOR_DATATYPES[typename] then typename else nil
	return {
		kind = literal("datatype"),
		cType = cType or "gargantuan::" .. typename,
		cIncludes = { `"gargantuan/datatypes/{typename}.hpp"` },
		luauType = typename,
		editorDataType = editorDataType,
		wireType = editorDataType,
		compoundType = editorDataType,
	}
end

const types = {
	Bool = type({ kind = literal("c"), cType = "bool", luauType = "boolean", editorDataType = "Bool", wireType = "Bool" }),
	Double = type({ kind = literal("c"), cType = "double", luauType = "number", editorDataType = "Number", wireType = "Double" }),
	Float = type({ kind = literal("c"), cType = "float", luauType = "number", editorDataType = "Number", wireType = "Float" }),
	Integer = type({ kind = literal("c"), cType = "int", luauType = "number", editorDataType = "Integer", wireType = "Int" }),
	String = type({ kind = literal("c"), cType = "std::string", cIncludes = { "<string>" }, luauType = "string", editorDataType = "String", wireType = "String" }),
	StringPointer = type({ kind = literal("c"), cType = "const char *", luauType = "string", editorDataType = "String", wireType = "String" }),
	StringView = type({
		kind = literal("c"),
		cType = "std::string_view",
		cIncludes = { "<string_view>" },
		luauType = "string",
		editorDataType = "String",
		wireType = "String",
	}),

	Axes = datatype("Axes"),
	CFrame = datatype("CFrame"),
	Color3 = datatype("Color3"),
	Font = datatype("Font"),
	Random = datatype("Random"),
	Rect = datatype("Rect"),
	TweenInfo = datatype("TweenInfo"),
	UDim = datatype("UDim"),
	UDim2 = datatype("UDim2"),
	Vector2 = datatype("Vector2"),
	Vector3 = datatype("Vector3", "glm::vec3"),
	RemoteSignal = type({
		kind = literal("datatype"),
		cType = "std::shared_ptr<gargantuan::BaseSignal>",
		cIncludes = { "<memory>", '"gargantuan/datatypes/Signal.hpp"' },
		luauType = "Signal<any...>",
	}),
}

function types.Instance(className: string?): Type
	className = className or "Instance"

	return {
		kind = "class",
		cType = `std::shared_ptr<gargantuan::{className}>`,
		cIncludes = { `"gargantuan/classes/{className}.hpp"` },
		cForwardStatement = `class {className};`,
		luauType = className,
		editorDataType = "ObjectReference",
		wireType = "ObjectReference",
		objectClassName = className,
		nullable = true,
	}
end

function types.Signal(...: Type): Type
	const numArgs = select("#", ...)
	if numArgs == 0 then
		return {
			kind = "datatype",
			cType = "std::shared_ptr<gargantuan::Signal<std::monostate>>",
			cIncludes = { "<variant>", "<memory>", '"gargantuan/datatypes/Signal.hpp"' },
			luauType = "Signal<>",
		}
	elseif numArgs == 1 then
		const type: Type = select(1, ...)
		const cIncludes = { "<memory>", '"gargantuan/datatypes/Signal.hpp"' }
		if type.cIncludes then
			tableext.extend(cIncludes, type.cIncludes)
		end

		return {
			kind = "datatype",
			cType = `std::shared_ptr<gargantuan::Signal<{type.cType}>>`,
			cIncludes = { "<memory>", '"gargantuan/datatypes/Signal.hpp"' },
			luauType = `Signal<{type.luauType}>`,
		}
	else
		const cIncludes = { "<tuple>", "<memory>", '"gargantuan/datatypes/Signal.hpp"' }
		const cArgs, luauArgs = table.create(numArgs), table.create(numArgs)
		for index = 1, numArgs do
			const type: Type = select(index, ...)
			if type.cIncludes then
				tableext.extend(cIncludes, type.cIncludes)
			end
			cArgs[index] = type.cType
			luauArgs[index] = type.luauType
		end

		return {
			kind = "datatype",
			cType = `std::shared_ptr<Signal<std::tuple<{table.concat(cArgs, ", ")}>>>`,
			cIncludes = cIncludes,
			luauType = `Signal<{table.concat(luauArgs, ", ")}>`,
		}
	end
end

function types.EnumItem(enumName: string?): Type
	if not enumName then
		return {
			kind = "enum",
			cType = "gargantuan::EnumItem",
			cIncludes = { `"gargantuan/reflection/Enums.hpp"` },
			cForwardStatement = `class EnumItem;`,
			luauType = "EnumItem",
		}
	else
		return {
			kind = "enum",
			cType = `gargantuan::Enums::{enumName}`,
			cIncludes = { `"gargantuan/reflection/Enums.hpp"` },
			cForwardStatement = `namespace Enums \{ enum {enumName}; };`,
			luauType = `Enum.{enumName}`,
			editorDataType = "NativeEnum",
			wireType = "EnumItem",
			enumName = enumName,
		}
	end
end

function types.Optional(base: Type): Type
	local cIncludes = base.cIncludes or {} :: never
	table.insert(cIncludes, "<optional>")

	return {
		kind = "optional",
		cType = `std::optional<{base.cType}>`,
		cIncludes = cIncludes,
		luauType = `{base.luauType}?`,
		optionalValue = base,
		editorDataType = base.editorDataType,
		wireType = base.wireType,
		compoundType = base.compoundType,
		enumName = base.enumName,
		objectClassName = base.objectClassName,
		nullable = true,
	}
end

function types.Array(base: Type): Type
	local cIncludes = base.cIncludes or {} :: never
	table.insert(cIncludes, "<vector>")

	return {
		kind = "c",
		cType = `std::vector<{base.cType}>`,
		cIncludes = cIncludes,
		luauType = `\{ {base.luauType} }`,
	}
end

function types.Function(...: { name: string, type: Type })
	const cIncludes = { "<functional>" }

	const numParams = select("#", ...)
	local cParams, luauParams = "", ""
	if numParams ~= 0 then
		for index = 1, numParams do
			const param: { name: string, type: Type } = select(index, ...)
			cParams ..= param.type.cType .. " " .. param.name
			luauParams ..= param.name .. ": " .. param.type.luauType

			if param.type.cIncludes then
				tableext.extend(cIncludes, param.type.cIncludes)
			end

			if index ~= numParams then
				cParams ..= ", "
				luauParams ..= ", "
			end
		end
	end

	return function(...: Type): Type
		const numReturns = select("#")
		local cReturns: string, luauReturns: string

		if numReturns == 0 then
			cReturns, luauReturns = "void", "()"
		elseif numReturns == 1 then
			const ret = select(1, ...)
			cReturns, luauReturns = ret.cType, ret.luauType
			if ret.cIncludes then
				tableext.extend(cIncludes, ret.cIncludes)
			end
		else
			table.insert(cIncludes, "<tuple>")
			cReturns, luauReturns = "std::tuple<", "("
			for index = 1, numReturns do
				const ret: Type = select(index, ...)
				cReturns ..= ret.cType
				luauReturns ..= ret.luauType

				if ret.cIncludes then
					tableext.extend(cIncludes, ret.cIncludes)
				end

				if index ~= numReturns then
					cReturns ..= ", "
					luauReturns ..= ", "
				end
			end
		end

		return {
			kind = "c",
			cType = `std::function<{cReturns}({cParams})>`,
			cIncludes = cIncludes,
			luauType = `({luauParams}) -> {luauReturns}`,
		}
	end
end

return table.freeze(types)
