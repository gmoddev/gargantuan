#include "gargantuan/services/RunService.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

namespace gargantuan {
	int RunService::PauseGameplay(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Pause);
	}
	int RunService::ResumeGameplay(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Resume);
	}
	int RunService::IsGameplayPaused(lua_State *L, Instance *Receiver) {
		auto *VM = ScriptEngine::Get(L);
		auto *App = VM->GetApplicationPresentation();
		lua_pushboolean(L, App && Receiver->GetDataModel() == VM->DataModel && App->IsCallerCurrent(*VM->DataModel) && App->Sessions.IsPaused());
		return 1;
	}
}
