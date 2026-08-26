#pragma once

#include "gargantuan/entitlements/EntitlementProvider.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace gargantuan::testing {
	struct EntitlementProviderConformanceFixture final {
		PlayerIdentity Identity;
		EntitlementId Granted;
		EntitlementId Denied;
		EntitlementId Unavailable;
		EntitlementId Expiring;
	};

	using EntitlementConformanceFailure = std::function<void(std::string)>;

	inline void RunEntitlementProviderConformance(
		IEntitlementProvider &Provider,
		const EntitlementProviderConformanceFixture &Fixture,
		const EntitlementConformanceFailure &Fail
	) {
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		const EntitlementRequestContext Context{{}, Deadline, {}};
		const auto StartResult = Provider.Start(Context);
		if (!StartResult || Provider.GetHealth() != EntitlementProviderHealth::Ready) Fail("lifecycle reaches Ready");
		auto ExpectStatus = [&](const EntitlementId &Id, EntitlementStatus Status, std::string Name) {
			auto Result = Provider.Check(Context, Fixture.Identity, Id);
			if (!Result || Result->Status != Status || Result->Entitlement != Id ||
				Result->Identity != Fixture.Identity)
				Fail(std::move(Name));
		};
		ExpectStatus(Fixture.Granted, EntitlementStatus::Granted, "Granted decision");
		ExpectStatus(Fixture.Denied, EntitlementStatus::Denied, "Denied decision");
		ExpectStatus(Fixture.Unavailable, EntitlementStatus::Unavailable, "Unavailable decision");

		auto Expiring = Provider.Check(Context, Fixture.Identity, Fixture.Expiring);
		if (!Expiring || Expiring->Status != EntitlementStatus::Granted || !Expiring->ExpiresAt)
			Fail("expiring Granted decision");

		const std::vector<EntitlementId> Batch{Fixture.Granted, Fixture.Denied};
		auto BatchResult = Provider.CheckMany(Context, Fixture.Identity, Batch);
		if (!BatchResult || BatchResult->size() != 2 || (*BatchResult)[0].Status != EntitlementStatus::Granted ||
			(*BatchResult)[0].Entitlement != Fixture.Granted || (*BatchResult)[0].Identity != Fixture.Identity ||
			(*BatchResult)[1].Status != EntitlementStatus::Denied || (*BatchResult)[1].Entitlement != Fixture.Denied ||
			(*BatchResult)[1].Identity != Fixture.Identity)
			Fail("bounded ordered batch");

		EntitlementCancellationToken Cancellation;
		Cancellation.Cancel();
		auto Cancelled = Provider.Check({Cancellation, Deadline, {}}, Fixture.Identity, Fixture.Granted);
		if (Cancelled && Cancelled->Status != EntitlementStatus::Unavailable) Fail("cancellation fails closed");

		auto Expired = Provider.Check(
			{{}, std::chrono::steady_clock::now() - std::chrono::milliseconds(1), {}}, Fixture.Identity, Fixture.Granted
		);
		if (Expired && Expired->Status != EntitlementStatus::Unavailable) Fail("deadline fails closed");
		Provider.Stop(Context);
	}
}
