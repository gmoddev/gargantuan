#pragma once

#include "gargantuan/reflection/Enums.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace gargantuan {
	inline void RequireInputMutation() {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
			throw std::runtime_error("[Input:Authoring] Construction requires MutateDataModel");
	}

	inline void ValidateInputName(std::string_view Name) {
		ValidateProtocolString(Name, 64, "Input definition name");
		if (Name.empty()) throw std::invalid_argument("[Input:Authoring] Name cannot be empty");
	}

	inline void ValidateInputOptions(lua_State *L, int Index, std::initializer_list<std::string_view> Names) {
		if (lua_isnoneornil(L, Index)) return;
		if (!lua_istable(L, Index)) throw std::invalid_argument("[Input:Authoring] Options must be a table");
		const auto Table = lua_absindex(L, Index);
		lua_pushnil(L);
		while (lua_next(L, Table)) {
			if (lua_type(L, -2) != LUA_TSTRING) {
				lua_pop(L, 2);
				throw std::invalid_argument("[Input:Authoring] Option names must be strings");
			}
			std::size_t Length = 0;
			const auto *Text = lua_tolstring(L, -2, &Length);
			const std::string_view Name(Text, Length);
			if (std::find(Names.begin(), Names.end(), Name) == Names.end()) {
				lua_pop(L, 2);
				throw std::invalid_argument("[Input:Authoring] Unknown option");
			}
			lua_pop(L, 1);
		}
	}

	template <typename T> T ReadInputValue(lua_State *L, int Index) {
		if constexpr (std::is_enum_v<T>) {
			if (!StackValue<EnumItem>::Is(L, Index))
				throw std::invalid_argument("[Input:Authoring] Expected enum item");
			const auto Item = StackValue<EnumItem>::From(L, Index);
			if (!Item.EnumType || Item.EnumType->Name != magic_enum::enum_type_name<T>() ||
				!Item.EnumType->FromValue(Item.Value))
				throw std::invalid_argument("[Input:Authoring] Enum type or value is invalid");
			return static_cast<T>(Item.Value);
		} else {
			if constexpr (std::is_same_v<T, std::string>) {
				if (lua_type(L, Index) != LUA_TSTRING) throw std::invalid_argument("[Input:Authoring] Expected string");
			}
			if constexpr (std::is_same_v<T, int>) {
				if (lua_type(L, Index) != LUA_TNUMBER)
					throw std::invalid_argument("[Input:Authoring] Expected integer");
				const auto Number = lua_tonumber(L, Index);
				if (!std::isfinite(Number) || std::trunc(Number) != Number ||
					Number < std::numeric_limits<int>::min() || Number > std::numeric_limits<int>::max())
					throw std::invalid_argument("[Input:Authoring] Integer is out of range");
			} else if constexpr (std::is_same_v<T, float>) {
				if (lua_type(L, Index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, Index)))
					throw std::invalid_argument("[Input:Authoring] Expected finite number");
			}
			if (!StackValue<T>::Is(L, Index))
				throw std::invalid_argument("[Input:Authoring] Option value has incorrect type");
			return StackValue<T>::From(L, Index);
		}
	}

	template <typename T> std::optional<T> ReadInputOption(lua_State *L, int Index, const char *Name) {
		if (lua_isnoneornil(L, Index)) return std::nullopt;
		lua_getfield(L, Index, Name);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			return std::nullopt;
		}
		auto Value = ReadInputValue<T>(L, -1);
		lua_pop(L, 1);
		return Value;
	}
}
