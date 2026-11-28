#include "gargantuan/services/SessionService.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"

namespace gargantuan {
	int SessionService::GetSession(lua_State *L, Instance *Receiver) { return ApplicationSession::GetInfo(L, Receiver); }
	int SessionService::StartLocal(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::StartLocal);
	}
	int SessionService::Leave(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Leave);
	}
}
