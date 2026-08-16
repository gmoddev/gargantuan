#pragma once

#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/network/RemoteManager.hpp"

namespace gargantuan::network {
	struct RemoteSignalPayload {
		RemotePeerContext Peer;
		std::vector<WireValue> Arguments;
		RemoteManager::ObjectResolver ResolveObject;
	};

	class RemoteSignal final : public BaseSignal {
	  public:
		explicit RemoteSignal(bool IncludePeerContext);
		Enums::SignalType GetSignalType() override;
		int LPushArgument(lua_State *L, std::any Value) override;

	  private:
		bool IncludePeerContext;
	};
}
