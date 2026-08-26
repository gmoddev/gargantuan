#pragma once

#include "gargantuan/entitlements/EntitlementProvider.hpp"
#include "gargantuan/services/generated/EntitlementService.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gargantuan {
	class Player;

	struct EntitlementProviderMetrics final {
		std::uint64_t SemanticChecks = 0;
		std::uint64_t ProviderCalls = 0;
		std::uint64_t Granted = 0;
		std::uint64_t Denied = 0;
		std::uint64_t Unavailable = 0;
		std::uint64_t Timeouts = 0;
		std::uint64_t CacheHits = 0;
		std::uint64_t CacheMisses = 0;
		std::uint64_t ReplacementAttempts = 0;
		std::uint64_t ReplacementCommits = 0;
		std::uint64_t ReplacementFailures = 0;
		std::uint64_t TotalProviderLatencyMicroseconds = 0;
		std::uint64_t MaximumProviderLatencyMicroseconds = 0;
		std::size_t InFlightRequests = 0;
	};

	class EntitlementService : public Instance {
		I_EntitlementService;

	  public:
		static constexpr std::size_t MaximumBatchSize = 32;
		static constexpr std::size_t MaximumInFlightRequests = 256;
		static constexpr std::size_t MaximumCacheEntries = 1'024;
		static constexpr std::chrono::milliseconds DefaultDeadline{5'000};
		static constexpr std::chrono::milliseconds SemanticCacheLifetime{5'000};
		using CheckCompletion = std::function<void(EntitlementDecision)>;
		using CheckManyCompletion = std::function<void(std::vector<EntitlementDecision>)>;
		struct State;

		EntitlementService();
		~EntitlementService() override;
		[[nodiscard]] bool ConfigureProvider(std::shared_ptr<IEntitlementProvider> Provider);
		void ShutdownProviderRuntime();
		[[nodiscard]] std::uint64_t GetProviderGeneration() const;
		[[nodiscard]] std::string GetProviderName() const;
		[[nodiscard]] EntitlementProviderHealth GetProviderHealth() const;
		[[nodiscard]] EntitlementProviderMetrics GetProviderMetrics() const;
		[[nodiscard]] EntitlementDecision Check(
			const std::shared_ptr<Player> &PlayerValue,
			const EntitlementId &Entitlement,
			const EntitlementCancellationToken &Cancellation = {}
		) const;
		[[nodiscard]] std::vector<EntitlementDecision> CheckMany(
			const std::shared_ptr<Player> &PlayerValue,
			std::span<const EntitlementId> Entitlements,
			const EntitlementCancellationToken &Cancellation = {}
		) const;
		[[nodiscard]] bool BeginCheck(
			const std::shared_ptr<Player> &PlayerValue,
			const EntitlementId &Entitlement,
			CheckCompletion Completion,
			const EntitlementCancellationToken &Cancellation = {}
		) const;
		[[nodiscard]] bool BeginCheckMany(
			const std::shared_ptr<Player> &PlayerValue,
			std::span<const EntitlementId> Entitlements,
			CheckManyCompletion Completion,
			const EntitlementCancellationToken &Cancellation = {}
		) const;
		std::size_t PumpAsyncCompletions();
		void DetachAsyncRuntime();

	  private:
		std::unique_ptr<State> Runtime;
	};
}
