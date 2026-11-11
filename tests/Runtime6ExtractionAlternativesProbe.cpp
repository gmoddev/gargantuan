// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;

	struct SourceState {
		CFrame Frame;
		glm::vec3 Size;
		glm::vec4 Color;
		RenderGeometry Geometry = RenderGeometry::Block;
		bool CastShadow = true;
	};

	struct SoASlots {
		std::vector<std::uint32_t> Generations;
		std::vector<CFrame> Frames;
		std::vector<glm::vec3> Sizes;
		std::vector<glm::vec4> Colors;
		std::vector<RenderGeometry> Geometries;
		std::vector<std::uint8_t> CastShadows;

		void Set(ObjectId Object, const SourceState &State) {
			const auto Required = static_cast<std::size_t>(Object.Slot) + 1;
			if (Generations.size() < Required) {
				Generations.resize(Required);
				Frames.resize(Required);
				Sizes.resize(Required);
				Colors.resize(Required);
				Geometries.resize(Required);
				CastShadows.resize(Required);
			}
			Generations[Object.Slot] = Object.Generation;
			Frames[Object.Slot] = State.Frame;
			Sizes[Object.Slot] = State.Size;
			Colors[Object.Slot] = State.Color;
			Geometries[Object.Slot] = State.Geometry;
			CastShadows[Object.Slot] = State.CastShadow;
		}

		void Erase(ObjectId Object) {
			if (Object.Slot < Generations.size() && Generations[Object.Slot] == Object.Generation)
				Generations[Object.Slot] = 0;
		}

		std::optional<SourceState> Get(ObjectId Object) const {
			if (Object.Slot >= Generations.size() || Generations[Object.Slot] != Object.Generation)
				return std::nullopt;
			return SourceState{Frames[Object.Slot], Sizes[Object.Slot], Colors[Object.Slot],
				Geometries[Object.Slot], CastShadows[Object.Slot] != 0};
		}
	};

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	RenderGeometry GeometryOf(Enums::PartType Shape) {
		switch (Shape) {
			case Enums::PartType::Block: return RenderGeometry::Block;
			case Enums::PartType::Ball: return RenderGeometry::Ball;
			case Enums::PartType::Cylinder: return RenderGeometry::Cylinder;
			case Enums::PartType::Wedge: return RenderGeometry::Wedge;
			case Enums::PartType::CornerWedge: return RenderGeometry::CornerWedge;
		}
		throw std::runtime_error("[Runtime6:ExtractionProbe] Unsupported Part geometry");
	}

	SourceState ReadSource(const Part &Value) {
		return {Value.GetRenderCFrame(), Value.GetSize(),
			glm::vec4(static_cast<glm::vec3>(Value.GetColor()), 1.0f - Value.GetTransparency()),
			GeometryOf(Value.GetShape()), Value.GetCastShadow()};
	}

	RenderItem Project(ObjectId Object, const SourceState &State) {
		const auto Model = glm::translate(glm::mat4(1.0f), State.Frame.Position) *
			glm::mat4(State.Frame.Rotation) * glm::scale(glm::mat4(1.0f), State.Size);
		return {Object, State.Geometry, Model, glm::inverse(Model), State.Color, State.CastShadow};
	}

	bool Equal(const RenderItem &Left, const RenderItem &Right) {
		return Left.Object == Right.Object && Left.Geometry == Right.Geometry &&
			Left.ModelMatrix == Right.ModelMatrix && Left.InverseModelMatrix == Right.InverseModelMatrix &&
			Left.Color == Right.Color && Left.CastShadow == Right.CastShadow;
	}

	std::shared_ptr<Part> Resolve(ObjectId Object) {
		auto Value = ObjectRegistry::Get().Lookup(Object);
		auto PartValue = std::dynamic_pointer_cast<Part>(Value);
		return PartValue && !PartValue->GetDestroyed() && !PartValue->IsDestroying() ? PartValue : nullptr;
	}

	std::vector<std::optional<RenderItem>> Direct(const std::vector<ObjectId> &Objects) {
		std::vector<std::optional<RenderItem>> Output;
		Output.reserve(Objects.size());
		for (const auto Object : Objects) {
			auto Value = Resolve(Object);
			Output.emplace_back(Value ? std::optional(Project(Object, ReadSource(*Value))) : std::nullopt);
		}
		return Output;
	}

	void SynchronizeSoA(SoASlots &Slots, const std::vector<ObjectId> &Objects) {
		for (const auto Object : Objects) {
			auto Value = Resolve(Object);
			if (Value) Slots.Set(Object, ReadSource(*Value));
			else Slots.Erase(Object);
		}
	}

	std::vector<std::optional<RenderItem>> ConsumeSoA(
		const SoASlots &Slots, const std::vector<ObjectId> &Objects
	) {
		std::vector<std::optional<RenderItem>> Output;
		Output.reserve(Objects.size());
		for (const auto Object : Objects) {
			auto State = Slots.Get(Object);
			Output.emplace_back(State ? std::optional(Project(Object, *State)) : std::nullopt);
		}
		return Output;
	}

	// Each replacement record is immutable after publication; the Main-owned
	// catalog selects the latest generation-safe record for downstream readers.
	using Snapshot = std::unordered_map<ObjectId, std::shared_ptr<const RenderItem>>;

	void SynchronizeSnapshot(Snapshot &Items, const std::vector<ObjectId> &Objects) {
		for (const auto Object : Objects) {
			auto Value = Resolve(Object);
			if (Value) Items.insert_or_assign(Object,
				std::make_shared<const RenderItem>(Project(Object, ReadSource(*Value))));
			else Items.erase(Object);
		}
	}

	std::vector<std::optional<RenderItem>> ConsumeSnapshot(
		const Snapshot &Items, const std::vector<ObjectId> &Objects
	) {
		std::vector<std::optional<RenderItem>> Output;
		Output.reserve(Objects.size());
		for (const auto Object : Objects) {
			const auto Found = Items.find(Object);
			Output.emplace_back(Found == Items.end() ? std::nullopt : std::optional(*Found->second));
		}
		return Output;
	}

	void Verify(const std::vector<std::optional<RenderItem>> &Expected,
		const std::vector<std::optional<RenderItem>> &Actual) {
		if (Expected.size() != Actual.size())
			throw std::runtime_error("[Runtime6:ExtractionProbe] Consumer count mismatch");
		for (std::size_t Index = 0; Index < Expected.size(); ++Index)
			if (Expected[Index].has_value() != Actual[Index].has_value() ||
				(Expected[Index] && !Equal(*Expected[Index], *Actual[Index])))
				throw std::runtime_error("[Runtime6:ExtractionProbe] Consumer final-state mismatch");
	}

	std::size_t Parse(const char *Value) {
		const auto Result = std::stoull(Value);
		if (Result > 50000) throw std::invalid_argument("Probe count exceeds its 50K bound");
		return static_cast<std::size_t>(Result);
	}
}

