#include "gargantuan/classes/RemoteEventBase.hpp"

#include "gargantuan/network/RemoteLuau.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <algorithm>
#include <lualib.h>
#include <map>
#include <string>

namespace gargantuan {
	namespace {
		void RequireDirection(network::RemoteManagerRole Role, std::string_view Method) {
			const auto Domain = Role == network::RemoteManagerRole::Server ? ScriptExecutionDomain::Server
																		 : ScriptExecutionDomain::Client;
			if (GetCurrentScriptSecurityContext().Domain != Domain)
				throw std::runtime_error(std::string(Method) + " is unavailable in the current runtime domain");
		}

		int PushSendResult(lua_State *L, const network::RemoteSendResult &Result, bool ReturnStatus,
			std::string_view Method) {
			if (ReturnStatus) {
				lua_pushboolean(L, Result.Accepted());
				lua_pushstring(L, network::GetRemoteSendStatusCode(Result.Status).data());
				return 2;
			}
			if (!Result.Accepted())
				throw std::runtime_error(std::string(Method) + " rejected: " +
					std::string(network::GetRemoteSendStatusName(Result.Status)));
			return 0;
		}
	}

	RemoteEventBase::RemoteEventBase()
		: OnClientSignal(std::make_shared<network::RemoteSignal>(false)),
		  OnServerSignal(std::make_shared<network::RemoteSignal>(true)),
		  OnPlayerSignal(std::make_shared<network::RemoteSignal>(network::RemoteSignalSender::Player)) {}

	std::shared_ptr<BaseSignal> RemoteEventBase::GetOnClientEvent() const {
		return OnClientSignal;
	}
	std::shared_ptr<BaseSignal> RemoteEventBase::GetOnServerEvent() const {
		return OnServerSignal;
	}
	std::shared_ptr<BaseSignal> RemoteEventBase::GetOnPlayerEvent() const {
		return OnPlayerSignal;
	}

	void RemoteEventBase::BindEventHandler() {
		if (!Manager) return;
		std::weak_ptr<RemoteEventBase> WeakSelf = std::dynamic_pointer_cast<RemoteEventBase>(shared_from_this());
		Manager->SetEventHandler(GetNetworkObjectId(), [WeakSelf](const network::RemoteInvocation &Invocation) {
			const auto Self = WeakSelf.lock();
			if (!Self || Self->GetDestroyed() || Self->IsDestroying()) return;
			auto *BoundManager = Self->GetRemoteManager();
			if (!BoundManager) return;
			const bool Server = BoundManager->GetRole() == network::RemoteManagerRole::Server;
			ScriptSecurityContext Context{
				Server ? ScriptExecutionDomain::Server : ScriptExecutionDomain::Client,
				{ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive},
			};
			ScriptSecurityScope Scope(Context);
			auto StableResolver = network::StabilizeRemoteLuauObjectReferences(
				Invocation.Arguments, [BoundManager](ObjectId Object) { return BoundManager->ResolveObject(Object); }
			);
			network::RemoteSignalPayload Payload{
				Invocation.Peer,
				std::make_shared<const std::vector<network::RemoteValue>>(Invocation.Arguments),
				std::move(StableResolver),
			};
			if (!Server) {
				Self->OnClientSignal->Fire(std::move(Payload));
				return;
			}
			if (Self->OnPlayerSignal->Connections.empty()) {
				Self->OnServerSignal->Fire(std::move(Payload));
				return;
			}
			Self->OnServerSignal->Fire(Payload);
			if (Self->GetDestroyed() || Self->IsDestroying() || Self->GetRemoteManager() != BoundManager) return;
			Payload.Sender = BoundManager->ResolvePlayer(Invocation.Peer);
			if (!Payload.Sender) return;
			const std::weak_ptr<Player> Sender = Payload.Sender;
			Self->OnPlayerSignal->Fire(std::move(Payload),
				[WeakSelf, BoundManager, Peer = Invocation.Peer, Remote = Invocation.Remote,
				 Publication = Invocation.Publication, Sender] {
					const auto Event = WeakSelf.lock();
					const auto PlayerValue = Sender.lock();
					return Event && PlayerValue && !Event->GetDestroyed() && !Event->IsDestroying() &&
						Event->GetRemoteManager() == BoundManager && BoundManager->ResolvePlayer(Peer) == PlayerValue &&
						BoundManager->IsCurrentPublication(Peer, Remote, Publication);
				}
			);
		});
	}

