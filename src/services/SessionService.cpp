#include "gargantuan/services/SessionService.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"

namespace gargantuan {
	int SessionService::Host(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Host);
	}
	int SessionService::FindServers(lua_State *L, Instance *Receiver) { return ApplicationSession::FindServers(L, Receiver); }
	int SessionService::Reconnect(lua_State *L, Instance *Receiver) { return ApplicationSession::RequestReconnect(L, Receiver); }
	int SessionService::GetSession(lua_State *L, Instance *Receiver) { return ApplicationSession::GetInfo(L, Receiver); }
	int SessionService::StartLocal(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::StartLocal);
	}
	int SessionService::Join(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Join);
	}
	int SessionService::Leave(lua_State *L, Instance *Receiver) {
		return ApplicationSession::Request(L, Receiver, SessionCommand::Leave);
	}
}
