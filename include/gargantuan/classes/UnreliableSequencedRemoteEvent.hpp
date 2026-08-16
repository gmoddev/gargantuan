#pragma once

#include "gargantuan/classes/RemoteEventBase.hpp"
#include "gargantuan/classes/generated/UnreliableSequencedRemoteEvent.hpp"

namespace gargantuan {
	class UnreliableSequencedRemoteEvent final : public RemoteEventBase {
		I_UnreliableSequencedRemoteEvent;

	  public:
		[[nodiscard]] network::RemoteInstanceKind GetRemoteKind() const override {
			return network::RemoteInstanceKind::UnreliableSequencedEvent;
		}
	};
}
