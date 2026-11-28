// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <limits>
#include <utility>

namespace gargantuan {
	namespace {
		void StringField(lua_State *L, const char *Name, const char *Value) {
			lua_pushstring(L, Value);
			lua_setfield(L, -2, Name);
		}

	}
	void PushWorldSlot(lua_State *L, const WorldSlot &Slot) {
			lua_createtable(L, 0, 6);
			StringField(L, "SaveId", Slot.SaveId.c_str());
			StringField(L, "WorldId", Slot.WorldId.c_str());
			StringField(L, "Name", Slot.Name.c_str());
			if (!Slot.Seed.empty()) StringField(L, "Seed", Slot.Seed.c_str());
			StringField(L, "Revision", "1");
			StringField(L, "Compatibility", Slot.Compatible ? "Compatible" : "RequiresMigration");
			lua_setreadonly(L, -1, true);
		}

	namespace {
		void PushSession(lua_State *L, bool Active, std::uint64_t Generation, const char *State, const WorldSlot *Slot = nullptr,
			bool Remote = false, bool Disconnected = false) {
			lua_createtable(L, 0, 4);
			StringField(L, "State", State);
			StringField(L, "Generation", std::to_string(Generation).c_str());
			StringField(L, "World", Active ? "Ready" : std::string_view(State) == "Starting" ? "Preparing" : "Absent");
			if (Active) StringField(L, "Mode", Remote ? "Remote" : "Offline");
			if (Disconnected) {
				lua_createtable(L, 0, 3);
				lua_pushboolean(L, false); lua_setfield(L, -2, "Ok");
				StringField(L, "Code", "Disconnected"); StringField(L, "Message", "The remote connection closed.");
				lua_setreadonly(L, -1, true); lua_setfield(L, -2, "LastError");
			}
			if (Slot) {
				StringField(L, "SaveId", Slot->SaveId.c_str());
				StringField(L, "WorldId", Slot->WorldId.c_str());
			}
			lua_setreadonly(L, -1, true);
		}

		struct Reply {
			const char *Code = nullptr;
			const char *Message = nullptr;
			bool Session = false;
			bool Active = false;
			bool Paused = false;
			std::uint64_t Generation = 0;
			std::shared_ptr<const WorldCatalogResult> Catalog;
			bool Page = false;
			std::optional<WorldSlot> Slot;
			bool Remote = false;

			bool Succeeded() const { return !Code && (!Catalog || Catalog->Code.empty()); }

			int Push(lua_State *L) const {
				lua_createtable(L, 0, 3);
				lua_pushboolean(L, Succeeded());
				lua_setfield(L, -2, "Ok");
				if (Code) {
					StringField(L, "Code", Code);
					StringField(L, "Message", Message);
				} else if (Catalog && !Catalog->Code.empty()) {
					StringField(L, "Code", Catalog->Code.c_str());
					StringField(L, "Message", "World storage could not complete the requested operation.");
					if (!Catalog->RecoveryId.empty()) StringField(L, "RecoveryId", Catalog->RecoveryId.c_str());
				} else if (Catalog) {
					if (Page) {
						lua_createtable(L, 0, 2);
						lua_createtable(L, static_cast<int>(Catalog->Items.size()), 0);
						int Index = 0;
						for (const auto &Item : Catalog->Items) { PushWorldSlot(L, Item); lua_rawseti(L, -2, ++Index); }
						lua_setreadonly(L, -1, true);
						lua_setfield(L, -2, "Items");
						if (!Catalog->NextCursor.empty()) StringField(L, "NextCursor", Catalog->NextCursor.c_str());
						lua_setreadonly(L, -1, true);
					} else PushWorldSlot(L, Catalog->Items.at(0));
					lua_setfield(L, -2, "Value");
				} else {
					if (Session) PushSession(L, Active, Generation, Active ? "Active" : "Idle", Slot ? &*Slot : nullptr, Remote);
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
		ObjectId Owner;
		bool CancelRequested = false;
		bool Irreversible = false;
		bool Started = false;
		std::string SaveId;
		std::string Address;
		std::optional<WorldSlot> Slot;
		bool ReadingSlot = false;
		bool Page = false;
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
				lua_pushstring(L, !Op.Result ? "Pending" : Op.Result->Succeeded() ? "Succeeded" :
					((Op.Result->Code && std::string_view(Op.Result->Code) == "Cancelled") ||
					(Op.Result->Catalog && Op.Result->Catalog->Code == "Cancelled")) ? "Cancelled" : "Failed");
				return 1;
			});
			Method("Cancel", [](lua_State *L) {
				auto &Op = Bound(L);
				const bool CanCancel = !Op.Result && !Op.CancelRequested && !Op.Irreversible;
				if (CanCancel) Op.CancelRequested = true;
				lua_pushboolean(L, CanCancel);
				return 1;
			});
			lua_pop(L, 1);
			lua_setreadonly(L, -1, true);
			return 1;
		}
	};

