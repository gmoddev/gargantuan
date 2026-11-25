#pragma once

#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/network/RemoteManager.hpp"

namespace gargantuan::network {
	struct RemoteSignalPayload {
		RemotePeerContext Peer;
		std::shared_ptr<const std::vector<RemoteValue>> Arguments;
		RemoteManager::ObjectResolver ResolveObject;
		std::shared_ptr<Player> Sender;
	};

	enum class RemoteSignalSender { None, Peer, Player };

	class RemoteSignal final : public BaseSignal {
	  public:
		explicit RemoteSignal(bool IncludePeerContext);
		explicit RemoteSignal(RemoteSignalSender Sender);
		Enums::SignalType GetSignalType() override;
		int LPushArgument(lua_State *L, std::any Value) override;

	  private:
		RemoteSignalSender Sender;
	};
}
