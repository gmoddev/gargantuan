#pragma once

#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/classes/generated/RemoteEventBase.hpp"
#include "gargantuan/network/RemoteSignal.hpp"

namespace gargantuan {
	class RemoteEventBase : public RemoteBase {
		I_RemoteEventBase;

	  public:
		RemoteEventBase();
		void BindEventHandler();

	  private:
		std::shared_ptr<network::RemoteSignal> OnClientSignal;
		std::shared_ptr<network::RemoteSignal> OnServerSignal;
	};
}
