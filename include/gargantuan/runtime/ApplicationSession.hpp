// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include <map>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct lua_State;
namespace gargantuan {
	class ApplicationPresentation;
	class Instance;
	class DataModel;
	class IPersistenceProvider;
	struct SessionFailure {
		std::string Code, Message, RecoveryId;
	};
	struct SessionSnapshot;
	enum class SessionCommand { StartLocal, Host, Join, Leave, Pause, Resume };
	enum class SessionAdmissionFailure { UnsupportedTopology, Unavailable, Disconnected, TimedOut, InvalidOptions,
		SaveFailed, UnknownOutcome, UnconfirmedLeave, Incompatible, NotAuthorized };
	struct WorldInUse final : std::runtime_error {
		WorldInUse() : std::runtime_error("Another session owns this world") {}
	};

	struct SessionInvitation {
		std::string Source, Id, Name, Address;
		std::chrono::steady_clock::time_point Expires;
	};
	struct SessionListing {
		std::string Id, Name;
		std::chrono::steady_clock::time_point Expires;
		std::string Address, ServerKey;
	};

	class ApplicationSession final {
	  public:
		using CatalogSetup = std::function<void(ApplicationSession &)>;
		explicit ApplicationSession(CatalogSetup Setup = {}, bool CanHost = false) : Setup(std::move(Setup)), CanHost(CanHost) {}
		[[nodiscard]] bool HasCurrentInvitation() const {
			return Invitation && !InvitationConsumed && std::chrono::steady_clock::now() < Invitation->Expires;
		}
		void SetInvitation(std::optional<SessionInvitation> Value) { Invitation = std::move(Value); }
		[[nodiscard]] bool InvitationExpired() const { return Invitation && !HasCurrentInvitation(); }
		void ConsumeInvitation() { InvitationConsumed = true; }
		void ConfigureDirectory(std::string Source) { DirectorySource = std::move(Source); }
		void CloseDirectory() { DirectoryRows.clear(); DirectorySource.clear(); }
		bool OfferSession(SessionListing Listing);
		void WithdrawSession(const std::string &Id) { DirectoryRows.erase(Id); }
		[[nodiscard]] const SessionListing *GetListing(std::string_view Id) const;
		[[nodiscard]] std::string_view GetJoinListing() const;
		[[nodiscard]] bool MatchesSelectedListing(const SessionListing &Listing) const;
		[[nodiscard]] bool MatchesInvitation(std::string_view Source, std::string_view Id) const {
			return Invitation && Invitation->Source == Source && Invitation->Id == Id;
		}
		void RefreshInvitation(std::chrono::milliseconds Lifetime) {
			Invitation->Expires = std::chrono::steady_clock::now() + Lifetime;
			InvitationConsumed = false;
		}
		static int FindServers(lua_State *L, Instance *Receiver);
		using Executor = std::function<bool(SessionCommand)>;
		[[nodiscard]] bool IsActive() const { return Active; }
		[[nodiscard]] bool IsPaused() const { return Active && Paused; }
		[[nodiscard]] bool StartingPrivateServer() const;
		[[nodiscard]] unsigned HostCapacity() const;
		[[nodiscard]] bool IsLeaving() const;
		[[nodiscard]] const WorldSlot *GetStartingSlot() const;
		[[nodiscard]] std::string_view GetJoinAddress() const;
		void RetireDisconnected(ApplicationPresentation &App);
		void ReportRetiredWriteStatus(ApplicationPresentation &App, PersistenceStatus Status);
		void RetireOperations(ApplicationPresentation &App);
		void FinishClose(ApplicationPresentation &App, PersistenceStatus Status);
		void Commit(ApplicationPresentation &App, const Executor &Execute,
			const std::function<PersistenceStatus()> &DiscardStart);
		static int Request(lua_State *L, Instance *Receiver, SessionCommand Command);
		static int RequestReconnect(lua_State *L, Instance *Receiver);
		static int GetInfo(lua_State *L, Instance *Receiver);
		[[nodiscard]] SessionSnapshot GetSnapshot() const;
		static int RequestCatalog(lua_State *L, Instance *Receiver, WorldCatalogAction Action);
		void ConfigureCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Graph,
			std::string Baseline, WorldCatalog::SlotProvider Open);
		void PumpCatalog(ApplicationPresentation &App);
	  private:
		bool OpenCatalog();
		CatalogSetup Setup;
		const bool CanHost;
		struct Operation;
		std::optional<SessionInvitation> Invitation;
		bool InvitationConsumed = false;
		std::shared_ptr<Operation> Pending;
		std::shared_ptr<WorldCatalog> Catalog;
		std::shared_ptr<Operation> CatalogOperation;
		std::optional<WorldSlot> ActiveSlot;
		std::uint64_t Generation = 0;
		bool Active = false;
		bool Remote = false;
		bool Private = false;
		bool Hosting = false;
		bool Retiring = false;
		std::optional<SessionFailure> LastError;
		std::string LastEndpoint;
		std::string DirectorySource, LastListing, LastServerKey;
		std::map<std::string, SessionListing, std::less<>> DirectoryRows;
		bool Paused = false;
		std::map<ObjectId, std::uint64_t> PauseOwners;
	};
}
