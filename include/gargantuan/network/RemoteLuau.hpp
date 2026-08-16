#pragma once

#include "gargantuan/network/RemoteManager.hpp"

#include <lua.h>

namespace gargantuan::network {
	[[nodiscard]] std::vector<WireValue> ReadRemoteLuauArguments(lua_State *L, int FirstIndex);
	void ValidateRemoteLuauArguments(
		std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	[[nodiscard]] RemoteManager::ObjectResolver StabilizeRemoteLuauObjectReferences(
		std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	int PushRemoteLuauArguments(
		lua_State *L, std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	);
	void PushRemotePeerContext(lua_State *L, const RemotePeerContext &Peer);
}
