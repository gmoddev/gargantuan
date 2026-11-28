// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/runtime/SessionSnapshot.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <limits>
#include <utility>

namespace gargantuan {
	namespace {
		bool StartsSession(SessionCommand Command) {
			return Command == SessionCommand::StartLocal || Command == SessionCommand::Host || Command == SessionCommand::Join;
		}
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
			StringField(L, "Compatibility", Slot.Initializing ? "Initializing" : Slot.Compatible ? "Compatible" : "RequiresMigration");
			lua_setreadonly(L, -1, true);
		}

	namespace {
		void PushSession(lua_State *L, bool Active, std::uint64_t Generation, const char *State, const WorldSlot *Slot = nullptr,
			bool Remote = false, bool Private = false, const SessionFailure *Error = nullptr, bool Hosting = false) {
			lua_createtable(L, 0, 4);
			StringField(L, "State", State);
			StringField(L, "Generation", std::to_string(Generation).c_str());
			const std::string_view Phase(State);
			StringField(L, "World", Phase == "Leaving" ? "Retiring" : Active ? "Ready" :
				Phase == "Starting" || Phase == "Connecting" ? "Preparing" : "Absent");
			if (Active) StringField(L, "Mode", Hosting ? "MultiplayerHost" : Private ? "PrivateHost" : Remote ? "Remote" : "Offline");
			if (Error) {
				lua_createtable(L, 0, 3);
				lua_pushboolean(L, false); lua_setfield(L, -2, "Ok");
				StringField(L, "Code", Error->Code.c_str());
				StringField(L, "Message", Error->Message.c_str());
				if (!Error->RecoveryId.empty()) StringField(L, "RecoveryId", Error->RecoveryId.c_str());
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
			bool Private = false;
			bool ListingPage = false;
			std::vector<SessionListing> Listings;
			std::string ListingCursor;
			std::optional<SessionFailure> LastError;
			bool Hosting = false;

			bool Succeeded() const { return !Code && (!Catalog || Catalog->Code.empty()); }

			int Push(lua_State *L) const {
				lua_createtable(L, 0, 3);
				lua_pushboolean(L, Succeeded());
				lua_setfield(L, -2, "Ok");
				if (Code) {
					StringField(L, "Code", Code);
					StringField(L, "Message", Message);
				} else if (ListingPage) {
					lua_createtable(L, 0, 2);
					lua_createtable(L, static_cast<int>(Listings.size()), 0);
					int Index = 0;
					for (const auto &Listing : Listings) {
						lua_createtable(L, 0, 2);
						StringField(L, "Id", Listing.Id.c_str());
						StringField(L, "Name", Listing.Name.c_str());
						lua_setreadonly(L, -1, true);
						lua_rawseti(L, -2, ++Index);
					}
					lua_setreadonly(L, -1, true);
					lua_setfield(L, -2, "Items");
					if (!ListingCursor.empty()) StringField(L, "NextCursor", ListingCursor.c_str());
					lua_setreadonly(L, -1, true);
					lua_setfield(L, -2, "Value");
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
					if (Session) PushSession(L, Active, Generation, Active ? "Active" : "Idle", Slot ? &*Slot : nullptr, Remote, Private, LastError ? &*LastError : nullptr, Hosting);
					else lua_pushboolean(L, Paused);
					lua_setfield(L, -2, "Value");
				}
				lua_setreadonly(L, -1, true);
				return 1;
			}
		};

		void PushProgress(lua_State *L, const char *Phase) {
			lua_createtable(L, 0, 1);
			StringField(L, "Phase", Phase);
			lua_setreadonly(L, -1, true);
		}

		class ProgressSignal final : public BaseSignal {
		  public:
			Enums::SignalType GetSignalType() override { return Enums::SignalType::Engine; }
			int LPushArgument(lua_State *L, std::any Value) override {
				PushProgress(L, std::any_cast<const char *>(Value));
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
		bool PrivateServer = false;
		unsigned Capacity = 2;
		bool CancelRequested = false;
		bool Irreversible = false;
		bool Announced = false;
		std::string SaveId;
		std::string Address;
		std::string Listing, ServerKey;
		std::optional<WorldSlot> Slot;
		bool ReadingSlot = false;
		bool Page = false;
		std::optional<Reply> Result;
		std::optional<Reply> RetirementOutcome;
		std::shared_ptr<Completion> Completed = std::make_shared<Completion>();
		std::shared_ptr<ProgressSignal> ProgressChanged = std::make_shared<ProgressSignal>();
		const char *Phase = "Validating";

		void Progress(const char *Next) {
			if (std::string_view(Phase) == Next) return;
			Phase = Next;
			ProgressChanged->Fire(Phase);
		}

		void Finish(Reply Outcome) {
			Result = std::move(Outcome);
			if (Result->Succeeded()) Progress("Ready");
			ProgressChanged->DisconnectAll();
			Completed->Fire(*Result);
			Completed->DisconnectAll();
		}

		using Reference = std::shared_ptr<Operation>;

		static Operation &Bound(lua_State *L) {
			return **static_cast<Reference *>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		static int Push(lua_State *L, Reference Value) {
			lua_createtable(L, 0, 7);
			StackValue<std::shared_ptr<ProgressSignal>>::Push(L, Value->ProgressChanged);
			lua_setfield(L, -2, "ProgressChanged");
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
			Method("GetProgress", [](lua_State *L) {
				const auto &Op = Bound(L);
				if (Op.Result && !Op.Result->Succeeded()) lua_pushnil(L);
				else PushProgress(L, Op.Result ? "Ready" : Op.Phase);
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

	bool ApplicationSession::OfferSession(SessionListing Listing) {
		const auto Now = std::chrono::steady_clock::now();
		std::erase_if(DirectoryRows, [&](const auto &Item) { return Item.second.Expires <= Now; });
		if (DirectorySource.empty() || Listing.Expires <= Now || (!DirectoryRows.contains(Listing.Id) && DirectoryRows.size() >= 256))
			return false;
		const auto Id = Listing.Id;
		if (const auto *Current = GetListing(Id); Current &&
			(Current->Address != Listing.Address || Current->ServerKey != Listing.ServerKey)) return false;
		DirectoryRows.insert_or_assign(Id, std::move(Listing));
		return true;
	}
	const SessionListing *ApplicationSession::GetListing(std::string_view Id) const {
		const auto Item = DirectoryRows.find(Id);
		return Item != DirectoryRows.end() && Item->second.Expires > std::chrono::steady_clock::now() ? &Item->second : nullptr;
	}
	std::string_view ApplicationSession::GetJoinListing() const { return Pending ? std::string_view(Pending->Listing) : std::string_view{}; }
	bool ApplicationSession::MatchesSelectedListing(const SessionListing &Listing) const {
		return Pending && Pending->Listing == Listing.Id && Pending->Address == Listing.Address && Pending->ServerKey == Listing.ServerKey;
	}

	int ApplicationSession::FindServers(lua_State *L, Instance *Receiver) {
		const auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		const std::string_view Source = lua_type(L, 2) == LUA_TSTRING ? std::string_view(lua_tostring(L, 2), lua_objlen(L, 2)) : std::string_view{};
		Reply Page;
		const bool HasCursor = !lua_isnoneornil(L, 3);
		const std::string_view Cursor = lua_type(L, 3) == LUA_TSTRING ? std::string_view(lua_tostring(L, 3), lua_objlen(L, 3)) : std::string_view{};
		if (!App) Page = {"NotAuthorized", "The local application owns session discovery."};
		else if (App->IsClosing()) Page = {"WrongState", "The application is closing."};
		else if ((Source != "Node" && Source != "LAN") || (HasCursor && !ProjectId::Parse(Cursor)) || lua_gettop(L) > 3)
			Page = {"InvalidOptions", "Invalid session source or cursor."};
		else if (App->Sessions.DirectorySource == Source) {
			Page.ListingPage = true;
			const auto Now = std::chrono::steady_clock::now();
			for (auto It = App->Sessions.DirectoryRows.upper_bound(Cursor); It != App->Sessions.DirectoryRows.end(); ++It) {
				if (It->second.Expires <= Now) continue;
				if (Page.Listings.size() == 16) { Page.ListingCursor = Page.Listings.back().Id; break; }
				Page.Listings.push_back(It->second);
			}
		}
		else if (!App->Sessions.Invitation || App->Sessions.Invitation->Source != Source)
			Page = {"Unavailable", "The launcher has no directory for this source."};
		else {
			Page.ListingPage = true;
			const auto &Value = *App->Sessions.Invitation;
			if (App->Sessions.HasCurrentInvitation() && Value.Id > Cursor) Page.Listings.push_back({Value.Id, Value.Name, Value.Expires});
		}
		Op->Result = std::move(Page);
		return Operation::Push(L, std::move(Op));
	}

	int ApplicationSession::RequestReconnect(lua_State *L, Instance *Receiver) {
		auto *App = Caller(L, Receiver);
		if (!App || (App->Sessions.LastEndpoint.empty() && App->Sessions.LastListing.empty()) || lua_gettop(L) != 1) {
			auto Op = std::make_shared<Operation>();
			Op->Result = !App ? Reply{"NotAuthorized", "The local application owns this operation."} :
				lua_gettop(L) != 1 ? Reply{"InvalidOptions", "Reconnect takes no options."} :
				Reply{"NotFound", "There is no previous remote session."};
			return Operation::Push(L, std::move(Op));
		}
		if (!App->Sessions.LastListing.empty()) {
			const auto *Row = App->Sessions.GetListing(App->Sessions.LastListing);
			if (Row && (Row->Address != App->Sessions.LastEndpoint || Row->ServerKey != App->Sessions.LastServerKey)) {
				auto Op = std::make_shared<Operation>();
				Op->Result = Reply{"NotAuthorized", "The previous session binding changed."};
				return Operation::Push(L, std::move(Op));
			}
		}
		lua_createtable(L, 0, 2);
		if (App->Sessions.LastListing.empty()) {
			StringField(L, "Kind", "Direct");
			StringField(L, "Address", App->Sessions.LastEndpoint.c_str());
		} else {
			StringField(L, "Kind", "Listing");
			StringField(L, "Id", App->Sessions.LastListing.c_str());
		}
		return Request(L, Receiver, SessionCommand::Join);
	}

	int ApplicationSession::Request(lua_State *L, Instance *Receiver, SessionCommand Command) {
		auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		Op->Command = Command;
		Op->OwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		Op->Owner = GetCurrentScriptSecurityContext().IssuingScript;
		if (!App) Op->Result = Reply{"NotAuthorized", "The local application owns this operation."};
		else if (App->IsClosing()) Op->Result = Reply{"WrongState", "The application is closing."};
		else if (lua_gettop(L) > 2 || (lua_gettop(L) > 1 && !lua_isnoneornil(L, 2) &&
			(!StartsSession(Command) || !lua_istable(L, 2))))
			Op->Result = Reply{"InvalidOptions", "Invalid offline operation options."};
		else if (App->Sessions.Retiring) Op->Result = Reply{"Busy", "The previous connection is retiring."};
		else if (Command == SessionCommand::Host && !App->Sessions.CanHost)
			Op->Result = Reply{"UnsupportedTopology", "The installation has no compatible enrolled multiplayer Server."};
		else if (Command == SessionCommand::Leave && App->Sessions.IsLeaving()) return Operation::Push(L, App->Sessions.Pending);
		else if (App->Sessions.Pending) Op->Result = Reply{"Busy", "Another session operation is pending."};
		else if (Command == SessionCommand::Leave && !App->Sessions.Active) {
			Op->Result = Reply{nullptr, nullptr, true, false, false, App->Sessions.Generation};
			Op->Result->LastError = App->Sessions.LastError;
		}
		else if (StartsSession(Command) && App->Sessions.Active)
			Op->Result = Reply{"WrongState", "Leave the active world first."};
		else if (!StartsSession(Command) && !App->Sessions.Active)
			Op->Result = Reply{"NoSession", "No gameplay session is active."};
		else if ((App->Sessions.Remote || App->Sessions.Hosting) && (Command == SessionCommand::Pause || Command == SessionCommand::Resume))
			Op->Result = Reply{"NotAuthorized", "Gameplay time belongs to the remote authority."};
		else {
			if (Command == SessionCommand::Join) {
				if (!lua_istable(L, 2)) Op->Result = Reply{"InvalidOptions", "A direct target is required."};
				else {
					lua_rawgetfield(L, 2, "Kind");
					const std::string_view Kind = lua_type(L, -1) == LUA_TSTRING ?
						std::string_view(lua_tostring(L, -1), lua_objlen(L, -1)) : std::string_view{};
					const bool Direct = Kind == "Direct", Listing = Kind == "Listing";
					lua_pop(L, 1);
					lua_rawgetfield(L, 2, Listing ? "Id" : "Address");
					if ((!Direct && !Listing) || lua_type(L, -1) != LUA_TSTRING || !lua_objlen(L, -1) || lua_objlen(L, -1) > 255)
						Op->Result = Reply{"InvalidOptions", "Expected a direct address or listing identity."};
					else if (Listing) {
						const std::string Id(lua_tostring(L, -1), lua_objlen(L, -1));
						if (const auto *Row = App->Sessions.GetListing(Id)) {
							Op->Listing = Id;
							Op->Address = Row->Address;
							Op->ServerKey = Row->ServerKey;
						}
						else if (!App->Sessions.HasCurrentInvitation() || App->Sessions.Invitation->Id != Id)
							Op->Result = Reply{"NotFound", "This session invitation is no longer available."};
						else Op->Address = App->Sessions.Invitation->Address;
					}
					else Op->Address.assign(lua_tostring(L, -1), lua_objlen(L, -1));
					lua_pop(L, 1);
				}
			}
			if ((Command == SessionCommand::StartLocal || Command == SessionCommand::Host) && lua_istable(L, 2)) {
				lua_pushnil(L);
				while (lua_next(L, 2)) {
					const std::string_view Key = lua_type(L, -2) == LUA_TSTRING ?
						std::string_view(lua_tostring(L, -2), lua_objlen(L, -2)) : std::string_view{};
					if (Key == "SaveId" && lua_type(L, -1) == LUA_TSTRING) {
						size_t Length{};
						const auto *Id = lua_tolstring(L, -1, &Length);
						if (Length == 32) Op->SaveId.assign(Id, Length);
						else Op->Result = Reply{"InvalidOptions", "Invalid world slot identity."};
					} else if (Command == SessionCommand::Host && Key == "MaxPlayers" && lua_type(L, -1) == LUA_TNUMBER) {
						const double Value = lua_tonumber(L, -1);
						if (Value >= 2 && Value <= 8 && Value == static_cast<unsigned>(Value)) Op->Capacity = static_cast<unsigned>(Value);
						else Op->Result = Reply{"InvalidOptions", "An independent host supports two through eight players."};
					} else if (Command == SessionCommand::Host && Key == "Discovery" && lua_type(L, -1) == LUA_TSTRING) {
						const std::string_view Source(lua_tostring(L, -1), lua_objlen(L, -1));
						if (Source == "Node") Op->Result = Reply{"UnsupportedTopology", "Node owns server deployment for this source."};
						else if (Source != "LAN") Op->Result = Reply{"InvalidOptions", "Unknown hosting source."};
					} else if (Command == SessionCommand::StartLocal && Key == "PrivateServer" && lua_isboolean(L, -1)) {
						Op->PrivateServer = lua_toboolean(L, -1);
					} else Op->Result = Reply{"InvalidOptions", "Unknown world selection option."};
					lua_pop(L, 1);
				}
			}
			if (!Op->SaveId.empty() && !App->Sessions.OpenCatalog()) Op->Result = Reply{"Unavailable", "World storage is unavailable."};
			if (!Op->SaveId.empty() && App->Sessions.CatalogOperation) Op->Result = Reply{"Busy", "World selection is busy."};
			if (!Op->Result) {
				if (StartsSession(Command)) {
					if (App->Sessions.Generation == std::numeric_limits<std::uint64_t>::max()) {
						Op->Result = Reply{"Unavailable", "Session generations exhausted."};
						return Operation::Push(L, std::move(Op));
					}
					App->Sessions.LastError.reset();
					++App->Sessions.Generation;
				}
				Op->Irreversible = Command == SessionCommand::Leave;
				App->Sessions.Pending = Op;
			}
		}
		return Operation::Push(L, std::move(Op));
	}

	bool ApplicationSession::OpenCatalog() {
		if (!Catalog && Setup) {
			try { Setup(*this); }
			catch (const std::exception &) { return false; }
		}
		return Catalog != nullptr;
	}

	void ApplicationSession::ConfigureCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Graph,
		std::string Baseline, WorldCatalog::SlotProvider Open) {
		Catalog = std::make_shared<WorldCatalog>(std::move(Provider), std::move(Graph), std::move(Baseline), std::move(Open));
	}

	int ApplicationSession::RequestCatalog(lua_State *L, Instance *Receiver, WorldCatalogAction Action) {
		auto *App = Caller(L, Receiver);
		auto Op = std::make_shared<Operation>();
		if (!App) Op->Result = Reply{"NotAuthorized", "The local application owns world selection."};
		else if (App->IsClosing()) Op->Result = Reply{"WrongState", "The application is closing."};
		else if (!App->Sessions.OpenCatalog()) Op->Result = Reply{"Unavailable", "World storage is unavailable."};
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
		Op->Progress("OpeningSave");
		auto Result = Catalog->Poll();
		Op->Irreversible = !Catalog->CanCancel();
		if (!Result) return;
		CatalogOperation.reset();
		Reply Outcome;
		Outcome.Page = Op->Page;
		Outcome.Catalog = std::make_shared<WorldCatalogResult>(std::move(*Result));
		Op->Finish(std::move(Outcome));
	}

	SessionSnapshot ApplicationSession::GetSnapshot() const {
		const char *State = Active ? "Active" : "Idle";
		if (Pending && (Pending->Command == SessionCommand::StartLocal || Pending->Command == SessionCommand::Host)) State = "Starting";
		if (Pending && Pending->Command == SessionCommand::Join) State = "Connecting";
		if (Retiring || (Pending && Pending->Command == SessionCommand::Leave)) State = "Leaving";
		return {Active, Generation, State, ActiveSlot, Remote, Private, Hosting, LastError};
	}

	int StackValue<SessionSnapshot>::Push(lua_State *L, const SessionSnapshot &Value) {
		PushSession(L, Value.Active, Value.Generation, Value.State, Value.Slot ? &*Value.Slot : nullptr,
			Value.Remote, Value.Private, Value.LastError ? &*Value.LastError : nullptr, Value.Hosting);
		return 1;
	}

	int ApplicationSession::GetInfo(lua_State *L, Instance *Receiver) {
		auto *App = Caller(L, Receiver);
		if (!App) luaL_error(L, "Application session is unavailable");
		return StackValue<SessionSnapshot>::Push(L, App->Sessions.GetSnapshot());
	}

	bool ApplicationSession::StartingPrivateServer() const { return Pending && Pending->PrivateServer; }
	unsigned ApplicationSession::HostCapacity() const { return Pending && Pending->Command == SessionCommand::Host ? Pending->Capacity : 0; }

	std::string_view ApplicationSession::GetJoinAddress() const { return Pending ? std::string_view(Pending->Address) : std::string_view{}; }

	bool ApplicationSession::IsLeaving() const {
		return Pending && Pending->Command == SessionCommand::Leave;
	}

	void ApplicationSession::RetireOperations(ApplicationPresentation &App) {
		if (Pending && Pending->Command != SessionCommand::Leave) {
			auto Op = std::exchange(Pending, {});
			const bool Transition = StartsSession(Op->Command);
			Reply Outcome{"Retired", "Application closed before the operation completed."};
			if (Transition) {
				LastError = SessionFailure{Outcome.Code, Outcome.Message};
				App.NotifySession();
			}
			Op->Finish(std::move(Outcome));
		}
		if (auto Op = std::exchange(CatalogOperation, {})) {
			const auto Recovery = Catalog->GetRecoveryId();
			Catalog->Cancel();
			Reply Outcome;
			Outcome.Catalog = std::make_shared<WorldCatalogResult>(WorldCatalogResult{
				.Code = Recovery.empty() ? "Retired" : "UnknownOutcome", .RecoveryId = Recovery});
			Op->Finish(std::move(Outcome));
		}
	}

	void ApplicationSession::FinishClose(ApplicationPresentation &App, PersistenceStatus Status) {
		if (!Active && !Pending) return;
		auto Op = std::exchange(Pending, {});
		const bool WasPaused = Paused;
		Active = Remote = Private = Hosting = Paused = false;
		PauseOwners.clear();
		ActiveSlot.reset();
		Reply Outcome{nullptr, nullptr, true, false, false, Generation};
		if (Status == PersistenceStatus::UnknownOutcome || Status == PersistenceStatus::Pending)
			Outcome = {"UnknownOutcome", "The final world write could not be confirmed."};
		else if (Status != PersistenceStatus::Ok)
			Outcome = {"SaveFailed", "A pending world write failed while closing."};
		if (Outcome.Code) LastError = SessionFailure{Outcome.Code, Outcome.Message};
		else LastError.reset();
		App.NotifySession();
		if (WasPaused) App.NotifyPause(false);
		if (Op) Op->Finish(std::move(Outcome));
	}

	void ApplicationSession::RetireDisconnected(ApplicationPresentation &App) {
		const bool WasPaused = Paused;
		auto Op = std::exchange(Pending, {});
		Retiring = true;
		LastError = SessionFailure{"Disconnected", "The connection closed during the session."};
		App.NotifySession();
		Active = Remote = Private = Hosting = Paused = Retiring = false;
		ActiveSlot.reset();
		PauseOwners.clear();
		App.NotifySession();
		if (WasPaused) App.NotifyPause(false);
		if (Op) {
			Reply Outcome{"Disconnected", "The connection closed during the session."};
			Op->Finish(std::move(Outcome));
		}
	}

	void ApplicationSession::ReportRetiredWriteStatus(ApplicationPresentation &App, PersistenceStatus Status) {
		LastError = Status == PersistenceStatus::Unavailable ?
			SessionFailure{"SaveFailed", "The retired private server could not save its world."} :
			SessionFailure{"UnknownOutcome", "The retired private server did not confirm its final write."};
		App.NotifySession();
	}

	const WorldSlot *ApplicationSession::GetStartingSlot() const {
		return Pending && Pending->Slot ? &*Pending->Slot : nullptr;
	}

	void ApplicationSession::Commit(ApplicationPresentation &App, const Executor &Execute,
		const std::function<PersistenceStatus()> &DiscardStart) {
		std::erase_if(PauseOwners, [&](const auto &Item) { return !App.IsOwnerCurrent(Item.first, Item.second); });
		if (PauseOwners.empty() && Active && Paused) {
			if (!Execute(SessionCommand::Resume)) return;
			Paused = false;
			App.NotifyPause(false);
		}
		if (!Pending) return;
		const bool PreviouslyPaused = Paused;
		auto Op = Pending;
		const bool SessionTransition = StartsSession(Op->Command) || Op->Command == SessionCommand::Leave;
		if (SessionTransition && !std::exchange(Op->Announced, true)) App.NotifySession();
		if (App.IsClosing()) return;
		const auto FinishDiscard = [&](Reply &Outcome) {
			const auto Status = DiscardStart();
			if (Status == PersistenceStatus::Pending) return false;
			if (Status == PersistenceStatus::UnknownOutcome)
				Outcome = {"UnknownOutcome", "A startup world write requires reconciliation."};
			else if (Status != PersistenceStatus::Ok)
				Outcome = {"SaveFailed", "A startup world write failed while retiring."};
			return true;
		};
		if (Op->RetirementOutcome) {
			auto Outcome = *Op->RetirementOutcome;
			if (!FinishDiscard(Outcome)) return;
			Pending.reset();
			if (Outcome.Code) LastError = SessionFailure{Outcome.Code, Outcome.Message};
			else if (Outcome.Catalog) LastError = SessionFailure{Outcome.Catalog->Code,
				"World storage could not complete the requested operation.", Outcome.Catalog->RecoveryId};
			App.NotifySession();
			Op->Finish(std::move(Outcome));
			return;
		}
		Reply Outcome{};
		if (!Op->SaveId.empty() && !Op->Slot && (!Op->CancelRequested || Op->ReadingSlot) && Catalog) {
			try {
				if (!Op->ReadingSlot) {
					Op->Progress("OpeningSave");
					if (App.IsClosing()) return;
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
		else if (!Op->Irreversible && !App.IsOwnerCurrent(Op->Owner, Op->OwnerGeneration)) Outcome = {"Retired", "Session owner retired."};
		else if (!Op->SaveId.empty() && !Catalog) Outcome = {"Unavailable", "World storage is unavailable."};
		else if (Outcome.Succeeded()) {
			try {
				const bool Starting = StartsSession(Op->Command);
				const bool Hold = Op->Command == SessionCommand::Pause;
				const bool ChangeHold = Hold || Op->Command == SessionCommand::Resume;
				if (ChangeHold) {
					const bool Effective = Hold || PauseOwners.size() > PauseOwners.count(Op->Owner);
					if (Effective != Paused) {
						Op->Irreversible = true;
						if (!Execute(Effective ? SessionCommand::Pause : SessionCommand::Resume)) return;
					}
					if (Hold) PauseOwners[Op->Owner] = Op->OwnerGeneration;
					else PauseOwners.erase(Op->Owner);
					Paused = Effective;
				} else {
					if (Op->Command == SessionCommand::Leave) {
						Op->Irreversible = true;
						Op->Progress("Retiring");
					} else Op->Progress(Op->Command == SessionCommand::Join ? "Connecting" :
						(Op->PrivateServer || Op->Command == SessionCommand::Host) ? "StartingHost" : "Loading");
					if (App.IsClosing()) return;
					if (Op->CancelRequested) Outcome = {"Cancelled", "Session operation was cancelled."};
					else if (!Execute(Op->Command)) return;
				}
				if (Outcome.Succeeded()) switch (Op->Command) {
				case SessionCommand::StartLocal: case SessionCommand::Join: case SessionCommand::Host:
					Hosting = Op->Command == SessionCommand::Host;
					Active = true; Paused = false; ActiveSlot = Op->Slot; Private = Op->PrivateServer; Remote = Op->Command == SessionCommand::Join;
					LastEndpoint = Remote ? Op->Address : std::string{};
					LastListing = Remote ? Op->Listing : std::string{};
					LastServerKey = Remote ? Op->ServerKey : std::string{};
					break;
				case SessionCommand::Leave: Active = Remote = Private = Hosting = Paused = false; PauseOwners.clear(); ActiveSlot.reset(); break;
				case SessionCommand::Pause: case SessionCommand::Resume: break;
				}
				Outcome = {nullptr, nullptr, Starting || Op->Command == SessionCommand::Leave,
					Active, Paused, Generation};
				Outcome.Slot = ActiveSlot;
				Outcome.Remote = Remote;
				Outcome.Private = Private;
				Outcome.Hosting = Hosting;
			} catch (const WorldInUse &) {
				Outcome = {"SaveInUse", "Another session owns the selected world."};
			} catch (SessionAdmissionFailure Failure) {
				switch (Failure) {
				case SessionAdmissionFailure::UnsupportedTopology: Outcome = {"UnsupportedTopology", "This connection profile is unavailable."}; break;
				case SessionAdmissionFailure::Unavailable: Outcome = {"Unavailable", "The selected world provider is unavailable."}; break;
				case SessionAdmissionFailure::Disconnected: Outcome = {"Disconnected", "The connection closed before the session was ready."}; break;
				case SessionAdmissionFailure::TimedOut: Outcome = {"TimedOut", "The connection did not become ready in time."}; break;
				case SessionAdmissionFailure::Incompatible: Outcome = {"Incompatible", "The server world baseline differs from the selected save."}; break;
				case SessionAdmissionFailure::InvalidOptions: Outcome = {"InvalidOptions", "The direct connection target is invalid."}; break;
				case SessionAdmissionFailure::NotAuthorized: Outcome = {"NotAuthorized", "A fresh native grant for this destination is required."}; break;
				case SessionAdmissionFailure::SaveFailed: Outcome = {"SaveFailed", "A pending world write failed while leaving."}; break;
				case SessionAdmissionFailure::UnknownOutcome: Outcome = {"UnknownOutcome", "A world write requires reconciliation."}; break;
				case SessionAdmissionFailure::UnconfirmedLeave:
					Active = Remote = Private = Hosting = Paused = false;
					PauseOwners.clear();
					ActiveSlot.reset();
					Outcome = {"UnknownOutcome", "The private server retired without confirming its final write status."};
					break;
				}
			} catch (const std::exception &) { Outcome = {"StartupFailed", "The host could not complete the session operation."}; }
		}
		if (!Outcome.Succeeded() && StartsSession(Op->Command)) {
			Op->RetirementOutcome = Outcome;
			if (!FinishDiscard(Outcome)) return;
		}
		Pending.reset();
		if (SessionTransition) {
			if (Outcome.Code) LastError = SessionFailure{Outcome.Code, Outcome.Message};
			else if (!Outcome.Succeeded()) LastError = SessionFailure{Outcome.Catalog->Code,
				"World storage could not complete the requested operation.", Outcome.Catalog->RecoveryId};
			else LastError.reset();
			App.NotifySession();
		}
		if (PreviouslyPaused != Paused) App.NotifyPause(Paused);
		Op->Finish(std::move(Outcome));
	}
}
