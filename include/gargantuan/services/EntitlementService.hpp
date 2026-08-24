#pragma once

#include "gargantuan/entitlements/EntitlementProvider.hpp"
#include "gargantuan/services/generated/EntitlementService.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace gargantuan {
	class Player;

	class EntitlementService : public Instance {
		I_EntitlementService;

	  public:
		static constexpr std::size_t MaximumBatchSize = 32;
		static constexpr std::chrono::milliseconds DefaultDeadline{5'000};

		EntitlementService();
		void ConfigureProvider(std::shared_ptr<IEntitlementProvider> Provider);
		[[nodiscard]] std::uint64_t GetProviderGeneration() const;
		[[nodiscard]] std::string GetProviderName() const;
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

	  private:
		mutable std::mutex ProviderMutex;
		std::shared_ptr<IEntitlementProvider> Provider;
		std::uint64_t ProviderGeneration = 1;
	};
}
