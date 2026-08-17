#include "gargantuan/Log.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <algorithm>
#include <format>
#include <lua.h>
#include <lualib.h>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <string_view>

namespace gargantuan {
	namespace {
		constexpr int MaximumDiagnosticArguments = 64;
		constexpr std::size_t MaximumDiagnosticArgumentBytes = 512;
		constexpr std::size_t MaximumDiagnosticMessageBytes = 2048;
		constexpr std::string_view TruncationMarker = "...<truncated>";

		std::string BoundedText(std::string_view Value, std::size_t MaximumBytes) {
			if (!IsValidProtocolUtf8(Value)) return "<invalid utf-8>";
			if (Value.size() <= MaximumBytes) return std::string(Value);
			const auto PayloadLimit = MaximumBytes > TruncationMarker.size()
				? MaximumBytes - TruncationMarker.size() : std::size_t{0};
			std::size_t End = PayloadLimit;
			while (End > 0 && (static_cast<unsigned char>(Value[End]) & 0xc0u) == 0x80u) --End;
			std::string Result(Value.substr(0, End));
			Result.append(TruncationMarker.substr(0, MaximumBytes - Result.size()));
			return Result;
		}

		std::string FormatDiagnosticArgument(lua_State *L, int Index) {
			switch (lua_type(L, Index)) {
			case LUA_TNIL: return "nil";
			case LUA_TBOOLEAN: return lua_toboolean(L, Index) ? "true" : "false";
			case LUA_TNUMBER: return std::format("{:g}", lua_tonumber(L, Index));
			case LUA_TSTRING: {
				std::size_t Length = 0;
				const auto *Value = lua_tolstring(L, Index, &Length);
				return BoundedText(std::string_view(Value, Length), MaximumDiagnosticArgumentBytes);
			}
			case LUA_TVECTOR: {
				const auto *Value = lua_tovector(L, Index);
				return Value ? std::format("Vector3({}, {}, {})", Value[0], Value[1], Value[2]) : "<Vector3>";
			}
			case LUA_TTABLE: return "<table>";
			case LUA_TFUNCTION: return "<function>";
			case LUA_TTHREAD: return "<thread>";
			case LUA_TLIGHTUSERDATA: return "<lightuserdata>";
			case LUA_TUSERDATA: {
				const auto Tag = lua_userdatatag(L, Index);
				const auto Name = magic_enum::enum_name(static_cast<UserdataTag>(Tag));
				return Name.empty() ? "<userdata>" : std::format("<{}>", Name);
			}
			default: return "<value>";
			}
		}

		std::string FormatDiagnostic(lua_State *L) {
			const auto ArgumentCount = lua_gettop(L);
			const auto FormattedCount = std::min(ArgumentCount, MaximumDiagnosticArguments);
			std::string Result;
			Result.reserve(std::min<std::size_t>(MaximumDiagnosticMessageBytes, FormattedCount * 16));
			for (int Index = 1; Index <= FormattedCount; ++Index) {
				if (Index > 1) Result.push_back('\t');
				auto Argument = FormatDiagnosticArgument(L, Index);
				if (Result.size() + Argument.size() > MaximumDiagnosticMessageBytes)
					return BoundedText(Result + Argument, MaximumDiagnosticMessageBytes);
				Result += Argument;
			}
			if (ArgumentCount > FormattedCount) {
				constexpr std::string_view Marker = "\t<arguments truncated>";
				return BoundedText(Result + std::string(Marker), MaximumDiagnosticMessageBytes);
			}
			return Result;
		}

		int EmitDiagnostic(lua_State *L, std::string Severity) {
			auto Message = FormatDiagnostic(L);
			if (Severity == "Warning") LOG_WARN(Lua, "[Runtime:Luau] %s", Message.c_str());
			else LOG_INFO(Lua, "[Runtime:Luau] %s", Message.c_str());
			ScriptEngine::Get(L)->EmitRuntimeDiagnostic(std::move(Severity), std::move(Message));
			return 0;
		}

		int RuntimePrint(lua_State *L) {
			return InvokeNativeCallback(L, [L] { return EmitDiagnostic(L, "Information"); });
		}
		int RuntimeWarn(lua_State *L) {
			return InvokeNativeCallback(L, [L] { return EmitDiagnostic(L, "Warning"); });
		}
	}

	int OpenLibBase(lua_State *L) {
		auto scriptEngine = ScriptEngine::Get(L);

		lua_pushcclosurek(L, RuntimePrint, "print", 0, nullptr);
		lua_setglobal(L, "print");

		lua_pushcclosurek(L, RuntimeWarn, "warn", 0, nullptr);
		lua_setglobal(L, "warn");

		StackValue<std::shared_ptr<Instance>>::Push(L, scriptEngine->DataModel);
		lua_setglobal(L, "game");

		lua_createtable(L, 0, 0);
		{
			lua_pushliteral(L, "gargantuan");
			lua_setfield(L, -2, "name");

			lua_pushliteral(L, "https://gargantuan.teamfireworks.org/");
			lua_setfield(L, -2, "url");

			lua_createtable(L, 0, 0);
			{
				lua_pushliteral(L, "0.0.0-indev");
				lua_setfield(L, -2, "display");

				lua_createtable(L, 0, 0);
				{
					lua_pushliteral(L, "https://github.com/teamfireworks/gargantuan.git/");
					lua_setfield(L, -2, "url");
				}
				lua_setreadonly(L, -1, true);
				lua_setfield(L, -2, "git");
			}
			lua_setreadonly(L, -1, true);
			lua_setfield(L, -2, "version");
		}
		lua_setreadonly(L, -1, true);
		lua_setglobal(L, "_RUNTIME");

		return 0;
	}
}
