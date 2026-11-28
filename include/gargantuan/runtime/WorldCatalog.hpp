// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/persistence/PersistenceRuntime.hpp"
#include <filesystem>
#include <functional>

struct lua_State;
namespace gargantuan {
	struct WorldSlot {
		std::string SaveId, WorldId, Name, Seed;
		std::string Baseline;
		bool Compatible = true;
		bool Initializing = false;
	};
	enum class WorldCatalogAction { List, Create, Resolve };
	void PushWorldSlot(lua_State *L, const WorldSlot &Slot);
	struct WorldCatalogResult {
		std::string Code, RecoveryId, NextCursor;
		std::vector<WorldSlot> Items;
	};
	// One bounded catalog root is committed using the local provider's revision CAS.
	// Each selected session uses its own provider database and writer ownership.
	class WorldCatalog final {
	  public:
		using SlotProvider = std::function<std::shared_ptr<IPersistenceProvider>(const WorldSlot &, bool Create)>;
		WorldCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Application,
			std::string Baseline, SlotProvider Open);
		[[nodiscard]] bool CanCancel() const { return !Writing; }
		void Begin(WorldCatalogAction Action, std::string Argument, std::string Seed = {});
		[[nodiscard]] std::string GetRecoveryId() const { return Writing ? NewSlot.SaveId : std::string{}; }
		void Cancel();
		std::optional<WorldCatalogResult> Poll();
	  private:
		PersistenceRuntime Storage;
		std::weak_ptr<DataModel> Application;
		std::string Baseline;
		SlotProvider Open;
		std::unique_ptr<PersistenceRuntime> SlotStorage;
		enum class Phase { Reading, Reserving, Opening, ReadingReservation, Publishing };
		Phase CurrentPhase = Phase::Reading;
		std::shared_ptr<PersistenceHandle> Handle;
		WorldCatalogAction Action{};
		WorldSlot NewSlot;
		std::string Argument;
		bool Writing = false, Cancelled = false;
	};
}
