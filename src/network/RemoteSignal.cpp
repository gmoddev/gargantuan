#include "gargantuan/network/RemoteSignal.hpp"

#include "gargantuan/network/RemoteLuau.hpp"

namespace gargantuan::network {
	RemoteSignal::RemoteSignal(bool IncludePeerContext) : IncludePeerContext(IncludePeerContext) {}

	Enums::SignalType RemoteSignal::GetSignalType() {
		return Enums::SignalType::Engine;
	}

	int RemoteSignal::LPushArgument(lua_State *L, std::any Value) {
		const auto *Payload = std::any_cast<RemoteSignalPayload>(&Value);
		if (!Payload) return 0;
		int Pushed = 0;
		if (IncludePeerContext) {
			PushRemotePeerContext(L, Payload->Peer);
			++Pushed;
		}
		return Pushed + PushRemoteLuauArguments(L, Payload->Arguments, Payload->ResolveObject);
	}
}
