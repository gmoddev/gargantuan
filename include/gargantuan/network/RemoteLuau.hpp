#pragma once

#include "gargantuan/network/RemoteManager.hpp"

#include <lua.h>

namespace gargantuan::network {
	[[nodiscard]] std::vector<RemoteValue>
	ReadRemoteLuauArguments(lua_State *L, int FirstIndex, const RemoteManager *Manager = nullptr);
	void ValidateRemoteLuauArguments(
		std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	[[nodiscard]] RemoteManager::ObjectResolver StabilizeRemoteLuauObjectReferences(
		std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	int PushRemoteLuauArguments(
		lua_State *L, std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	void PushRemotePeerContext(lua_State *L, const RemotePeerContext &Peer);
}
