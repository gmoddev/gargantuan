#pragma once

#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/classes/generated/RemoteFunction.hpp"

namespace gargantuan {
	class RemoteFunction final : public RemoteBase {
		I_RemoteFunction;

	  public:
		RemoteFunction();
		~RemoteFunction() override;
		[[nodiscard]] network::RemoteInstanceKind GetRemoteKind() const override {
			return network::RemoteInstanceKind::Function;
		}
		void BindRequestHandler();

	  private:
		lua_State *HandlerState = nullptr;
		int ServerHandlerReference = LUA_NOREF;
		int ClientHandlerReference = LUA_NOREF;
		void ClearHandlers();
	};
}
