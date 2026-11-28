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
	enum class SessionCommand { StartLocal, Join, Leave, Pause, Resume };
	enum class SessionAdmissionFailure { UnsupportedTopology, Unavailable, Disconnected, TimedOut, InvalidOptions,
		SaveFailed, UnknownOutcome };
	struct WorldInUse final : std::runtime_error {
		WorldInUse() : std::runtime_error("Another session owns this world") {}
	};

	class ApplicationSession final {
	  public:
		using Executor = std::function<bool(SessionCommand)>;
		[[nodiscard]] bool IsActive() const { return Active; }
		[[nodiscard]] bool IsPaused() const { return Active && Paused; }
		[[nodiscard]] bool IsLeaving() const;
		[[nodiscard]] const WorldSlot *GetStartingSlot() const;
		[[nodiscard]] std::string_view GetJoinAddress() const;
		void RetireDisconnected();
		void Commit(ApplicationPresentation &App, const Executor &Execute, const std::function<void()> &DiscardStart);
		static int Request(lua_State *L, Instance *Receiver, SessionCommand Command);
		static int RequestReconnect(lua_State *L, Instance *Receiver);
		static int GetInfo(lua_State *L, Instance *Receiver);
		static int RequestCatalog(lua_State *L, Instance *Receiver, WorldCatalogAction Action);
		void ConfigureCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Graph,
			std::string Baseline, WorldCatalog::SlotProvider Open);
		void PumpCatalog(ApplicationPresentation &App);
	  private:
		struct Operation;
		std::shared_ptr<Operation> Pending;
		std::shared_ptr<WorldCatalog> Catalog;
		std::shared_ptr<Operation> CatalogOperation;
		std::optional<WorldSlot> ActiveSlot;
		std::uint64_t Generation = 0;
		bool Active = false;
		bool Remote = false;
		bool Disconnected = false;
		std::string LastEndpoint;
		bool Paused = false;
		std::map<ObjectId, std::uint64_t> PauseOwners;
	};
}