	int RemoteEventBase::FireServer(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Client, false, "FireServer");
	}

	int RemoteEventBase::FireClient(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Server, false, "FireClient");
	}

	int RemoteEventBase::TryFireServer(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Client, true, "TryFireServer");
	}

	int RemoteEventBase::TryFireClient(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Server, true, "TryFireClient");
	}

	int RemoteEventBase::Submit(lua_State *L, Instance *InstanceValue, network::RemoteManagerRole Role,
		bool ReturnStatus, std::string_view Method) {
		RequireDirection(Role, Method);
		auto *Event = dynamic_cast<RemoteEventBase *>(InstanceValue);
		auto *BoundManager = Event ? Event->GetRemoteManager() : nullptr;
		if (!BoundManager)
			return PushSendResult(L, {network::RemoteSendStatus::UnknownRemote}, ReturnStatus, Method);
		if (BoundManager->GetRole() != Role)
			throw std::runtime_error(std::string(Method) + " is unavailable in the current runtime domain");
		const auto [Connection, FirstArgument] = Role == network::RemoteManagerRole::Server
			? Event->ReadClientTarget(L)
			: std::pair{Event->GetDefaultServerConnection().value_or(network::ConnectionId{}), 2};
		if (!Connection.IsValid())
			return PushSendResult(L, {network::RemoteSendStatus::InvalidPeer}, ReturnStatus, Method);
		const auto Result = BoundManager->SendEvent(Connection, Event->GetNetworkObjectId(),
			network::ReadRemoteLuauArguments(L, FirstArgument, BoundManager));
		return PushSendResult(L, Result, ReturnStatus, Method);
	}

	int RemoteEventBase::FireAllClients(lua_State *L, Instance *InstanceValue) {
		return SubmitBroadcast(L, InstanceValue, false);
	}

	int RemoteEventBase::TryFireAllClients(lua_State *L, Instance *InstanceValue) {
		return SubmitBroadcast(L, InstanceValue, true);
	}

	int RemoteEventBase::SubmitBroadcast(lua_State *L, Instance *InstanceValue, bool ReturnStatus) {
		const auto Method = ReturnStatus ? "TryFireAllClients" : "FireAllClients";
		RequireDirection(network::RemoteManagerRole::Server, Method);
		auto *Event = dynamic_cast<RemoteEventBase *>(InstanceValue);
		auto *BoundManager = Event ? Event->GetRemoteManager() : nullptr;
		if (!BoundManager || BoundManager->GetRole() != network::RemoteManagerRole::Server)
			throw std::runtime_error(std::string(Method) + " requires a server-bound Remote");
		const auto Results = BoundManager->Broadcast(Event->GetNetworkObjectId(),
			network::ReadRemoteLuauArguments(L, 2, BoundManager));
		const auto Accepted = std::ranges::count_if(Results, [](const auto &Result) { return Result.Accepted(); });
		if (ReturnStatus) {
			std::map<std::string_view, int> Rejections;
			for (const auto &Result : Results)
				if (!Result.Accepted()) ++Rejections[network::GetRemoteSendStatusCode(Result.Status)];
			lua_pushinteger(L, static_cast<int>(Accepted));
			lua_pushinteger(L, static_cast<int>(Results.size() - static_cast<std::size_t>(Accepted)));
			lua_createtable(L, 0, static_cast<int>(Rejections.size()));
			for (const auto &[Code, Count] : Rejections) {
				lua_pushinteger(L, Count);
				lua_setfield(L, -2, Code.data());
			}
			return 3;
		}
		if (Results.empty()) throw std::runtime_error("FireAllClients has no eligible peers");
		if (Accepted == 0) throw std::runtime_error("FireAllClients was rejected for every eligible peer");
		return 0;
	}
}
