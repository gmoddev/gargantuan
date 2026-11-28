// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
// Linked only into the separate operation-count diagnostic.
#pragma once

#include "SpatialOrdinaryOperations.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace gargantuan::ordinary_benchmark::operations {
	inline constexpr std::array Names{
		"ScriptWrite","EagerWriteClassName","SynchronousRegistryRead","SharedRegistryOwnerCopy",
		"ModelWeakLock","ModelFallbackParentLock","MutationModelWeakLock","MotionExplicitAssignmentWalk",
		"AssignmentWalkNode","AssignmentWalkParentLock",
		"MotionScopedModelCompare","EffectiveWorldNode","EffectiveWorldParentLock","ImpulsePartVisit",
		"ImpulseProtectionLookup","OrderedReadbackPartVisit","RecountReadbackPartVisit","ObserveCollisionAssets",
		"CollisionAssetServiceLookup","CollisionAssetChangeRead","TerrainWorkPoll","NearestPromptQuery",
		"NearestPromptPlayerWorld","NearestPromptCellLookup","IncrementalGridPublication","EmptyGridMapConstruct",
		"GridSetupNewCalls","GridSetupNewBytes","GridCommitNewCalls","GridCommitNewBytes"};
	static_assert(Names.size() == static_cast<std::size_t>(Operation::Count));

	struct OperationRow {
		std::uint64_t Tick, EngineStepNs;
		Counts Called;
	};

	class Capture {
	public:
		Capture() {
			if (const char *Value = std::getenv("GARGANTUAN_ORDINARY_PROFILE_CSV"); Value && *Value) {
				Path = Value;
				Rows.reserve(4096);
			}
		}
		~Capture() {
			if (Path.empty()) return;
			std::ofstream Stream(Path);
			Stream << "Tick,EngineStepNs,Dropped";
			for (const auto Name : Names) Stream << ',' << Name;
			Stream << '\n';
			for (const auto &Row : Rows) {
				Stream << Row.Tick << ',' << Row.EngineStepNs << ',' << Dropped;
				for (const auto Value : Row.Called) Stream << ',' << Value;
				Stream << '\n';
			}
		}
		void Step(Engine &Runtime, runtime_detail::WorkSample &Sample) {
			if (Path.empty()) {
				const runtime_detail::WorkCapture Timer(&Sample);
				Runtime.Step();
				return;
			}
			Counts Called{};
			BenchmarkAllocationProbe::Counts AllocationCounts;
			const BenchmarkAllocationProbe::Scope Allocations(AllocationCounts);
			{
				struct Lifetime {
					Counts *Previous;
					~Lifetime() { Active = Previous; }
				} Guard{Active};
				Active = &Called;
				const runtime_detail::WorkCapture Timer(&Sample);
				Runtime.Step();
			}
			if (Rows.size() == Rows.capacity()) {
				++Dropped;
				return;
			}
			const auto &Whole = Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::EngineStep)];
			Rows.push_back({Runtime.GetSimulationTick(), Whole.Nanoseconds, Called});
		}
	private:
		std::string Path;
		std::vector<OperationRow> Rows;
		std::uint64_t Dropped = 0;
	};

	inline void Step(Engine &Runtime, runtime_detail::WorkSample &Sample) {
		static thread_local Capture Collector;
		Collector.Step(Runtime, Sample);
	}
}
