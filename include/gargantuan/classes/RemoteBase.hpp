#pragma once

#include "gargantuan/classes/generated/RemoteBase.hpp"
#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"

#include <utility>

namespace gargantuan::network {
	class RemoteManager;
	enum class RemoteManagerRole : std::uint8_t;
}

namespace gargantuan {
	class RemoteBase : public Instance {
		I_RemoteBase;

	  public:
		RemoteBase();
		~RemoteBase() override;
		bool BindRemoteManager(
			network::RemoteManager *Manager,
			std::optional<network::ConnectionId> DefaultServerConnection = std::nullopt,
			std::optional<ObjectId> NetworkObject = std::nullopt
		);
		[[nodiscard]] network::RemoteManager *GetRemoteManager() const;
		[[nodiscard]] std::optional<network::ConnectionId> GetDefaultServerConnection() const;
		[[nodiscard]] ObjectId GetNetworkObjectId() const;
		[[nodiscard]] virtual network::RemoteInstanceKind GetRemoteKind() const = 0;

	  protected:
		// Returns the native connection and the first argument after the target.
		[[nodiscard]] std::pair<network::ConnectionId, int> ReadClientTarget(lua_State *L) const;
		network::RemoteManager *Manager = nullptr;
		std::weak_ptr<void> ManagerLifetime;
		std::optional<network::ConnectionId> DefaultServerConnection;
		ObjectId NetworkObject;
	};
}
