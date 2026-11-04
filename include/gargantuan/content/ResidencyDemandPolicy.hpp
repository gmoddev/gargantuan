#pragma once

#include "gargantuan/content/ContentAvailability.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace gargantuan {
	using ResidencyFocusId = std::uint64_t;

	// Internal Engine policy, never a serialized or client-supplied priority.
	enum class ResidencyDemandClass : std::uint8_t {
		Critical,
		Gameplay,
		Predictive,
		Presentation,
		Speculative,
	};

	struct ResidencyDemandPolicyLimits final {
		std::size_t MaximumFocuses = 512;
		std::size_t MaximumCandidatesPerFocus = 256;
		std::size_t MaximumFocusEvaluationsPerTick = 16;
		std::size_t MaximumQueryCellsPerTick = 512;
		std::size_t MaximumMembershipVisitsPerTick = 4'096;
		std::size_t MaximumDemandDeltasPerTick = 32;
		std::size_t MaximumCandidateRevalidationsPerTick = 8'192;
		std::size_t MaximumRequestValidationVisitsPerTick = 4'096;
		float EnterRadius = 256.0f;
		float LeaveRadius = 320.0f;
		float LookaheadSeconds = 1.0f;
		float MaximumLookaheadDistance = 128.0f;
		std::uint64_t GraceTicks = 30;
	};

	struct ResidencyDemandSnapshot final {
		std::string Key;
		ResidencyDemandClass Class = ResidencyDemandClass::Presentation;
		std::optional<std::uint64_t> DeadlineTick;
		float DistanceSquared = 0.0f;
		std::uint64_t CostBytes = 0;
		std::uint64_t Revision = 0;
		bool Desired = false;
		bool Requested = false;
	};

	struct ResidencyDemandPolicyMetrics final {
		std::uint64_t QueryPages = 0;
		std::uint64_t QueryCellsVisited = 0;
		std::uint64_t QueryMembershipVisits = 0;
		std::uint64_t QueryFailures = 0;
		std::uint64_t ServiceBindingFailures = 0;
		std::uint64_t StaleRevisions = 0;
		std::uint64_t CompletedFocusEvaluations = 0;
		std::uint64_t MovingSweepCompletions = 0;
		std::uint64_t CandidateRevalidations = 0;
		std::uint64_t RankingOperations = 0;
		std::uint64_t RequestValidationVisits = 0;
		std::uint64_t Requests = 0;
		std::uint64_t Releases = 0;
		std::uint64_t RequestDeferrals = 0;
		std::size_t Focuses = 0;
		std::size_t PendingFocuses = 0;
		std::size_t UnhealthyFocuses = 0;
		std::size_t RetainedCandidates = 0;
		std::size_t DemandRecords = 0;
		std::size_t PendingDeltas = 0;
	};

	// One policy per Engine/manifest. Construction may consume immutable metadata
	// on a worker; mutable methods run on Main. The caller owns generation-safe
	// focus IDs and must remove them when their native owner ends.
	class ResidencyDemandPolicy final {
	  public:
		explicit ResidencyDemandPolicy(
			const PackageContentManifest &Manifest,
			ResidencyDemandPolicyLimits Limits = {}
		);
		~ResidencyDemandPolicy();

		[[nodiscard]] bool UpdateFocus(
			ResidencyFocusId Id,
			glm::vec3 Position,
			glm::vec3 AcceptedVelocity = {},
			ResidencyDemandClass Class = ResidencyDemandClass::Gameplay
		);
		[[nodiscard]] bool RemoveFocus(ResidencyFocusId Id);
		[[nodiscard]] bool SetRequirement(
			std::string_view Key,
			ResidencyDemandClass Class,
			std::optional<std::uint64_t> DeadlineTick = std::nullopt
		);
		[[nodiscard]] bool ClearRequirement(std::string_view Key);

		// Null availability evaluates policy without changing 3L; useful for
		// deterministic cost qualification. A live service must have this manifest.
		void Step(ContentAvailabilityService *Availability, std::uint64_t Tick);
		void Stop(ContentAvailabilityService *Availability);
		[[nodiscard]] ResidencyDemandPolicyMetrics GetMetrics() const;
		[[nodiscard]] std::vector<ResidencyDemandSnapshot> GetDemands() const;

		ResidencyDemandPolicy(const ResidencyDemandPolicy &) = delete;
		ResidencyDemandPolicy &operator=(const ResidencyDemandPolicy &) = delete;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