	int ApplicationSession::RequestReconnect(lua_State *L, Instance *Receiver) {
		auto *App = Caller(L, Receiver);
		if (!App || !App->Sessions.Disconnected || App->Sessions.LastEndpoint.empty() || lua_gettop(L) != 1) {
			auto Op = std::make_shared<Operation>();
			Op->Result = !App ? Reply{"NotAuthorized", "The local application owns this operation."} :
				Reply{"WrongState", "There is no previous disconnected session."};
			return Operation::Push(L, std::move(Op));
		}
		lua_createtable(L, 0, 2);
		StringField(L, "Kind", "Direct");
		StringField(L, "Address", App->Sessions.LastEndpoint.c_str());
		return Request(L, Receiver, SessionCommand::Join);
	}

	int ApplicationSession::Request(lua_State *L, Instance *Receiver, SessionCommand Command) {
		auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		Op->Command = Command;
		Op->OwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		Op->Owner = GetCurrentScriptSecurityContext().IssuingScript;
		if (!App) Op->Result = Reply{"NotAuthorized", "The local application owns this operation."};
		else if (lua_gettop(L) > 1 && !lua_isnoneornil(L, 2) &&
			((Command != SessionCommand::StartLocal && Command != SessionCommand::Join) || !lua_istable(L, 2)))
			Op->Result = Reply{"InvalidOptions", "Invalid offline operation options."};
		else if (App->Sessions.Pending) Op->Result = Reply{"Busy", "Another session operation is pending."};
		else if ((Command == SessionCommand::StartLocal || Command == SessionCommand::Join) && App->Sessions.Active)
			Op->Result = Reply{"WrongState", "Leave the active world first."};
		else if (Command != SessionCommand::StartLocal && Command != SessionCommand::Join && !App->Sessions.Active)
			Op->Result = Reply{"NoSession", "No gameplay session is active."};
		else if (App->Sessions.Remote && (Command == SessionCommand::Pause || Command == SessionCommand::Resume))
			Op->Result = Reply{"NotAuthorized", "Gameplay time belongs to the remote authority."};
		else {
			if (Command == SessionCommand::Join) {
				if (!lua_istable(L, 2)) Op->Result = Reply{"InvalidOptions", "A direct target is required."};
				else {
					lua_rawgetfield(L, 2, "Kind");
					const bool Direct = lua_type(L, -1) == LUA_TSTRING && std::string_view(lua_tostring(L, -1)) == "Direct";
					lua_pop(L, 1);
					lua_rawgetfield(L, 2, "Address");
					if (!Direct || lua_type(L, -1) != LUA_TSTRING || lua_objlen(L, -1) > 255)
						Op->Result = Reply{"InvalidOptions", "Expected a bounded direct connection target."};
					else Op->Address.assign(lua_tostring(L, -1), lua_objlen(L, -1));
					lua_pop(L, 1);
				}
			}
			if (Command == SessionCommand::StartLocal && lua_istable(L, 2)) {
				lua_pushnil(L);
				while (lua_next(L, 2)) {
					const auto *Key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
					if (Key && std::string_view(Key) == "SaveId" && lua_type(L, -1) == LUA_TSTRING) {
						size_t Length{};
						const auto *Id = lua_tolstring(L, -1, &Length);
						if (Length == 32) Op->SaveId.assign(Id, Length);
						else Op->Result = Reply{"InvalidOptions", "Invalid world slot identity."};
					} else if (Key && std::string_view(Key) == "PrivateServer" && lua_isboolean(L, -1)) {
						if (lua_toboolean(L, -1)) Op->Result = Reply{"UnsupportedTopology", "Private Server is unavailable."};
					} else Op->Result = Reply{"InvalidOptions", "Unknown world selection option."};
					lua_pop(L, 1);
				}
			}
			if (!Op->SaveId.empty() && App->Sessions.CatalogOperation) Op->Result = Reply{"Busy", "World selection is busy."};
			if (!Op->Result) App->Sessions.Pending = Op;
		}
		return Operation::Push(L, std::move(Op));
	}

