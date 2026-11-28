// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <limits>
#include <utility>

namespace gargantuan {
	namespace {
		void StringField(lua_State *L, const char *Name, const char *Value) {
			lua_pushstring(L, Value);
			lua_setfield(L, -2, Name);
		}

		void PushSession(lua_State *L, bool Active, std::uint64_t Generation, const char *State) {
			lua_createtable(L, 0, 4);
			StringField(L, "State", State);
			StringField(L, "Generation", std::to_string(Generation).c_str());
			StringField(L, "World", Active ? "Ready" : "Absent");
			if (Active) StringField(L, "Mode", "Offline");
			lua_setreadonly(L, -1, true);
		}

		struct Reply {
			const char *Code = nullptr;
			const char *Message = nullptr;
			bool Session = false;
			bool Active = false;
			bool Paused = false;
			std::uint64_t Generation = 0;

			int Push(lua_State *L) const {
				lua_createtable(L, 0, 3);
				lua_pushboolean(L, Code == nullptr);
				lua_setfield(L, -2, "Ok");
				if (Code) {
					StringField(L, "Code", Code);
					StringField(L, "Message", Message);
				} else {
					if (Session) PushSession(L, Active, Generation, Active ? "Active" : "Idle");
					else lua_pushboolean(L, Paused);
					lua_setfield(L, -2, "Value");
				}
				lua_setreadonly(L, -1, true);
				return 1;
			}
		};

		class Completion final : public BaseSignal {
		  public:
			Enums::SignalType GetSignalType() override { return Enums::SignalType::Engine; }
			int LPushArgument(lua_State *L, std::any Value) override { return std::any_cast<const Reply &>(Value).Push(L); }
		};

		ApplicationPresentation *Caller(lua_State *L, Instance *Receiver) {
			auto *VM = ScriptEngine::Get(L);
			auto *App = VM->GetApplicationPresentation();
			return App && Receiver->GetDataModel() == VM->DataModel && App->IsCallerCurrent(*VM->DataModel) ? App : nullptr;
		}
	}

	struct ApplicationSession::Operation {
		SessionCommand Command{};
		std::uint64_t OwnerGeneration = 0;
		bool CancelRequested = false;
		std::optional<Reply> Result;
		std::shared_ptr<Completion> Completed = std::make_shared<Completion>();
		using Reference = std::shared_ptr<Operation>;

		static Operation &Bound(lua_State *L) {
			return **static_cast<Reference *>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		static int Push(lua_State *L, Reference Value) {
			lua_createtable(L, 0, 5);
			StackValue<std::shared_ptr<Completion>>::Push(L, Value->Completed);
			lua_setfield(L, -2, "Completed");
			new (lua_newuserdatadtor(L, sizeof(Reference), [](void *Data) {
				std::destroy_at(static_cast<Reference *>(Data));
			})) Reference(std::move(Value));
			const auto Method = [&](const char *Name, lua_CFunction Function) {
				lua_pushvalue(L, -1);
				lua_pushcclosure(L, Function, Name, 1);
				lua_setfield(L, -3, Name);
			};
			Method("Await", [](lua_State *L) {
				auto &Op = Bound(L);
				return Op.Result ? Op.Result->Push(L) : BaseSignal::LWait(L, Op.Completed.get());
			});
			Method("GetResult", [](lua_State *L) {
				auto &Op = Bound(L);
				if (Op.Result) return Op.Result->Push(L);
				lua_pushnil(L);
				return 1;
			});
			Method("GetState", [](lua_State *L) {
				const auto &Op = Bound(L);
				lua_pushstring(L, !Op.Result ? "Pending" : !Op.Result->Code ? "Succeeded" :
					Op.CancelRequested ? "Cancelled" : "Failed");
				return 1;
			});
			Method("Cancel", [](lua_State *L) {
				auto &Op = Bound(L);
				const bool CanCancel = !Op.Result && !Op.CancelRequested;
				if (CanCancel) Op.CancelRequested = true;
				lua_pushboolean(L, CanCancel);
				return 1;
			});
			lua_pop(L, 1);
			lua_setreadonly(L, -1, true);
			return 1;
		}
	};

	int ApplicationSession::Request(lua_State *L, Instance *Receiver, SessionCommand Command) {
		auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		Op->Command = Command;
		Op->OwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		if (!App) Op->Result = Reply{"NotAuthorized", "The local application owns this operation."};
		else if (lua_gettop(L) > 1 && !lua_isnoneornil(L, 2))
			Op->Result = Reply{"InvalidOptions", "This offline operation takes no options."};
		else if (App->Sessions.Pending) Op->Result = Reply{"Busy", "Another session operation is pending."};
		else if (Command == SessionCommand::StartLocal && App->Sessions.Active) Op->Result = Reply{"WrongState", "Leave the active world first."};
		else if (Command != SessionCommand::StartLocal && !App->Sessions.Active) Op->Result = Reply{"NoSession", "No gameplay session is active."};
		else App->Sessions.Pending = Op;
		return Operation::Push(L, std::move(Op));
	}

	int ApplicationSession::GetInfo(lua_State *L, Instance *Receiver) {
		auto *App = Caller(L, Receiver);
		if (!App) luaL_error(L, "Application session is unavailable");
		const auto &Sessions = App->Sessions;
		const auto Pending = Sessions.Pending;
		const char *State = Sessions.Active ? "Active" : "Idle";
		if (Pending && Pending->Command == SessionCommand::StartLocal) State = "Starting";
		if (Pending && Pending->Command == SessionCommand::Leave) State = "Leaving";
		PushSession(L, Sessions.Active, Sessions.Generation, State);
		return 1;
	}

	void ApplicationSession::Commit(ApplicationPresentation &App, const Executor &Execute) {
		if (!Pending) return;
		auto Op = std::exchange(Pending, {});
		Reply Outcome{};
		if (Op->CancelRequested) Outcome = {"Cancelled", "Session operation was cancelled."};
		else if (!App.IsEntryCurrent(Op->OwnerGeneration)) Outcome = {"Retired", "Session owner retired."};
		else {
			try {
				if (Op->Command == SessionCommand::StartLocal) {
					if (Generation == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Session generations exhausted");
					++Generation;
				}
				Execute(Op->Command);
				switch (Op->Command) {
				case SessionCommand::StartLocal: Active = true; Paused = false; break;
				case SessionCommand::Leave: Active = false; Paused = false; break;
				case SessionCommand::Pause: Paused = true; break;
				case SessionCommand::Resume: Paused = false; break;
				}
				Outcome = {nullptr, nullptr, Op->Command == SessionCommand::StartLocal || Op->Command == SessionCommand::Leave,
					Active, Paused, Generation};
			} catch (const std::exception &) { Outcome = {"StartupFailed", "The host could not complete the session operation."}; }
		}
		Op->Result = Outcome;
		Op->Completed->Fire(Outcome);
		Op->Completed->DisconnectAll();
	}
}
