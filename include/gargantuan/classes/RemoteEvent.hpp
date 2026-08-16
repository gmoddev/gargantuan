#pragma once

#include "gargantuan/classes/RemoteEventBase.hpp"
#include "gargantuan/classes/generated/RemoteEvent.hpp"

namespace gargantuan {
	class RemoteEvent final : public RemoteEventBase {
		I_RemoteEvent;

	  public:
		[[nodiscard]] network::RemoteInstanceKind GetRemoteKind() const override {
			return network::RemoteInstanceKind::ReliableEvent;
		}
	};
}