int main(int ArgCount, char **Args) {
	try {
		if (ArgCount < 4 || ArgCount > 6)
			throw std::invalid_argument(
				"Usage: probe count changed-per-frame frames [random|sequential] [lifecycle-count]");
		const auto Count = Parse(Args[1]);
		const auto Changed = Parse(Args[2]);
		const auto Frames = Parse(Args[3]);
		const auto Order = ArgCount >= 5 ? std::string(Args[4]) : std::string("sequential");
		if (Order != "random" && Order != "sequential")
			throw std::invalid_argument("Probe order must be random or sequential");
		const bool RandomOrder = Order == "random";
		const auto Lifecycle = ArgCount >= 6 ? Parse(Args[5]) : 0;
		if (Count == 0 || Frames == 0 || Changed > Count || Lifecycle > Count ||
			Lifecycle > 1024 || Frames > 100)
			throw std::invalid_argument("Probe arguments exceed bounded workload limits");
		gargantuan::BootstrapNativeRuntimeSchema();
		auto &Dirty = gargantuan::RenderDirtyAccumulator::Get();
		gargantuan::ChangeJournal::Get().Clear();
		Dirty.Clear();
		auto Game = std::make_shared<gargantuan::DataModel>();
		auto World = std::dynamic_pointer_cast<gargantuan::Workspace>(Game->GetService("Workspace"));
		std::optional<gargantuan::PhysicsWorld> RetainedPhysics(std::move(World->Physics));
		if (World->Physics.IsValid() || !RetainedPhysics->IsValid())
			throw std::runtime_error("[Runtime6:ExtractionProbe] Renderer-only backend detach failed");
		std::vector<std::shared_ptr<gargantuan::Part>> Parts;
		Parts.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Value = std::make_shared<gargantuan::Part>();
			Value->SetCFrame(gargantuan::CFrame(glm::vec3(static_cast<float>(Index), 0.0f, 0.0f)));
			Value->SetParent(World);
			Parts.push_back(std::move(Value));
		}
		SoASlots Slots;
		Snapshot Items;
		Items.reserve(Count);
		for (const auto &Value : Parts) {
			const auto Object = Value->GetObjectId();
			const auto Source = ReadSource(*Value);
			Slots.Set(Object, Source);
			Items.emplace(Object, std::make_shared<const gargantuan::RenderItem>(Project(Object, Source)));
		}
		Dirty.Clear();
		const auto Consumer = Dirty.CreateConsumer();
		std::vector<std::size_t> Selection(Count);
		std::iota(Selection.begin(), Selection.end(), 0);
		std::mt19937_64 Random(0x8d145c9fU);
		std::size_t ReusedSlots = 0;
		std::cout << "frame,count,changed,lifecycle,order,dirty,setter_ms,capture_ms,direct_ms,"
			"soa_sync_ms,soa_consume_ms,snapshot_sync_ms,snapshot_consume_ms,verify_ms\n";
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			if (RandomOrder) std::shuffle(Selection.begin(), Selection.end(), Random);
			std::unordered_set<gargantuan::ObjectId> ExpectedTouched;
			ExpectedTouched.reserve(Changed + 2 * Lifecycle);
			for (std::size_t Index = 0; Index < Changed; ++Index)
				ExpectedTouched.insert(Parts[Selection[Index]]->GetObjectId());
			for (std::size_t Index = 0; Index < Lifecycle; ++Index)
				ExpectedTouched.insert(Parts[Selection[Count - Index - 1]]->GetObjectId());
			const auto SetterStart = Clock::now();
			for (std::size_t Index = 0; Index < Changed; ++Index) {
				auto &Value = Parts[Selection[Index]];
				auto FrameValue = Value->GetCFrame();
				FrameValue.Position.z = static_cast<float>(Frame + 1) * 0.25f;
				Value->SetCFrame(FrameValue);
			}
			std::vector<gargantuan::ObjectId> Retired;
			for (std::size_t Index = 0; Index < Lifecycle; ++Index) {
				auto &Value = Parts[Selection[Count - Index - 1]];
				Retired.push_back(Value->GetObjectId());
				Value->Destroy();
				auto Replacement = std::make_shared<gargantuan::Part>();
				Replacement->SetParent(World);
				const auto NewObject = Replacement->GetObjectId();
				if (NewObject == Retired.back())
					throw std::runtime_error("[Runtime6:ExtractionProbe] Replacement reused a stale identity");
				if (NewObject.Slot == Retired.back().Slot) {
					if (NewObject.Generation <= Retired.back().Generation)
						throw std::runtime_error("[Runtime6:ExtractionProbe] Slot generation did not advance");
					++ReusedSlots;
				}
				Value = std::move(Replacement);
				if (gargantuan::ObjectRegistry::Get().Lookup(Retired.back()))
					throw std::runtime_error("[Runtime6:ExtractionProbe] Retired generation still resolves");
			}
			const auto SetterMs = Milliseconds(Clock::now() - SetterStart);
			for (std::size_t Index = 0; Index < Lifecycle; ++Index)
				ExpectedTouched.insert(Parts[Selection[Count - Index - 1]]->GetObjectId());
			const auto CaptureStart = Clock::now();
			const auto Batch = Dirty.Capture(World->GetReplicationScopeId(), Consumer);
			const auto CaptureMs = Milliseconds(Clock::now() - CaptureStart);
			if (Batch.FullResyncRequired)
				throw std::runtime_error("[Runtime6:ExtractionProbe] Unexpected dirty full-resync");
			std::unordered_set<gargantuan::ObjectId> ActualTouched;
			ActualTouched.reserve(Batch.Records.size());
			for (const auto &Record : Batch.Records) ActualTouched.insert(Record.Object);
			if (ActualTouched != ExpectedTouched || ActualTouched.size() != Batch.Records.size())
				throw std::runtime_error("[Runtime6:ExtractionProbe] Dirty identities differ from committed mutations");
			std::vector<gargantuan::ObjectId> Objects;
			Objects.reserve(Batch.Records.size());
			for (const auto &Record : Batch.Records) Objects.push_back(Record.Object);
			if (RandomOrder) std::shuffle(Objects.begin(), Objects.end(), Random);
			std::vector<std::optional<gargantuan::RenderItem>> Expected;
			std::vector<std::optional<gargantuan::RenderItem>> SoAOutput;
			std::vector<std::optional<gargantuan::RenderItem>> SnapshotOutput;
			double DirectMs = 0.0, SoASyncMs = 0.0, SoAConsumeMs = 0.0;
			double SnapshotSyncMs = 0.0, SnapshotConsumeMs = 0.0;
			const auto RunDirect = [&] {
				const auto Start = Clock::now();
				Expected = Direct(Objects);
				DirectMs = Milliseconds(Clock::now() - Start);
			};
			const auto RunSoA = [&] {
				auto Start = Clock::now();
				SynchronizeSoA(Slots, Objects);
				SoASyncMs = Milliseconds(Clock::now() - Start);
				Start = Clock::now();
				SoAOutput = ConsumeSoA(Slots, Objects);
				SoAConsumeMs = Milliseconds(Clock::now() - Start);
			};
			const auto RunSnapshot = [&] {
				auto Start = Clock::now();
				SynchronizeSnapshot(Items, Objects);
				SnapshotSyncMs = Milliseconds(Clock::now() - Start);
				Start = Clock::now();
				SnapshotOutput = ConsumeSnapshot(Items, Objects);
				SnapshotConsumeMs = Milliseconds(Clock::now() - Start);
			};
			switch (Frame % 3) {
				case 0: RunDirect(); RunSoA(); RunSnapshot(); break;
				case 1: RunSoA(); RunSnapshot(); RunDirect(); break;
				default: RunSnapshot(); RunDirect(); RunSoA(); break;
			}
			const auto VerifyStart = Clock::now();
			Verify(Expected, SoAOutput);
			Verify(Expected, SnapshotOutput);
			for (const auto Old : Retired)
				if (Slots.Get(Old) || Items.contains(Old))
					throw std::runtime_error("[Runtime6:ExtractionProbe] Retired generation survived in derived state");
			const auto VerifyMs = Milliseconds(Clock::now() - VerifyStart);
			Dirty.Acknowledge(Batch);
			std::cout << Frame << ',' << Count << ',' << Changed << ',' << Lifecycle << ',' << Order << ','
				Objects.size()
				<< ',' << std::fixed << std::setprecision(6) << SetterMs << ',' << CaptureMs << ','
				<< DirectMs << ',' << SoASyncMs << ',' << SoAConsumeMs << ','
				<< SnapshotSyncMs << ',' << SnapshotConsumeMs << ',' << VerifyMs << '\n' << std::flush;
		}
		std::vector<gargantuan::ObjectId> CurrentIds;
		CurrentIds.reserve(Count);
		for (const auto &Value : Parts) CurrentIds.push_back(Value->GetObjectId());
		const auto FullDirectStart = Clock::now();
		const auto FullExpected = Direct(CurrentIds);
		const auto FullDirectMs = Milliseconds(Clock::now() - FullDirectStart);
		const auto RetainedVerifyStart = Clock::now();
		Verify(FullExpected, ConsumeSoA(Slots, CurrentIds));
		Verify(FullExpected, ConsumeSnapshot(Items, CurrentIds));
		const auto RetainedVerifyMs = Milliseconds(Clock::now() - RetainedVerifyStart);
		SoASlots FullSlots;
		Snapshot FullItems;
		FullItems.reserve(Count);
		const auto FullSyncStart = Clock::now();
		for (const auto &Value : Parts) {
			const auto Object = Value->GetObjectId();
			const auto Source = ReadSource(*Value);
			FullSlots.Set(Object, Source);
			FullItems.emplace(Object, std::make_shared<const gargantuan::RenderItem>(Project(Object, Source)));
		}
		const auto FullSyncMs = Milliseconds(Clock::now() - FullSyncStart);
		const auto FullConsumeStart = Clock::now();
		Verify(FullExpected, ConsumeSoA(FullSlots, CurrentIds));
		Verify(FullExpected, ConsumeSnapshot(FullItems, CurrentIds));
		const auto FullConsumeMs = Milliseconds(Clock::now() - FullConsumeStart);
		std::cerr << "[Runtime6:ExtractionProbe] full_resync_count=" << Count
			<< " reused_slots=" << ReusedSlots
			<< " direct_ms=" << FullDirectMs << " dual_cache_sync_ms=" << FullSyncMs
			<< " retained_full_verify_ms=" << RetainedVerifyMs
			<< " dual_cache_consume_verify_ms=" << FullConsumeMs
			<< " soa_logical_bytes="
			<< FullSlots.Generations.capacity() * sizeof(std::uint32_t) +
				FullSlots.Frames.capacity() * sizeof(gargantuan::CFrame) +
				FullSlots.Sizes.capacity() * sizeof(glm::vec3) +
				FullSlots.Colors.capacity() * sizeof(glm::vec4) +
				FullSlots.Geometries.capacity() * sizeof(gargantuan::RenderGeometry) +
				FullSlots.CastShadows.capacity() * sizeof(std::uint8_t)
			<< " snapshot_value_bytes=" << FullItems.size() * sizeof(gargantuan::RenderItem)
			<< '\n';
		Dirty.ReleaseConsumer(Consumer);
		Game->Destroy();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime6:ExtractionProbe] " << Error.what() << '\n';
		return 1;
	}
}
