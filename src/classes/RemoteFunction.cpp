#include "gargantuan/classes/RemoteFunction.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/network/RemoteLuau.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <cmath>
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
					State.Reply(network::ReadRemoteLuauArguments(Thread, 1), std::nullopt);
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
			return std::chrono::milliseconds(static_cast<std::int64_t>(Seconds * 1000));
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

		int Invoke(
			lua_State *L,
			RemoteFunction *Function,
			network::ConnectionId Connection,
			int FirstArgument,
			std::chrono::milliseconds Deadline
		) {
			lua_State *Main = lua_mainthread(L);
			if (L == Main) throw std::runtime_error("RemoteFunction invocation requires a yieldable Luau coroutine");
			lua_pushthread(L);
			lua_xmove(L, Main, 1);
			const int ThreadReference = lua_ref(Main, -1);
			lua_pop(Main, 1);
			auto *Manager = Function->GetRemoteManager();
			const auto SecurityContext = GetCurrentScriptSecurityContext();
			auto Completion = [L, Main, ThreadReference, Manager, SecurityContext](
								  network::RemoteRequestResult Result
							  ) {
				int ArgumentCount = 0;
				try {
					if (Result.Outcome.Status == network::RemoteRequestTerminalStatus::Success) {
						network::ValidateRemoteLuauArguments(Result.Results, [Manager](ObjectId Object) {
							return Manager ? Manager->ResolveObject(Object) : nullptr;
						});
						ArgumentCount = network::PushRemoteLuauArguments(L, Result.Results, [Manager](ObjectId Object) {
							return Manager ? Manager->ResolveObject(Object) : nullptr;
						});
					} else {
						lua_pushnil(L);
						lua_pushstring(L, TerminalStatusName(Result.Outcome.Status).data());
						if (Result.Outcome.Error)
							lua_pushlstring(
								L, Result.Outcome.Error->Message.data(), Result.Outcome.Error->Message.size()
							);
						else
							lua_pushstring(L, "Remote request terminated");
						ArgumentCount = 3;
					}
				} catch (const std::exception &Error) {
					LOG_ERROR(Lua, "[Networking:Remote] request completion error: %s", Error.what());
					lua_settop(L, 0);
					lua_pushnil(L);
					lua_pushstring(L, "protocol_rejected");
					lua_pushstring(L, "Remote response contained unsupported values");
					ArgumentCount = 3;
				}
				ScriptSecurityScope SecurityScope(SecurityContext);
				const int Status = lua_resume(L, Main, ArgumentCount);
				if (!CompleteSuspendedHandler(L, Status) && Status != LUA_OK && Status != LUA_YIELD)
					LOG_ERROR(Lua, "[Networking:Remote] request coroutine error: %s", lua_tostring(L, -1));
				lua_unref(Main, ThreadReference);
			};
			auto Result = Manager->StartRequest(
				Connection,
				Function->GetObjectId(),
				network::ReadRemoteLuauArguments(L, FirstArgument),
				std::move(Completion),
				Deadline
			);
			if (!Result.Accepted()) {
				lua_unref(Main, ThreadReference);
				throw std::runtime_error(
					"RemoteFunction request rejected: " + std::string(network::GetRemoteSendStatusName(Result.Status))
				);
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
		Manager->SetRequestHandler(
			GetObjectId(),
			[this](const network::RemoteInvocation &Invocation, network::RemoteManager::RequestReply Reply) {
				auto *BoundManager = GetRemoteManager();
				if (!BoundManager) {
					Reply({}, network::StructuredRemoteError{"remote_unavailable", "Remote is unavailable"});
					return;
				}
				const auto BoundManagerLifetime = ManagerLifetime;
				const bool Server = BoundManager->GetRole() == network::RemoteManagerRole::Server;
				const int Reference = Server ? ServerHandlerReference : ClientHandlerReference;
				if (!HandlerState || Reference == LUA_NOREF) {
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
				if (Server) {
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
				const int Status = lua_resume(Thread, HandlerState, ArgumentCount);
				if (Status == LUA_OK) {
					try {
						Reply(network::ReadRemoteLuauArguments(Thread, 1), std::nullopt);
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
						   GetRemoteManager() == BoundManager &&
						   BoundManager->GetMetrics().InFlightRequests > PendingBefore) {
					SuspendedHandlers().emplace(
						Thread, SuspendedHandler{this, HandlerState, ThreadReference, std::move(Reply)}
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
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			"SetServerHandler"
		);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto *MainState = lua_mainthread(L);
		if (Function->HandlerState && Function->HandlerState != MainState)
			throw std::runtime_error("RemoteFunction handlers must use one Luau VM");
		Function->HandlerState = MainState;
		if (Function->ServerHandlerReference != LUA_NOREF)
			lua_unref(Function->HandlerState, Function->ServerHandlerReference);
		Function->ServerHandlerReference = lua_ref(L, 2);
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
		Function->BindRequestHandler();
		return 0;
	}

	int RemoteFunction::InvokeServer(lua_State *L, Instance *InstanceValue) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Client,
			ScriptExecutionDomain::Client,
			"InvokeServer"
		);
		const auto Connection = Function->GetDefaultServerConnection();
		if (!Connection) throw std::runtime_error("InvokeServer has no active server connection");
		return Invoke(L, Function, *Connection, 2, network::DefaultRemoteRequestDeadline);
	}

	int RemoteFunction::InvokeServerWithTimeout(lua_State *L, Instance *InstanceValue) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Client,
			ScriptExecutionDomain::Client,
			"InvokeServerWithTimeout"
		);
		const auto Connection = Function->GetDefaultServerConnection();
		if (!Connection) throw std::runtime_error("InvokeServerWithTimeout has no active server connection");
		return Invoke(L, Function, *Connection, 3, ReadDeadline(L, 2));
	}

	int RemoteFunction::InvokeClient(lua_State *L, Instance *InstanceValue) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			"InvokeClient"
		);
		return Invoke(
			L,
			Function,
			{static_cast<std::uint32_t>(luaL_checkunsigned(L, 2)),
			 static_cast<std::uint32_t>(luaL_checkunsigned(L, 3))},
			4,
			network::DefaultRemoteRequestDeadline
		);
	}

	int RemoteFunction::InvokeClientWithTimeout(lua_State *L, Instance *InstanceValue) {
		auto *Function = CheckedFunction(InstanceValue);
		RequireDirection(
			Function->Manager->GetRole(),
			GetCurrentScriptSecurityContext().Domain,
			network::RemoteManagerRole::Server,
			ScriptExecutionDomain::Server,
			"InvokeClientWithTimeout"
		);
		return Invoke(
			L,
			Function,
			{static_cast<std::uint32_t>(luaL_checkunsigned(L, 2)),
			 static_cast<std::uint32_t>(luaL_checkunsigned(L, 3))},
			5,
			ReadDeadline(L, 4)
		);
	}
}
