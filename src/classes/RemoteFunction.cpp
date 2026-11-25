#include "gargantuan/classes/RemoteFunction.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/RemoteLuau.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/scripting/ExecutionBudget.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <cmath>
#include <algorithm>
#include <lualib.h>
#include <unordered_map>

namespace gargantuan {
	namespace {
		struct SuspendedHandler {
			RemoteFunction *Owner = nullptr;
			lua_State *MainState = nullptr;
			int ThreadReference = LUA_NOREF;
			network::RemoteManager::RequestReply Reply;
		};

		std::unordered_map<lua_State *, SuspendedHandler> &SuspendedHandlers() {
			static std::unordered_map<lua_State *, SuspendedHandler> Handlers;
			return Handlers;
		}

		bool CompleteSuspendedHandler(lua_State *Thread, int Status) {
			auto Handler = SuspendedHandlers().find(Thread);
			if (Handler == SuspendedHandlers().end()) return false;
			if (Status == LUA_YIELD) return true;
			auto State = std::move(Handler->second);
			SuspendedHandlers().erase(Handler);
			if (Status == LUA_OK) {
				try {
					State.Reply(network::ReadRemoteLuauArguments(Thread, 1, State.Owner->GetRemoteManager()), std::nullopt);
				} catch (const std::exception &Error) {
					LOG_ERROR(Lua, "[Networking:Remote] nested handler result error: %s", Error.what());
					State.Reply(
						{},
						network::StructuredRemoteError{"invalid_result", "Remote handler returned unsupported values"}
					);
				}
			} else {
				LOG_ERROR(Lua, "[Networking:Remote] nested handler error: %s", lua_tostring(Thread, -1));
				State.Reply({}, network::StructuredRemoteError{"handler_error", "Remote handler failed"});
			}
			lua_unref(State.MainState, State.ThreadReference);
			return true;
		}

