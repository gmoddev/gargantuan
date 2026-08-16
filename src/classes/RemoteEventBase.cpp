#include "gargantuan/classes/RemoteEventBase.hpp"

#include "gargantuan/network/RemoteLuau.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <lualib.h>
#include <ranges>
#include <string>

namespace gargantuan {
	namespace {
		RemoteEventBase *CheckedEvent(Instance *InstanceValue) {
			auto *Event = dynamic_cast<RemoteEventBase *>(InstanceValue);
			if (!Event || !Event->GetRemoteManager())
				throw std::runtime_error("Remote event is not bound to a RemoteManager");
			return Event;
		}

		void RequireDirection(
			network::RemoteManagerRole Role,
			ScriptExecutionDomain Domain,
			network::RemoteManagerRole RequiredRole,
			ScriptExecutionDomain RequiredDomain,
			std::string_view Method
		) {
			if (Role != RequiredRole || Domain != RequiredDomain)
				throw std::runtime_error(std::string(Method) + " is unavailable in the current runtime domain");
		}
	}

	RemoteEventBase::RemoteEventBase()
		: OnClientSignal(std::make_shared<network::RemoteSignal>(false)),
		  OnServerSignal(std::make_shared<network::RemoteSignal>(true)) {}

	std::shared_ptr<BaseSignal> RemoteEventBase::GetOnClientEvent() const {
		return OnClientSignal;
	}
	std::shared_ptr<BaseSignal> RemoteEventBase::GetOnServerEvent() const {
		return OnServerSignal;
	}

	void RemoteEventBase::BindEventHandler() {
		if (!Manager) return;
		auto Resolver = [Manager = Manager](ObjectId Object) { return Manager->ResolveObject(Object); };
		Manager->SetEventHandler(GetObjectId(), [this, Resolver](const network::RemoteInvocation &Invocation) {
			ScriptSecurityContext Context{
				Manager->GetRole() == network::RemoteManagerRole::Server ? ScriptExecutionDomain::Server
																		 : ScriptExecutionDomain::Client,
				{ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive},
			};
			ScriptSecurityScope Scope(Context);
			auto StableResolver = network::StabilizeRemoteLuauObjectReferences(Invocation.Arguments, Resolver);
			network::RemoteSignalPayload Payload{Invocation.Peer, Invocation.Arguments, std::move(StableResolver)};
			if (Manager->GetRole() == network::RemoteManagerRole::Server)
				OnServerSignal->Fire(Payload);
			else
				OnClientSignal->Fire(Payload);
		});
	}

	int RemoteEventBase::FireServer(lua_State *L, Instance *InstanceValue) {
		auto *Event = CheckedEvent(InstanceValue);
		RequireDirection(
			Event->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Client,
			ScriptExecutionDomain::Client,
			"FireServer"
		);
		const auto Connection = Event->GetDefaultServerConnection();
		if (!Connection) throw std::runtime_error("FireServer has no active server connection");
		auto Result = Event->Manager->SendEvent(
			*Connection, Event->GetObjectId(), network::ReadRemoteLuauArguments(L, 2)
		);
		if (!Result.Accepted())
			throw std::runtime_error(
				"FireServer rejected: " + std::string(network::GetRemoteSendStatusName(Result.Status))
			);
		return 0;
	}

	int RemoteEventBase::FireClient(lua_State *L, Instance *InstanceValue) {
		auto *Event = CheckedEvent(InstanceValue);
		RequireDirection(
			Event->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			"FireClient"
		);
		const network::ConnectionId Connection{
			static_cast<std::uint32_t>(luaL_checkunsigned(L, 2)),
			static_cast<std::uint32_t>(luaL_checkunsigned(L, 3)),
		};
		auto Result = Event->Manager->SendEvent(
			Connection, Event->GetObjectId(), network::ReadRemoteLuauArguments(L, 4)
		);
		if (!Result.Accepted())
			throw std::runtime_error(
				"FireClient rejected: " + std::string(network::GetRemoteSendStatusName(Result.Status))
			);
		return 0;
	}

	int RemoteEventBase::FireAllClients(lua_State *L, Instance *InstanceValue) {
		auto *Event = CheckedEvent(InstanceValue);
		RequireDirection(
			Event->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			"FireAllClients"
		);
		auto Results = Event->Manager->Broadcast(Event->GetObjectId(), network::ReadRemoteLuauArguments(L, 2));
		if (Results.empty()) throw std::runtime_error("FireAllClients has no eligible peers");
		if (std::ranges::none_of(Results, [](const auto &Result) { return Result.Accepted(); }))
			throw std::runtime_error("FireAllClients was rejected for every eligible peer");
		return 0;
	}
}
