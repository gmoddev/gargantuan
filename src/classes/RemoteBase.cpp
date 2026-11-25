#include "gargantuan/classes/RemoteBase.hpp"

#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEventBase.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"

namespace gargantuan {
	RemoteBase::RemoteBase() {
		Destroying->Connect([this](std::monostate) {
			if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(GetNetworkObjectId());
			Manager = nullptr;
			ManagerLifetime.reset();
			NetworkObject = {};
		});
	}

	RemoteBase::~RemoteBase() {
		if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(GetNetworkObjectId());
	}

	bool RemoteBase::BindRemoteManager(
		network::RemoteManager *NewManager,
		std::optional<network::ConnectionId> NewDefaultServerConnection,
		std::optional<ObjectId> NewNetworkObject
	) {
		AssertIsAlive();
		if (!NewManager) return false;
		const auto Object = NewNetworkObject.value_or(GetObjectId());
		if (!Object.IsValid()) return false;
		if (Manager == NewManager && !ManagerLifetime.expired() && GetNetworkObjectId() == Object) {
			DefaultServerConnection = NewDefaultServerConnection;
			return true;
		}
		if (!NewManager->RegisterRemote(Object, GetRemoteKind())) return false;
		if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(GetNetworkObjectId());
		Manager = NewManager;
		ManagerLifetime = Manager->GetLifetimeToken();
		DefaultServerConnection = NewDefaultServerConnection;
		NetworkObject = Object;
		if (auto *Event = dynamic_cast<RemoteEventBase *>(this)) Event->BindEventHandler();
		if (auto *Function = dynamic_cast<RemoteFunction *>(this)) Function->BindRequestHandler();
		return true;
	}

	network::RemoteManager *RemoteBase::GetRemoteManager() const {
		return Manager && !ManagerLifetime.expired() ? Manager : nullptr;
	}
	std::optional<network::ConnectionId> RemoteBase::GetDefaultServerConnection() const {
		return DefaultServerConnection;
	}
	ObjectId RemoteBase::GetNetworkObjectId() const {
		return NetworkObject.IsValid() ? NetworkObject : Id;
	}

	std::pair<network::ConnectionId, int> RemoteBase::ReadClientTarget(lua_State *L) const {
		if (StackValue<std::shared_ptr<Instance>>::Is(L, 2)) {
			const auto PlayerValue = StackValue<std::shared_ptr<Player>>::From(L, 2);
			if (!PlayerValue) throw std::invalid_argument("Remote client target must be a Player or peer slot and generation");
			auto *BoundManager = GetRemoteManager();
			const auto Peer = BoundManager ? BoundManager->ResolvePeer(*PlayerValue) : std::nullopt;
			return {Peer ? Peer->Connection : network::ConnectionId{}, 3};
		}
		return {{static_cast<std::uint32_t>(luaL_checkunsigned(L, 2)),
				 static_cast<std::uint32_t>(luaL_checkunsigned(L, 3))}, 4};
	}
}