	void ApplicationSession::ConfigureCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Graph,
		std::string Baseline, WorldCatalog::SlotProvider Open) {
		Catalog = std::make_shared<WorldCatalog>(std::move(Provider), std::move(Graph), std::move(Baseline), std::move(Open));
	}

	int ApplicationSession::RequestCatalog(lua_State *L, Instance *Receiver, WorldCatalogAction Action) {
		auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		if (!App) Op->Result = Reply{"NotAuthorized", "The local application owns world selection."};
		else if (!App->Sessions.Catalog) Op->Result = Reply{"Unavailable", "World storage is unavailable."};
		else if (App->Sessions.CatalogOperation || (App->Sessions.Pending && !App->Sessions.Pending->SaveId.empty()))
			Op->Result = Reply{"Busy", "A world catalog operation is pending."};
		else {
			const auto ReadString = [&](int Index, bool Optional) -> std::string {
				if (Optional && lua_isnoneornil(L, Index)) return {};
				if (lua_type(L, Index) != LUA_TSTRING || lua_objlen(L, Index) > 256) throw std::invalid_argument("Invalid string");
				size_t Size{};
				const char *Text = lua_tolstring(L, Index, &Size);
				return {Text, Size};
			};
			try {
				std::string Value, Seed;
				if (Action == WorldCatalogAction::Create) {
					if (!lua_istable(L, 2)) throw std::invalid_argument("Invalid create options");
					lua_rawgetfield(L, 2, "Name");
					Value = ReadString(-1, false);
					lua_pop(L, 1);
					lua_rawgetfield(L, 2, "Seed");
					Seed = ReadString(-1, true);
					lua_pop(L, 1);
				} else Value = ReadString(2, Action == WorldCatalogAction::List);
				App->Sessions.Catalog->Begin(Action, std::move(Value), std::move(Seed));
				Op->Page = Action == WorldCatalogAction::List;
				Op->OwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
				Op->Owner = GetCurrentScriptSecurityContext().IssuingScript;
				App->Sessions.CatalogOperation = Op;
			} catch (const std::invalid_argument &) { Op->Result = Reply{"InvalidOptions", "Invalid world catalog arguments."}; }
		}
		return Operation::Push(L, std::move(Op));
	}

	void ApplicationSession::PumpCatalog(ApplicationPresentation &App) {
		if (!CatalogOperation) return;
		auto Op = CatalogOperation;
		if (Op->CancelRequested || !App.IsOwnerCurrent(Op->Owner, Op->OwnerGeneration)) Catalog->Cancel();
		auto Result = Catalog->Poll();
		Op->Irreversible = !Catalog->CanCancel();
		if (!Result) return;
		CatalogOperation.reset();
		Reply Outcome;
		Outcome.Page = Op->Page;
		Outcome.Catalog = std::make_shared<WorldCatalogResult>(std::move(*Result));
		Op->Result = Outcome;
		Op->Completed->Fire(Outcome);
		Op->Completed->DisconnectAll();
	}

	int ApplicationSession::GetInfo(lua_State *L, Instance *Receiver) {
		auto *App = Caller(L, Receiver);
		if (!App) luaL_error(L, "Application session is unavailable");
		const auto &Sessions = App->Sessions;
		const auto Pending = Sessions.Pending;
		const char *State = Sessions.Active ? "Active" : "Idle";
		if (Pending && Pending->Command == SessionCommand::StartLocal) State = "Starting";
		if (Pending && Pending->Command == SessionCommand::Join) State = "Connecting";
		if (Pending && Pending->Command == SessionCommand::Leave) State = "Leaving";
		PushSession(L, Sessions.Active, Sessions.Generation, State, Sessions.ActiveSlot ? &*Sessions.ActiveSlot : nullptr,
			Sessions.Remote, Sessions.Disconnected);
		return 1;
	}

	std::string_view ApplicationSession::GetJoinAddress() const { return Pending ? std::string_view(Pending->Address) : std::string_view{}; }

	bool ApplicationSession::IsLeaving() const {
		return Pending && Pending->Command == SessionCommand::Leave;
	}

	void ApplicationSession::RetireDisconnected() {
		Active = Remote = Paused = false;
		Disconnected = true;
		PauseOwners.clear();
	}

	const WorldSlot *ApplicationSession::GetStartingSlot() const {
		return Pending && Pending->Slot ? &*Pending->Slot : nullptr;
	}

	void ApplicationSession::Commit(ApplicationPresentation &App, const Executor &Execute, const std::function<void()> &DiscardStart) {
		if (std::erase_if(PauseOwners, [&](const auto &Item) { return !App.IsOwnerCurrent(Item.first, Item.second); }) &&
			PauseOwners.empty() && Active && Paused) {
			(void)Execute(SessionCommand::Resume);
			Paused = false;
			App.NotifyPause(false);
		}
		if (!Pending) return;
		const bool PreviouslyPaused = Paused;
		auto Op = Pending;
		Reply Outcome{};
		if (!Op->SaveId.empty() && !Op->Slot && (!Op->CancelRequested || Op->ReadingSlot) && Catalog) {
			try {
				if (!Op->ReadingSlot) {
					Catalog->Begin(WorldCatalogAction::Resolve, Op->SaveId);
					Op->ReadingSlot = true;
				}
				if (Op->CancelRequested) Catalog->Cancel();
				auto Resolved = Catalog->Poll();
				if (!Resolved) return;
				Op->ReadingSlot = false;
				if (!Resolved->Code.empty()) Outcome.Catalog = std::make_shared<WorldCatalogResult>(std::move(*Resolved));
				else if (!Resolved->Items.at(0).Compatible) Outcome = {"Incompatible", "The selected world requires migration."};
				else Op->Slot = Resolved->Items.at(0);
			} catch (const std::invalid_argument &) { Outcome = {"InvalidOptions", "Invalid world slot identity."}; }
		}
		if (Op->CancelRequested) Outcome = {"Cancelled", "Session operation was cancelled."};
		else if (!App.IsOwnerCurrent(Op->Owner, Op->OwnerGeneration)) Outcome = {"Retired", "Session owner retired."};
		else if (!Op->SaveId.empty() && !Catalog) Outcome = {"Unavailable", "World storage is unavailable."};
		else if (Outcome.Succeeded()) {
			try {
				const bool Starting = Op->Command == SessionCommand::StartLocal || Op->Command == SessionCommand::Join;
				if (Starting && !Op->Started) {
					if (Generation == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Session generations exhausted");
					++Generation;
				}
				Op->Started = true;
				const bool Hold = Op->Command == SessionCommand::Pause;
				const bool ChangeHold = Hold || Op->Command == SessionCommand::Resume;
				if (ChangeHold) {
					const bool Effective = Hold || PauseOwners.size() > PauseOwners.count(Op->Owner);
					if (Effective != Paused) (void)Execute(Effective ? SessionCommand::Pause : SessionCommand::Resume);
					if (Hold) PauseOwners[Op->Owner] = Op->OwnerGeneration;
					else PauseOwners.erase(Op->Owner);
					Paused = Effective;
				} else if (!Execute(Op->Command)) return;
				switch (Op->Command) {
				case SessionCommand::StartLocal: case SessionCommand::Join:
					Active = true; Paused = false; ActiveSlot = Op->Slot; Remote = Op->Command == SessionCommand::Join;
					LastEndpoint = Remote ? Op->Address : std::string{};
					Disconnected = false; break;
				case SessionCommand::Leave: Active = Remote = Paused = false; PauseOwners.clear(); ActiveSlot.reset(); break;
				case SessionCommand::Pause: case SessionCommand::Resume: break;
				}
				Outcome = {nullptr, nullptr, Starting || Op->Command == SessionCommand::Leave,
					Active, Paused, Generation};
				Outcome.Slot = ActiveSlot;
				Outcome.Remote = Remote;
			} catch (const WorldInUse &) {
				Outcome = {"SaveInUse", "Another session owns the selected world."};
			} catch (SessionAdmissionFailure Failure) {
				switch (Failure) {
				case SessionAdmissionFailure::UnsupportedTopology: Outcome = {"UnsupportedTopology", "This connection profile is unavailable."}; break;
				case SessionAdmissionFailure::Unavailable: Outcome = {"Unavailable", "The selected world provider is unavailable."}; break;
				case SessionAdmissionFailure::Disconnected: Outcome = {"Disconnected", "The connection closed before the session was ready."}; break;
				case SessionAdmissionFailure::TimedOut: Outcome = {"TimedOut", "The connection did not become ready in time."}; break;
				case SessionAdmissionFailure::InvalidOptions: Outcome = {"InvalidOptions", "The direct connection target is invalid."}; break;
				case SessionAdmissionFailure::SaveFailed: Outcome = {"SaveFailed", "A pending world write failed while leaving."}; break;
				case SessionAdmissionFailure::UnknownOutcome: Outcome = {"UnknownOutcome", "A world write requires reconciliation."}; break;
				}
			} catch (const std::exception &) { Outcome = {"StartupFailed", "The host could not complete the session operation."}; }
		}
		Pending.reset();
		if (!Outcome.Succeeded() && (Op->Command == SessionCommand::StartLocal || Op->Command == SessionCommand::Join)) DiscardStart();
		Op->Result = Outcome;
		if (PreviouslyPaused != Paused) App.NotifyPause(Paused);
		Op->Completed->Fire(Outcome);
		Op->Completed->DisconnectAll();
	}
}
