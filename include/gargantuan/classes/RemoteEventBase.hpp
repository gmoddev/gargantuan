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
		std::shared_ptr<network::RemoteSignal> OnPlayerSignal;

		static int Submit(lua_State *L, Instance *InstanceValue, network::RemoteManagerRole Role,
			bool ReturnStatus, std::string_view Method);
		static int SubmitBroadcast(lua_State *L, Instance *InstanceValue, bool ReturnStatus);
	};
}
