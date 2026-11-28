#include "gargantuan/services/ApplicationService.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

namespace gargantuan {
	int ApplicationService::GetState(lua_State *L, Instance *Receiver) {
		auto *VM = ScriptEngine::Get(L);
		const auto *App = VM->GetApplicationPresentation();
		if (App && Receiver->GetDataModel() == VM->DataModel && App->IsCallerCurrent(*VM->DataModel)) lua_pushstring(L, App->GetState());
		else lua_pushnil(L);
		return 1;
	}

	int ApplicationService::Quit(lua_State *L, Instance *Receiver) {
		auto *VM = ScriptEngine::Get(L);
		auto *App = VM->GetApplicationPresentation();
		const bool Authorized = App && Receiver->GetDataModel() == VM->DataModel && App->IsCallerCurrent(*VM->DataModel);
		lua_createtable(L, 0, 3);
		lua_pushboolean(L, Authorized);
		lua_setfield(L, -2, "Ok");
		if (Authorized) {
			App->QuitRequested = true;
			lua_pushboolean(L, true);
			lua_setfield(L, -2, "Value");
		} else {
			lua_pushliteral(L, "NotAuthorized"); lua_setfield(L, -2, "Code");
			lua_pushliteral(L, "The local application owns shutdown."); lua_setfield(L, -2, "Message");
		}
		lua_setreadonly(L, -1, true);
		return 1;
	}
}
