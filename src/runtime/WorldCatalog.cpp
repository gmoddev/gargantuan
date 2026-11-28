// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "serialization/JsonCodec.hpp"
#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace gargantuan {
	namespace {
		using Json = JsonCodec::Json;
		constexpr std::size_t MaximumSlots = 128, PageSize = 10;
		PersistenceRequest RootRequest() {
			return {.Store = "ApplicationWorldCatalog", .Key = "Slots"};
		}
		bool ValidName(const std::string &Text) {
			return !Text.empty() && Text.size() <= 96 && IsPersistenceUtf8(Text) &&
				std::ranges::none_of(Text, [](unsigned char Byte) { return Byte < 32 || Byte == 127; });
		}
		bool ValidSeed(const std::string &Text) {
			return Text.size() <= 128 && IsPersistenceUtf8(Text) && Text.find('\0') == std::string::npos;
		}
		std::vector<WorldSlot> Decode(const PersistenceRecord &Record, const std::string &Baseline) {
			const auto &Bytes = std::get<PersistenceBytes>(Record.Payload);
			const auto Document = JsonCodec::Parse({reinterpret_cast<const char *>(Bytes.data()), Bytes.size()},
				PersistenceMaxValueBytes, "world catalog");
			if (!Document || Document->at("Version") != 1 || !Document->at("Slots").is_array() ||
				Document->at("Slots").size() > MaximumSlots) throw std::invalid_argument("Unsupported catalog");
			std::vector<WorldSlot> Slots;
			std::unordered_set<std::string> Ids;
			for (const auto &Row : Document->at("Slots")) {
				WorldSlot Slot{Row.at("SaveId").get<std::string>(), Row.at("WorldId").get<std::string>(),
					Row.at("Name").get<std::string>(), Row.at("Seed").get<std::string>()};
				Slot.Initializing = Row.value("Initializing", false);
				Slot.Baseline = Row.value("Baseline", "");
				Slot.Compatible = Slot.Baseline == Baseline && !Baseline.empty();
				if (!ProjectId::Parse(Slot.SaveId) || !ProjectId::Parse(Slot.WorldId) || !Ids.insert(Slot.SaveId).second ||
					!ValidName(Slot.Name) || !ValidSeed(Slot.Seed)) throw std::invalid_argument("Invalid world slot");
				Slots.push_back(std::move(Slot));
			}
			return Slots;
		}
		PersistenceBytes Encode(const std::vector<WorldSlot> &Slots) {
			Json Rows = Json::array();
			for (const auto &Slot : Slots) Rows.push_back({{"SaveId", Slot.SaveId}, {"WorldId", Slot.WorldId},
				{"Name", Slot.Name}, {"Seed", Slot.Seed}, {"Baseline", Slot.Baseline}, {"Initializing", Slot.Initializing}});
			const auto Text = Json{{"Version", 1}, {"Slots", std::move(Rows)}}.dump();
			const auto *Begin = reinterpret_cast<const std::byte *>(Text.data());
			return {Begin, Begin + Text.size()};
		}
		const char *Code(PersistenceStatus Status) {
			switch (Status) {
			case PersistenceStatus::Cancelled: return "Cancelled";
			case PersistenceStatus::Conflict: case PersistenceStatus::Busy: return "Busy";
			case PersistenceStatus::UnknownOutcome: return "UnknownOutcome";
			case PersistenceStatus::Deadline: return "TimedOut";
			default: return "Unavailable";
			}
		}
	}
	WorldCatalog::WorldCatalog(std::shared_ptr<IPersistenceProvider> Provider, std::shared_ptr<DataModel> Graph,
		std::string BaselineValue, SlotProvider OpenValue)
		: Storage(std::move(Provider), Graph), Application(Graph), Baseline(std::move(BaselineValue)), Open(std::move(OpenValue)) {}

	void WorldCatalog::Begin(WorldCatalogAction Requested, std::string Value, std::string Seed) {
		if (Handle) throw std::logic_error("World catalog is busy");
		Action = Requested;
		Argument = std::move(Value);
		CurrentPhase = Phase::Reading;
		Writing = false;
		Cancelled = false;
		if (Action == WorldCatalogAction::Create) {
			if (!ValidName(Argument) || !ValidSeed(Seed)) throw std::invalid_argument("Invalid world name or seed");
			NewSlot = {ProjectId::New().ToString(), ProjectId::New().ToString(), Argument, std::move(Seed), Baseline};
		} else if (Action == WorldCatalogAction::Resolve && !ProjectId::Parse(Argument))
			throw std::invalid_argument("Invalid save identity");
		Handle = Storage.Submit(RootRequest());
	}
	void WorldCatalog::Cancel() {
		Cancelled = true;
		if (Handle && CanCancel()) (void)Handle->Cancel();
	}
	std::optional<WorldCatalogResult> WorldCatalog::Poll() {
		if (!Handle) return std::nullopt;
		if (SlotStorage) SlotStorage->Pump();
		else Storage.Pump();
		const auto &Current = Handle->Poll();
		if (Current.Status == PersistenceStatus::Pending) return std::nullopt;
		auto Terminal = std::move(Handle);
		const auto OpenReserved = [&] {
			CurrentPhase = Phase::Opening;
			SlotStorage = std::make_unique<PersistenceRuntime>(Open(NewSlot, true), Application);
			// The provider verifies its private profile identity on open. This read
			// merely forces that open on the worker. Its value belongs to gameplay.
			Handle = SlotStorage->Submit({.Store = "WorldAdmission", .Key = "Profile"});
		};
		if (CurrentPhase == Phase::Reserving) {
			if (Current.Status != PersistenceStatus::Ok) return WorldCatalogResult{.Code = Code(Current.Status),
				.RecoveryId = Current.Status == PersistenceStatus::UnknownOutcome ? NewSlot.SaveId : ""};
			OpenReserved();
			return std::nullopt;
		}
		if (CurrentPhase == Phase::Opening) {
			SlotStorage.reset();
			if (Current.Status != PersistenceStatus::Ok && Current.Status != PersistenceStatus::Missing)
				return WorldCatalogResult{.Code = Code(Current.Status), .RecoveryId = NewSlot.SaveId};
			CurrentPhase = Phase::ReadingReservation;
			Handle = Storage.Submit(RootRequest());
			return std::nullopt;
		}
		if (CurrentPhase == Phase::Publishing) {
			if (Current.Status == PersistenceStatus::Ok) return WorldCatalogResult{.Items = {NewSlot}};
			return WorldCatalogResult{.Code = Code(Current.Status), .RecoveryId = NewSlot.SaveId};
		}
		if (Cancelled && !Writing) return WorldCatalogResult{.Code = "Cancelled"};
		if (Current.Status != PersistenceStatus::Ok && Current.Status != PersistenceStatus::Missing)
			return WorldCatalogResult{.Code = Code(Current.Status)};
		const auto Revision = Current.Record ? Current.Record->Revision : 0;
		std::vector<WorldSlot> Slots;
		try { if (Current.Record) Slots = Decode(*Current.Record, Baseline); }
		catch (const std::exception &) { return WorldCatalogResult{.Code = "Incompatible"}; }
		const auto StoreSlots = [&](Phase Next) {
			auto Request = RootRequest();
			Request.Operation = PersistenceOperation::Put;
			Request.ExpectedRevision = Revision;
			Request.Payload = Encode(Slots);
			Writing = true;
			CurrentPhase = Next;
			Handle = Storage.Submit(std::move(Request));
		};
		if (CurrentPhase == Phase::ReadingReservation) {
			auto Found = std::ranges::find(Slots, NewSlot.SaveId, &WorldSlot::SaveId);
			if (Found == Slots.end() || Found->WorldId != NewSlot.WorldId)
				return WorldCatalogResult{.Code = "Incompatible", .RecoveryId = NewSlot.SaveId};
			if (!Found->Initializing) return WorldCatalogResult{.Items = {*Found}};
			Found->Initializing = false;
			NewSlot = *Found;
			StoreSlots(Phase::Publishing);
			return std::nullopt;
		}
		if (Action == WorldCatalogAction::Create) {
			if (Slots.size() == MaximumSlots) return WorldCatalogResult{.Code = "StorageFull"};
			NewSlot.Initializing = true;
			Slots.push_back(NewSlot);
			StoreSlots(Phase::Reserving);
			return std::nullopt;
		}
		if (Action == WorldCatalogAction::Resolve) {
			auto Found = std::ranges::find(Slots, Argument, &WorldSlot::SaveId);
			if (Found == Slots.end()) return WorldCatalogResult{.Code = "NotFound"};
			if (Found->Initializing && Found->Compatible) {
				NewSlot = *Found;
				Writing = true;
				OpenReserved();
				return std::nullopt;
			}
			return WorldCatalogResult{.Items = {*Found}};
		}
		// Interrupted creates remain discoverable by their original identity.
		// Selecting an Initializing slot reopens the reservation before publishing it.
		std::size_t Offset = 0;
		if (!Argument.empty()) {
			const auto Prefix = std::to_string(Revision) + "/";
			if (!Argument.starts_with(Prefix)) return WorldCatalogResult{.Code = "Incompatible"};
			const auto *End = Argument.data() + Argument.size();
			const auto Parsed = std::from_chars(Argument.data() + Prefix.size(), End, Offset);
			if (Parsed.ec != std::errc{} || Parsed.ptr != End || Offset >= Slots.size())
				return WorldCatalogResult{.Code = "InvalidOptions"};
		}
		const auto Next = std::min(Slots.size(), Offset + PageSize);
		return WorldCatalogResult{.NextCursor = Next < Slots.size() ? std::to_string(Revision) + "/" + std::to_string(Next) : "",
			.Items = {Slots.begin() + Offset, Slots.begin() + Next}};
	}
}
