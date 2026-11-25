#pragma once

#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/classes/generated/RemoteFunction.hpp"

namespace gargantuan {
	class ScriptEngine;

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
		friend class ScriptEngine;
		lua_State *HandlerState = nullptr;
		int ServerHandlerReference = LUA_NOREF;
		int ClientHandlerReference = LUA_NOREF;
		bool IncludePlayerInServerHandler = false;
		void ClearHandlers();
		static int InstallServerHandler(lua_State *L, Instance *InstanceValue, bool IncludePlayer);
		static int Submit(lua_State *L, Instance *InstanceValue, network::RemoteManagerRole Role,
			bool ExplicitDeadline, bool ReturnStatus, std::string_view Method);
	};
}
