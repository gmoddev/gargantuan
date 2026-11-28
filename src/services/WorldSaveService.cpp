#include "gargantuan/services/WorldSaveService.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
namespace gargantuan {
	int WorldSaveService::GetCurrentWorld(lua_State *L, Instance *Receiver) {
		auto &Service = static_cast<WorldSaveService &>(*Receiver);
		const auto &Context = GetCurrentScriptSecurityContext();
		const bool Authority = Context.Domain == ScriptExecutionDomain::Server && Service.GetDataModel() == ScriptEngine::Get(L)->DataModel;
		const auto *Slot = Authority && Service.Selected ? &*Service.Selected : nullptr;
		lua_createtable(L, 0, 3);
		lua_pushboolean(L, Slot != nullptr);
		lua_setfield(L, -2, "Ok");
		if (Slot) {
			PushWorldSlot(L, *Slot);
			lua_setfield(L, -2, "Value");
		} else {
			lua_pushstring(L, Authority ? "NoSession" : "NotAuthorized");
			lua_setfield(L, -2, "Code");
			lua_pushliteral(L, "World metadata requires a selected authoritative world.");
			lua_setfield(L, -2, "Message");
		}
		lua_setreadonly(L, -1, true);
		return 1;
	}
	int WorldSaveService::List(lua_State *L, Instance *Receiver) {
		return ApplicationSession::RequestCatalog(L, Receiver, WorldCatalogAction::List);
	}
	int WorldSaveService::Create(lua_State *L, Instance *Receiver) {
		return ApplicationSession::RequestCatalog(L, Receiver, WorldCatalogAction::Create);
	}
	int WorldSaveService::ResolveCreate(lua_State *L, Instance *Receiver) {
		return ApplicationSession::RequestCatalog(L, Receiver, WorldCatalogAction::Resolve);
	}
}
