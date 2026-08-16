#pragma once

#include "gargantuan/classes/RemoteEventBase.hpp"
#include "gargantuan/classes/generated/UnreliableRemoteEvent.hpp"

namespace gargantuan {
	class UnreliableRemoteEvent final : public RemoteEventBase {
		I_UnreliableRemoteEvent;

	  public:
		[[nodiscard]] network::RemoteInstanceKind GetRemoteKind() const override {
			return network::RemoteInstanceKind::UnreliableEvent;
		}
	};
}