		RemoteFunction *CheckedFunction(Instance *InstanceValue) {
			auto *Function = dynamic_cast<RemoteFunction *>(InstanceValue);
			if (!Function || !Function->GetRemoteManager())
				throw std::runtime_error("RemoteFunction is not bound to a RemoteManager");
			return Function;
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

		std::chrono::milliseconds ReadDeadline(lua_State *L, int Index) {
			const auto Seconds = static_cast<double>(luaL_checknumber(L, Index));
			if (!std::isfinite(Seconds) || Seconds <= 0 || Seconds > 30)
				throw std::invalid_argument("Remote request timeout must be finite and in (0, 30] seconds");
			return std::chrono::milliseconds(std::max<std::int64_t>(1, static_cast<std::int64_t>(Seconds * 1000)));
		}

		std::string_view TerminalStatusName(network::RemoteRequestTerminalStatus Status) {
			switch (Status) {
			case network::RemoteRequestTerminalStatus::Success:
				return "success";
			case network::RemoteRequestTerminalStatus::Timeout:
				return "timeout";
			case network::RemoteRequestTerminalStatus::Cancelled:
				return "cancelled";
			case network::RemoteRequestTerminalStatus::Disconnected:
				return "disconnected";
			case network::RemoteRequestTerminalStatus::RemoteError:
				return "remote_error";
			case network::RemoteRequestTerminalStatus::ProtocolRejected:
				return "protocol_rejected";
			case network::RemoteRequestTerminalStatus::ResourceRejected:
				return "resource_rejected";
			}
			return "protocol_rejected";
		}

		int PushFailure(lua_State *L, bool ReturnStatus, std::string_view Code, std::string_view Message) {
			if (ReturnStatus) lua_pushboolean(L, false);
			else lua_pushnil(L);
			lua_pushlstring(L, Code.data(), Code.size());
			lua_pushlstring(L, Message.data(), Message.size());
			return 3;
		}

		int RejectSubmission(lua_State *L, bool ReturnStatus, network::RemoteSendStatus Status) {
			if (ReturnStatus)
				return PushFailure(L, true, network::GetRemoteSendStatusCode(Status), network::GetRemoteSendStatusName(Status));
			throw std::runtime_error(
				"RemoteFunction request rejected: " + std::string(network::GetRemoteSendStatusName(Status))
			);
		}

		int Invoke(
			lua_State *L,
			RemoteFunction *Function,
			network::ConnectionId Connection,
			int FirstArgument,
			std::chrono::milliseconds Deadline,
			bool ReturnStatus
		) {
			lua_State *Main = lua_mainthread(L);
			if (L == Main || !lua_isyieldable(L))
				throw std::runtime_error("RemoteFunction invocation requires a yieldable Luau coroutine");
			if (ScriptEngine::Get(Main)->IsStopping())
				throw std::runtime_error("RemoteFunction runtime is stopping");
			auto *Manager = Function->GetRemoteManager();
			auto Arguments = network::ReadRemoteLuauArguments(L, FirstArgument, Manager);
			lua_pushthread(L);
			lua_xmove(L, Main, 1);
			const int ThreadReference = lua_ref(Main, -1);
			lua_pop(Main, 1);
			auto RuntimeWeak = ScriptEngine::Get(Main)->weak_from_this();
			const bool ManagedRuntime = !RuntimeWeak.expired();
			const auto SecurityContext = GetCurrentScriptSecurityContext();
			auto Completion = [L, Main, ThreadReference, Manager, SecurityContext, RuntimeWeak, ManagedRuntime, ReturnStatus](
								  network::RemoteRequestResult Result
							  ) {
				auto Runtime = RuntimeWeak.lock();
				if (ManagedRuntime && !Runtime) return;
				if (Runtime && Runtime->IsStopping()) {
					lua_unref(Main, ThreadReference);
					return;
				}
				int ArgumentCount = 0;
				try {
					if (Result.Outcome.Status == network::RemoteRequestTerminalStatus::Success) {
						network::ValidateRemoteLuauArguments(Result.Results, [Manager](ObjectId Object) {
							return Manager ? Manager->ResolveObject(Object) : nullptr;
						});
						if (ReturnStatus) lua_pushboolean(L, true);
						ArgumentCount = (ReturnStatus ? 1 : 0) + network::PushRemoteLuauArguments(L, Result.Results, [Manager](ObjectId Object) {
							return Manager ? Manager->ResolveObject(Object) : nullptr;
						});
					} else {
						ArgumentCount = PushFailure(L, ReturnStatus, TerminalStatusName(Result.Outcome.Status),
							Result.Outcome.Error ? std::string_view(Result.Outcome.Error->Message) : "Remote request terminated");
						if (ReturnStatus && Result.Outcome.Error) {
							const auto &Code = Result.Outcome.Error->Code;
							lua_pushlstring(L, Code.data(), Code.size());
							++ArgumentCount;
						}
					}
				} catch (const std::exception &Error) {
					LOG_ERROR(Lua, "[Networking:Remote] request completion error: %s", Error.what());
					lua_settop(L, 0);
					ArgumentCount = PushFailure(L, ReturnStatus, "protocol_rejected", "Remote response contained unsupported values");
				}
				ScriptSecurityScope SecurityScope(SecurityContext);
				const int PreResumeStatus = lua_costatus(Main, L);
				const int Status = ResumeGameplay(L, Main, ArgumentCount);
				ScriptEngine::Get(Main)->CompleteManagedScriptThreadResume(L, Status, PreResumeStatus);
				if (!CompleteSuspendedHandler(L, Status) && Status != LUA_OK && Status != LUA_YIELD)
					LOG_ERROR(Lua, "[Networking:Remote] request coroutine error: %s", lua_tostring(L, -1));
				lua_unref(Main, ThreadReference);
			};
			network::RemoteSendResult Result;
			try {
				Result = Manager->StartRequest(Connection, Function->GetNetworkObjectId(), std::move(Arguments),
					std::move(Completion), Deadline);
			} catch (...) {
				lua_unref(Main, ThreadReference);
				throw;
			}
			if (!Result.Accepted()) {
				lua_unref(Main, ThreadReference);
				return RejectSubmission(L, ReturnStatus, Result.Status);
			}
			return lua_yield(L, 0);
		}
	}

	RemoteFunction::RemoteFunction() = default;
	RemoteFunction::~RemoteFunction() {
		ClearHandlers();
	}

	void RemoteFunction::ClearHandlers() {
		for (auto Iterator = SuspendedHandlers().begin(); Iterator != SuspendedHandlers().end();) {
			if (Iterator->second.Owner != this) {
				++Iterator;
				continue;
			}
			lua_unref(Iterator->second.MainState, Iterator->second.ThreadReference);
			Iterator = SuspendedHandlers().erase(Iterator);
		}
		if (!HandlerState) return;
		if (ServerHandlerReference != LUA_NOREF) lua_unref(HandlerState, ServerHandlerReference);
		if (ClientHandlerReference != LUA_NOREF) lua_unref(HandlerState, ClientHandlerReference);
		ServerHandlerReference = ClientHandlerReference = LUA_NOREF;
		HandlerState = nullptr;
	}

