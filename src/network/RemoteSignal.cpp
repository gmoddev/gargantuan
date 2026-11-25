#include "gargantuan/network/RemoteSignal.hpp"

#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/RemoteLuau.hpp"

namespace gargantuan::network {
	RemoteSignal::RemoteSignal(bool IncludePeerContext)
		: RemoteSignal(IncludePeerContext ? RemoteSignalSender::Peer : RemoteSignalSender::None) {}
	RemoteSignal::RemoteSignal(RemoteSignalSender Sender) : Sender(Sender) {}

	Enums::SignalType RemoteSignal::GetSignalType() {
		return Enums::SignalType::Engine;
	}

	int RemoteSignal::LPushArgument(lua_State *L, std::any Value) {
		const auto *Payload = std::any_cast<RemoteSignalPayload>(&Value);
		if (!Payload || !Payload->Arguments) return 0;
		int Pushed = 0;
		if (Sender == RemoteSignalSender::Peer) {
			PushRemotePeerContext(L, Payload->Peer);
			++Pushed;
		} else if (Sender == RemoteSignalSender::Player) {
			Pushed = StackValue<std::shared_ptr<Player>>::Push(L, Payload->Sender);
		}
		return Pushed + PushRemoteLuauArguments(L, *Payload->Arguments, Payload->ResolveObject);
	}
}
