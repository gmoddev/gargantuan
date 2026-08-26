#pragma once

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/Enums.hpp"
#include "gargantuan/scripting/Userdata.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace gargantuan {
	G_ENUM(RaycastFilterType, Exclude, Include);

	struct RaycastFilterRoots {
		std::vector<std::weak_ptr<Instance>> Values;
	};

	template <> struct StackValue<RaycastFilterRoots> {
		static inline std::string_view ReflectedTypedef() {
			return "{ Instance }";
		}
		static bool Is(lua_State *L, int Index);
		static RaycastFilterRoots From(lua_State *L, int Index);
		static int Push(lua_State *L, RaycastFilterRoots Value);
	};

	G_USERDATA_DECL(
		RaycastParams, static constexpr std::size_t MaximumFilterRoots = 128;

		Enums::RaycastFilterType FilterType = Enums::RaycastFilterType::Exclude;
		RaycastFilterRoots FilterDescendantsInstances;
	)
}
