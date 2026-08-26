#include "gargantuan/datatypes/RaycastParams.hpp"

#include "gargantuan/scripting/UserdataTag.hpp"

#include <algorithm>
#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gargantuan {
	bool StackValue<RaycastFilterRoots>::Is(lua_State *L, int Index) {
		return lua_istable(L, Index);
	}

	RaycastFilterRoots StackValue<RaycastFilterRoots>::From(lua_State *L, int Index) {
		luaL_checktype(L, Index, LUA_TTABLE);
		Index = lua_absindex(L, Index);
		std::vector<std::pair<std::size_t, std::shared_ptr<Instance>>> Entries;
		int Iterator = 0;
		while ((Iterator = lua_rawiter(L, Index, Iterator)) != -1) {
			if (Entries.size() == RaycastParams::MaximumFilterRoots) {
				lua_pop(L, 2);
				throw std::invalid_argument("RaycastParams accepts at most 128 filter roots");
			}
			if (lua_type(L, -2) != LUA_TNUMBER) {
				lua_pop(L, 2);
				throw std::invalid_argument("RaycastParams filter roots must use contiguous numeric indices");
			}
			const auto NumericIndex = lua_tonumber(L, -2);
			if (!std::isfinite(NumericIndex) || NumericIndex < 1.0 ||
				NumericIndex > static_cast<double>(RaycastParams::MaximumFilterRoots) ||
				std::floor(NumericIndex) != NumericIndex) {
				lua_pop(L, 2);
				throw std::invalid_argument("RaycastParams filter root index is outside the bounded array");
			}
			const auto EntryIndex = static_cast<std::size_t>(NumericIndex);
			if (!StackValue<std::shared_ptr<Instance>>::Is(L, -1)) {
				lua_pop(L, 2);
				throw std::invalid_argument("RaycastParams filter roots must be non-nil Instances");
			}
			auto Value = StackValue<std::shared_ptr<Instance>>::From(L, -1);
			lua_pop(L, 2);
			if (!Value || Value->GetDestroyed() || Value->IsDestroying())
				throw std::invalid_argument("RaycastParams filter roots must be live Instances");
			Entries.emplace_back(EntryIndex, std::move(Value));
		}
		std::ranges::sort(Entries, [](const auto &Left, const auto &Right) { return Left.first < Right.first; });
		RaycastFilterRoots Result;
		Result.Values.reserve(Entries.size());
		std::unordered_set<ObjectId> Seen;
		for (std::size_t Offset = 0; Offset < Entries.size(); ++Offset) {
			if (Entries[Offset].first != Offset + 1)
				throw std::invalid_argument("RaycastParams filter roots must use contiguous numeric indices");
			auto &Value = Entries[Offset].second;
			if (Seen.insert(Value->GetObjectId()).second) Result.Values.emplace_back(Value);
		}
		return Result;
	}

	int StackValue<RaycastFilterRoots>::Push(lua_State *L, RaycastFilterRoots Value) {
		lua_createtable(L, static_cast<int>(Value.Values.size()), 0);
		int Output = lua_gettop(L);
		int Index = 1;
		for (const auto &WeakValue : Value.Values) {
			auto InstanceValue = WeakValue.lock();
			if (!InstanceValue || InstanceValue->GetDestroyed() || InstanceValue->IsDestroying()) continue;
			StackValue<std::shared_ptr<Instance>>::Push(L, std::move(InstanceValue));
			lua_rawseti(L, Output, Index++);
		}
		return 1;
	}

	G_USERDATA_IMPL(
		RaycastParams,
		.Tag = UserdataTag::RaycastParams,
		.Type = "RaycastParams",
		.Properties = {
			{"FilterType",
			 Property::fromReadWrite<EnumItem>(
				 [](RaycastParams *Params) {
					 auto EnumType = Enum::fromType<Enums::RaycastFilterType>();
					 return *EnumType->FromValue(static_cast<int>(Params->FilterType));
				 },
				 [](RaycastParams *Params, EnumItem Value) {
					 if (!Value.EnumType || Value.EnumType->Name != "RaycastFilterType")
						 throw std::invalid_argument("RaycastParams.FilterType requires Enum.RaycastFilterType");
					 Params->FilterType = static_cast<Enums::RaycastFilterType>(Value.Value);
				 }
			 )},
			{"FilterDescendantsInstances", Property::fromReadWriteMember<&RaycastParams::FilterDescendantsInstances>()},
		}
	);
}