	void RemoteFunction::BindRequestHandler() {
		if (!Manager) return;
		std::weak_ptr<RemoteFunction> WeakSelf = std::dynamic_pointer_cast<RemoteFunction>(shared_from_this());
		Manager->SetRequestHandler(
			GetNetworkObjectId(),
			[WeakSelf](const network::RemoteInvocation &Invocation, network::RemoteManager::RequestReply Reply) {
				auto Self = WeakSelf.lock();
				if (!Self || Self->GetDestroyed() || Self->IsDestroying()) {
					Reply({}, network::StructuredRemoteError{"remote_unavailable", "Remote is unavailable"});
					return;
				}
				auto *Function = Self.get();
				auto *BoundManager = Function->GetRemoteManager();
				if (!BoundManager) {
					Reply({}, network::StructuredRemoteError{"remote_unavailable", "Remote is unavailable"});
					return;
				}
				const auto BoundManagerLifetime = Function->ManagerLifetime;
				const bool Server = BoundManager->GetRole() == network::RemoteManagerRole::Server;
				const auto Sender = Server && Function->IncludePlayerInServerHandler
					? BoundManager->ResolvePlayer(Invocation.Peer) : nullptr;
				if (Server && Function->IncludePlayerInServerHandler && !Sender) {
					Reply({}, network::StructuredRemoteError{"player_unavailable", "Sending Player is unavailable"});
					return;
				}
				const int Reference = Server ? Function->ServerHandlerReference : Function->ClientHandlerReference;
				lua_State *HandlerState = Function->HandlerState;
				if (!HandlerState || Reference == LUA_NOREF) {
					Reply({}, network::StructuredRemoteError{"no_handler", "Remote request has no Luau handler"});
					return;
				}
				auto Runtime = ScriptEngine::Get(HandlerState)->weak_from_this().lock();
				if (Runtime && Runtime->IsStopping()) {
					Reply({}, network::StructuredRemoteError{"no_handler", "Remote request has no Luau handler"});
					return;
				}
				try {
					network::ValidateRemoteLuauArguments(Invocation.Arguments, [BoundManager](ObjectId Object) {
						return BoundManager->ResolveObject(Object);
					});
				} catch (const std::exception &Error) {
					LOG_ERROR(Lua, "[Networking:Remote] handler argument error: %s", Error.what());
					Reply({}, network::StructuredRemoteError{"invalid_arguments", "Remote arguments were rejected"});
					return;
				}
				lua_State *Thread = lua_newthread(HandlerState);
				const int ThreadReference = lua_ref(HandlerState, -1);
				lua_pop(HandlerState, 1);
				lua_getref(HandlerState, Reference);
				lua_xmove(HandlerState, Thread, 1);
				int ArgumentCount = 0;
				if (Sender) {
					ArgumentCount = StackValue<std::shared_ptr<Player>>::Push(Thread, Sender);
				} else if (Server) {
					network::PushRemotePeerContext(Thread, Invocation.Peer);
					++ArgumentCount;
				}
				try {
					ArgumentCount += network::PushRemoteLuauArguments(
						Thread, Invocation.Arguments, [BoundManager](ObjectId Object) {
							return BoundManager->ResolveObject(Object);
						}
					);
				} catch (const std::exception &Error) {
					LOG_ERROR(Lua, "[Networking:Remote] handler argument error: %s", Error.what());
					lua_unref(HandlerState, ThreadReference);
					Reply({}, network::StructuredRemoteError{"invalid_arguments", "Remote arguments were rejected"});
					return;
				}
				ScriptSecurityContext Context{
					Server ? ScriptExecutionDomain::Server : ScriptExecutionDomain::Client,
					{ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive},
				};
				Context.AllowsTaskSchedulerYield = false;
				ScriptSecurityScope Scope(Context);
				const auto PendingBefore = BoundManager->GetMetrics().InFlightRequests;
				const int Status = ResumeGameplay(Thread, HandlerState, ArgumentCount);
				if (Status == LUA_OK) {
					try {
						const auto *ReplyManager = !BoundManagerLifetime.expired() &&
							Function->GetRemoteManager() == BoundManager ? BoundManager : nullptr;
						Reply(network::ReadRemoteLuauArguments(Thread, 1, ReplyManager), std::nullopt);
					} catch (const std::exception &Error) {
						LOG_ERROR(Lua, "[Networking:Remote] handler result error: %s", Error.what());
						Reply(
							{},
							network::StructuredRemoteError{
								"invalid_result", "Remote handler returned unsupported values"
							}
						);
					}
				} else if (Status == LUA_YIELD && !BoundManagerLifetime.expired() &&
						   Function->GetRemoteManager() == BoundManager &&
						   BoundManager->GetMetrics().InFlightRequests > PendingBefore) {
					SuspendedHandlers().emplace(
						Thread, SuspendedHandler{Function, HandlerState, ThreadReference, std::move(Reply)}
					);
					return;
				} else {
					if (Status != LUA_YIELD)
						LOG_ERROR(Lua, "[Networking:Remote] handler error: %s", lua_tostring(Thread, -1));
					Reply({}, network::StructuredRemoteError{"handler_error", "Remote handler failed"});
				}
				lua_unref(HandlerState, ThreadReference);
			}
		);
	}

