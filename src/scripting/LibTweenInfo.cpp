#include "gargantuan/datatypes/TweenInfo.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/StackValue.hpp"
#include <cmath>
#include <stdexcept>
#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	namespace {
		template <typename E>
		E ReadTweenEnum(lua_State *L, int Index, E Default) {
			if (lua_isnoneornil(L, Index)) return Default;
			const auto Item = CheckStackValue<EnumItem>(L, Index);
			if (!Item.EnumType || Item.EnumType->Name != magic_enum::enum_type_name<E>() ||
				!Item.EnumType->FromValue(Item.Value))
				throw std::invalid_argument("[Tween:Info] Wrong easing enum type");
			return static_cast<E>(Item.Value);
		}
	}

	int LibTweenInfo_new(lua_State *L) {
		return InvokeNativeCallback(L, [L] {
			const double Time = luaL_optnumber(L, 1, 1.0);
			const auto Style = ReadTweenEnum(L, 2, Enums::EasingStyle::Quad);
			const auto Direction = ReadTweenEnum(L, 3, Enums::EasingDirection::Out);
			const double RepeatCount = luaL_optnumber(L, 4, 0.0);
			const bool Reverses = luaL_optboolean(L, 5, false);
			const double DelayTime = luaL_optnumber(L, 6, 0.0);
			if (!std::isfinite(Time) || Time < 0.0 || Time > 86'400.0 ||
				!std::isfinite(DelayTime) || DelayTime < 0.0 || DelayTime > 86'400.0 ||
				!std::isfinite(RepeatCount) || RepeatCount < 0.0 || RepeatCount > 1'000.0 ||
				std::trunc(RepeatCount) != RepeatCount)
				throw std::invalid_argument("[Tween:Info] Time and delay must be in [0, 86400], repeat count an integer in [0, 1000]");
			const TweenInfo Info(static_cast<float>(Time), Style, Direction, static_cast<int32_t>(RepeatCount),
				Reverses, static_cast<float>(DelayTime));
			return StackValue<TweenInfo>::Push(L, Info);
		});
	}

	luaL_Reg LibTweenInfo[]{{"new", LibTweenInfo_new}, {nullptr, nullptr}};

	int OpenLibTweenInfo(lua_State *L) {
		luaL_register(L, "TweenInfo", LibTweenInfo);
		return 0;
    }
}
