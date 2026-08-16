#include "gargantuan/classes/RemoteBase.hpp"

#include "gargantuan/classes/RemoteEventBase.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"

namespace gargantuan {
	RemoteBase::RemoteBase() {
		Destroying->Connect([this](std::monostate) {
			if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(GetObjectId());
			Manager = nullptr;
			ManagerLifetime.reset();
		});
	}

	RemoteBase::~RemoteBase() {
		if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(Id);
	}

	bool RemoteBase::BindRemoteManager(
		network::RemoteManager *NewManager, std::optional<network::ConnectionId> NewDefaultServerConnection
	) {
		AssertIsAlive();
		if (!NewManager) return false;
		const auto Object = GetObjectId();
		if (Manager == NewManager && !ManagerLifetime.expired()) {
			DefaultServerConnection = NewDefaultServerConnection;
			return true;
		}
		if (!NewManager->RegisterRemote(Object, GetRemoteKind())) return false;
		if (Manager && !ManagerLifetime.expired()) Manager->UnregisterRemote(Object);
		Manager = NewManager;
		ManagerLifetime = Manager->GetLifetimeToken();
		DefaultServerConnection = NewDefaultServerConnection;
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
}
