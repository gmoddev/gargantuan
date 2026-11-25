#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <lualib.h>

namespace gargantuan {
	namespace {
		char InstanceCacheKey;
	}

	int StackValue<std::shared_ptr<Instance>>::Push(lua_State *L, std::shared_ptr<Instance> Value) {
		if (!Value) {
			lua_pushnil(L);
			return 1;
		}
		// A live wrapper owns this exact allocation. Its address cannot be reused
		// until the wrapper is unreachable. This private lookup must not publish
		// an ObjectId just to expose a detached Instance to Luau.
		Instance *Lookup = Value.get();
		luaL_checkstack(L, 3, "Instance wrapper");
		lua_rawgetp(L, LUA_REGISTRYINDEX, &InstanceCacheKey);
		lua_rawgetp(L, -1, Lookup);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			StackValue<Instance::Self>::Push(L, std::move(Value));
			lua_pushvalue(L, -1);
			lua_rawsetp(L, -3, Lookup);
		}
		lua_remove(L, -2);
		return 1;
	}

	int LibInstance_new(lua_State *L) {
		auto className = luaL_checkstring(L, 1);

		auto classDefinition = InstanceClassRegistry::GetDefinitionByName(className);
		if (!classDefinition) {
			luaL_error(L, "Unknown instance class named %s", className);
			return 0;
		}

		if (!InstanceClassRegistry::IsConstructible(*classDefinition)) {
			luaL_error(L, "Instance class %s cannot be constructed", className);
			return 0;
		}

		auto *ScriptEngineValue = ScriptEngine::Get(L);
		if (classDefinition->ClassName == "DataModel" ||
			(ScriptEngineValue->DataModel && ScriptEngineValue->DataModel->IsProtectedServiceClass(classDefinition->Id))) {
			luaL_error(L, "Instance class %s is a protected service and cannot be constructed", className);
			return 0;
		}

		auto instance = InstanceClassRegistry::Construct(*classDefinition);
		StackValue<std::shared_ptr<Instance>>::Push(L, instance);
		return 1;
	};

	luaL_Reg LibInstance[] = {
		{"new", LibInstance_new},
		{nullptr, nullptr},
	};

	int OpenLibInstance(lua_State *L) {
		lua_newtable(L);
		lua_newtable(L);
		lua_pushliteral(L, "vs");
		lua_setfield(L, -2, "__mode");
		lua_setmetatable(L, -2);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &InstanceCacheKey);
		luaL_register(L, "Instance", LibInstance);
		return 0;
	}
} // namespace gargantuan
