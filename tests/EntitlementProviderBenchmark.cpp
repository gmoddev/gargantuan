#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/entitlements/EntitlementProvider.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/EntitlementService.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;
	using Samples = std::vector<std::uint64_t>;

	class CustomProvider final : public gargantuan::IEntitlementProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "benchmark-custom";
		}
		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &,
			const gargantuan::PlayerIdentity &Identity,
			const gargantuan::EntitlementId &Entitlement
		) override {
			const auto Status = Entitlement.Value() == "game.base" ? gargantuan::EntitlementStatus::Granted
																   : gargantuan::EntitlementStatus::Unavailable;
			return gargantuan::EntitlementDecision{Status, Entitlement, Identity, std::nullopt};
		}
	};

	template <typename Function> Samples Measure(std::size_t Count, Function &&Callback) {
		Samples Result;
		Result.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto StartedAt = Clock::now();
			Callback();
			Result.push_back(
				static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - StartedAt).count()
				)
			);
		}
		std::sort(Result.begin(), Result.end());
		return Result;
	}

	nlohmann::ordered_json Summarize(const Samples &Values) {
		auto Percentile = [&](double Fraction) {
			const auto Index = static_cast<std::size_t>(Fraction * static_cast<double>(Values.size() - 1));
			return Values[Index];
		};
		return {
			{"samples", Values.size()},
			{"p50_ns", Percentile(0.50)},
			{"p95_ns", Percentile(0.95)},
			{"p99_ns", Percentile(0.99)},
		};
	}
}

int main(int ArgumentCount, char **Arguments) {
	using namespace gargantuan;
	const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
	const std::size_t Count = Quick ? 512 : 5'000;
	BootstrapNativeRuntimeSchema();
	auto World = std::make_shared<DataModel>();
	auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
	auto PlayerValue = std::make_shared<Player>();
	PlayerValue->InitializeAuthenticationIdentity({"benchmark", "player"});
	PlayerValue->SetParent(World);
	const auto GameBase = *EntitlementId::Parse("game.base");
	const auto Missing = *EntitlementId::Parse("feature.provider_unavailable");
	const PlayerIdentity Identity{"benchmark", "player"};
	const EntitlementRequestContext Context{{}, Clock::now() + std::chrono::hours(1), {}};

	auto Local = std::make_shared<LocalEntitlementProvider>(
		std::vector<LocalEntitlementGrant>{{Identity, GameBase, std::nullopt}}
	);
	auto Custom = std::make_shared<CustomProvider>();
	auto LocalDirect = Measure(Count, [&] { (void)Local->Check(Context, Identity, GameBase); });
	auto CustomDirect = Measure(Count, [&] { (void)Custom->Check(Context, Identity, GameBase); });
	(void)Service->ConfigureProvider(Custom);
	auto DispatchMiss = Measure(Count, [&] { (void)Service->Check(PlayerValue, Missing); });
	(void)Service->Check(PlayerValue, GameBase);
	auto CacheHit = Measure(Count, [&] { (void)Service->Check(PlayerValue, GameBase); });
	std::uint64_t SwapIndex = 0;
	auto Swap = Measure(Count, [&] {
		if ((SwapIndex++ & 1U) == 0)
			(void)Service->ConfigureProvider(Local);
		else
			(void)Service->ConfigureProvider(Custom);
	});

	nlohmann::ordered_json Report{
		{"benchmark", "entitlement-provider-foundation-1"},
		{"quick", Quick},
		{"local_provider", Summarize(LocalDirect)},
		{"custom_provider", Summarize(CustomDirect)},
		{"service_cache_miss", Summarize(DispatchMiss)},
		{"service_cache_hit", Summarize(CacheHit)},
		{"provider_swap", Summarize(Swap)},
	};
	std::cout << Report.dump(2) << '\n';
	return 0;
}
