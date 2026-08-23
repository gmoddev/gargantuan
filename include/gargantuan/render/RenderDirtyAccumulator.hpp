// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	using RenderDirtyConsumerId = std::uint64_t;
	inline constexpr RenderDirtyConsumerId InvalidRenderDirtyConsumerId = 0;

	struct RenderDirtyLimits {
		std::size_t MaximumScopes = 64;
		std::size_t MaximumConsumers = 64;
		std::size_t MaximumDistinctObjects = 131072;
		std::size_t MaximumDeformableBytes = 32 * 1024 * 1024;
		std::size_t MaximumUiBytes = 32 * 1024 * 1024;
		std::size_t MaximumPublicationBytes = 64 * 1024 * 1024;
		std::size_t MaximumDiagnostics = 8;
	};

	struct RenderDirtyRecord {
		ObjectId Object;
		RenderUpdateDomain Domains = RenderUpdateDomain::None;
		std::size_t EstimatedDeformableBytes = 0;
		std::uint64_t Version = 0;
	};

	struct RenderDirtyBatch {
		ObjectId Scope;
		RenderDirtyConsumerId Consumer = InvalidRenderDirtyConsumerId;
		std::uint64_t CaptureVersion = 0;
		std::uint64_t GlobalCaptureVersion = 0;
		bool FullResyncRequired = false;
		bool UiDirty = false;
		std::size_t EstimatedDeformableBytes = 0;
		std::size_t EstimatedUiBytes = 0;
		std::vector<RenderDirtyRecord> Records;
		std::vector<std::string> Diagnostics;
	};

	struct RenderDirtyProfile {
		std::uint64_t ClassificationCalls = 0;
		std::uint64_t AccumulationCalls = 0;
		std::uint64_t ClassificationNanoseconds = 0;
		std::uint64_t AccumulationNanoseconds = 0;
	};

	class RenderDirtyAccumulator final {
	  public:
		explicit RenderDirtyAccumulator(RenderDirtyLimits LimitsValue = {});
		static RenderDirtyAccumulator &Get();

		void RecordChange(ObjectId Scope, ObjectId Object, const ChangePayload &Payload) noexcept;
		void Mark(
			ObjectId Scope,
			ObjectId Object,
			RenderUpdateDomain Domains,
			std::size_t EstimatedDeformableBytes = 0
		) noexcept;
		void MarkUi(ObjectId Scope, std::size_t EstimatedUiBytes) noexcept;
		void RequestFullResync(ObjectId Scope, std::string Diagnostic) noexcept;

		[[nodiscard]] RenderDirtyConsumerId CreateConsumer();
		void ReleaseConsumer(RenderDirtyConsumerId Consumer, ObjectId Scope = {});
		[[nodiscard]] RenderDirtyBatch Capture(ObjectId Scope, RenderDirtyConsumerId Consumer);
		void Acknowledge(const RenderDirtyBatch &Batch);
		void Clear();

		[[nodiscard]] const RenderDirtyLimits &GetLimits() const { return Limits; }
		[[nodiscard]] std::size_t GetPendingObjectCount(ObjectId Scope) const;
		[[nodiscard]] static RenderUpdateDomain Classify(const ChangePayload &Payload);
		void SetProfilingEnabled(bool Enabled) { ProfilingEnabled.store(Enabled, std::memory_order_relaxed); }
		void ResetProfile();
		[[nodiscard]] RenderDirtyProfile GetProfile() const;

	  private:
		struct Entry {
			RenderUpdateDomain Domains = RenderUpdateDomain::None;
			std::size_t EstimatedDeformableBytes = 0;
			std::uint64_t Version = 0;
		};
		struct ScopeState {
			std::uint64_t NextVersion = 1;
			std::uint64_t FullResyncVersion = 0;
			std::uint64_t UiVersion = 0;
			std::size_t EstimatedDeformableBytes = 0;
			std::size_t EstimatedUiBytes = 0;
			std::unordered_map<ObjectId, Entry> Entries;
			std::unordered_map<RenderDirtyConsumerId, std::uint64_t> ConsumerVersions;
			std::vector<std::string> Diagnostics;
		};

		void MarkFailure(ScopeState &State, std::string Diagnostic) noexcept;
		void MarkGlobalFailureLocked() noexcept;
		void MarkGlobalFailure() noexcept;
		[[nodiscard]] std::uint64_t AdvanceVersion(ScopeState &State) noexcept;

		RenderDirtyLimits Limits;
		mutable std::mutex Mutex;
		std::unordered_map<ObjectId, ScopeState> Scopes;
		std::unordered_map<RenderDirtyConsumerId, std::uint64_t> ConsumerGlobalVersions;
		std::uint64_t GlobalFailureVersion = 0;
		std::uint64_t NextGlobalVersion = 1;
		std::atomic<RenderDirtyConsumerId> NextConsumer = 1;
		std::atomic<bool> ProfilingEnabled = false;
		std::atomic<std::uint64_t> ProfileClassificationCalls = 0;
		std::atomic<std::uint64_t> ProfileAccumulationCalls = 0;
		std::atomic<std::uint64_t> ProfileClassificationNanoseconds = 0;
		std::atomic<std::uint64_t> ProfileAccumulationNanoseconds = 0;
	};
}
