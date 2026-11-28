// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "BenchmarkAllocationProbe.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace gargantuan::ordinary_benchmark::operations {
	enum class Operation {
		ScriptWrite, EagerWriteClassName, SynchronousRegistryRead, SharedRegistryOwnerCopy,
		ModelWeakLock, ModelFallbackParentLock, MutationModelWeakLock, MotionExplicitAssignmentWalk,
		AssignmentWalkNode, AssignmentWalkParentLock,
		MotionScopedModelCompare, EffectiveWorldNode, EffectiveWorldParentLock, ImpulsePartVisit,
		ImpulseProtectionLookup, OrderedReadbackPartVisit, RecountReadbackPartVisit, ObserveCollisionAssets,
		CollisionAssetServiceLookup, CollisionAssetChangeRead, TerrainWorkPoll, NearestPromptQuery,
		NearestPromptPlayerWorld, NearestPromptCellLookup, IncrementalGridPublication, EmptyGridMapConstruct,
		GridSetupNewCalls, GridSetupNewBytes, GridCommitNewCalls, GridCommitNewBytes, Count
	};
	using Counts = std::array<std::uint64_t,static_cast<std::size_t>(Operation::Count)>;
	inline thread_local Counts *Active = nullptr;

	inline void Record(Operation Which, std::uint64_t Amount = 1) noexcept {
		if (Active) (*Active)[static_cast<std::size_t>(Which)] += Amount;
	}

	inline BenchmarkAllocationProbe::Counts Allocation() noexcept {
		return {BenchmarkAllocationProbe::ReadCalls(), BenchmarkAllocationProbe::ReadBytes()};
	}
	inline void RecordAllocation(BenchmarkAllocationProbe::Counts Before, Operation Calls, Operation Bytes) noexcept {
		const auto After = Allocation();
		Record(Calls, After.Calls - Before.Calls);
		Record(Bytes, After.Bytes - Before.Bytes);
	}
}