	int RemoteFunction::SetServerHandler(lua_State *L, Instance *InstanceValue) {
		return InstallServerHandler(L, InstanceValue, false);
	}

	int RemoteFunction::SetPlayerHandler(lua_State *L, Instance *InstanceValue) {
		return InstallServerHandler(L, InstanceValue, true);
	}

	int RemoteFunction::InstallServerHandler(lua_State *L, Instance *InstanceValue, bool IncludePlayer) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			IncludePlayer ? "SetPlayerHandler" : "SetServerHandler"
		);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto *MainState = lua_mainthread(L);
		if (Function->HandlerState && Function->HandlerState != MainState)
			throw std::runtime_error("RemoteFunction handlers must use one Luau VM");
		Function->HandlerState = MainState;
		if (Function->ServerHandlerReference != LUA_NOREF)
			lua_unref(Function->HandlerState, Function->ServerHandlerReference);
		Function->ServerHandlerReference = lua_ref(L, 2);
		Function->IncludePlayerInServerHandler = IncludePlayer;
		ScriptEngine::Get(MainState)->TrackRemoteFunction(
			std::dynamic_pointer_cast<RemoteFunction>(Function->shared_from_this())
		);
		Function->BindRequestHandler();
		return 0;
	}

	int RemoteFunction::SetClientHandler(lua_State *L, Instance *InstanceValue) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Client,
			ScriptExecutionDomain::Client,
			"SetClientHandler"
		);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto *MainState = lua_mainthread(L);
		if (Function->HandlerState && Function->HandlerState != MainState)
			throw std::runtime_error("RemoteFunction handlers must use one Luau VM");
		Function->HandlerState = MainState;
		if (Function->ClientHandlerReference != LUA_NOREF)
			lua_unref(Function->HandlerState, Function->ClientHandlerReference);
		Function->ClientHandlerReference = lua_ref(L, 2);
		ScriptEngine::Get(MainState)->TrackRemoteFunction(
			std::dynamic_pointer_cast<RemoteFunction>(Function->shared_from_this())
		);
		Function->BindRequestHandler();
		return 0;
	}

	int RemoteFunction::InvokeServer(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Client, false, false, "InvokeServer");
	}

	int RemoteFunction::InvokeServerWithTimeout(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Client, true, false, "InvokeServerWithTimeout");
	}

	int RemoteFunction::InvokeClient(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Server, false, false, "InvokeClient");
	}

	int RemoteFunction::InvokeClientWithTimeout(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Server, true, false, "InvokeClientWithTimeout");
	}

	int RemoteFunction::TryInvokeServer(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Client, true, true, "TryInvokeServer");
	}

	int RemoteFunction::TryInvokeClient(lua_State *L, Instance *InstanceValue) {
		return Submit(L, InstanceValue, network::RemoteManagerRole::Server, true, true, "TryInvokeClient");
	}

	int RemoteFunction::Submit(lua_State *L, Instance *InstanceValue, network::RemoteManagerRole Role,
		bool ExplicitDeadline, bool ReturnStatus, std::string_view Method) {
		auto *Function = dynamic_cast<RemoteFunction *>(InstanceValue);
		auto *BoundManager = Function ? Function->GetRemoteManager() : nullptr;
		const bool Server = Role == network::RemoteManagerRole::Server;
		RequireDirection(
			BoundManager ? BoundManager->GetRole() : Role,
			GetCurrentScriptSecurityContext().Domain,
			Role,
			Server ? ScriptExecutionDomain::Server : ScriptExecutionDomain::Client,
			Method
		);
		if (!BoundManager) return RejectSubmission(L, ReturnStatus, network::RemoteSendStatus::UnknownRemote);
		const auto [Connection, FirstArgument] = Server
			? Function->ReadClientTarget(L)
			: std::pair{Function->GetDefaultServerConnection().value_or(network::ConnectionId{}), 2};
		if (!Connection.IsValid()) return RejectSubmission(L, ReturnStatus, network::RemoteSendStatus::InvalidPeer);
		const auto Deadline = ExplicitDeadline ? ReadDeadline(L, FirstArgument) : network::DefaultRemoteRequestDeadline;
		return Invoke(L, Function, Connection, FirstArgument + (ExplicitDeadline ? 1 : 0), Deadline, ReturnStatus);
	}
}
