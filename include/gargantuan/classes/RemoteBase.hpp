#pragma once

#include "gargantuan/classes/generated/RemoteBase.hpp"
#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"

namespace gargantuan::network {
	class RemoteManager;
}

namespace gargantuan {
	class RemoteBase : public Instance {
		I_RemoteBase;

	  public:
		RemoteBase();
		~RemoteBase() override;
		bool BindRemoteManager(
			network::RemoteManager *Manager, std::optional<network::ConnectionId> DefaultServerConnection = std::nullopt
		);
		[[nodiscard]] network::RemoteManager *GetRemoteManager() const;
		[[nodiscard]] std::optional<network::ConnectionId> GetDefaultServerConnection() const;
		[[nodiscard]] virtual network::RemoteInstanceKind GetRemoteKind() const = 0;

	  protected:
		network::RemoteManager *Manager = nullptr;
		std::weak_ptr<void> ManagerLifetime;
		std::optional<network::ConnectionId> DefaultServerConnection;
	};
}
