#include "gargantuan/entitlements/EntitlementProvider.hpp"

#include <algorithm>
#include <stdexcept>

namespace gargantuan {
	namespace {
		EntitlementProviderError ContextError(const EntitlementRequestContext &Context) {
			return {
				Context.IsCancelled() ? EntitlementProviderErrorCode::Cancelled
									  : EntitlementProviderErrorCode::DeadlineExceeded
			};
		}
	}

	std::optional<EntitlementId> EntitlementId::Parse(std::string_view Value) {
		if (Value.empty() || Value.size() > MaximumBytes) return std::nullopt;
		bool SegmentStart = true;
		bool HasSeparator = false;
		for (const auto Character : Value) {
			if (Character == '.') {
				if (SegmentStart) return std::nullopt;
				SegmentStart = true;
				HasSeparator = true;
				continue;
			}
			if (SegmentStart) {
				if (Character < 'a' || Character > 'z') return std::nullopt;
				SegmentStart = false;
				continue;
			}
			if ((Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') || Character == '_' ||
				Character == '-')
				continue;
			return std::nullopt;
		}
		if (SegmentStart || !HasSeparator) return std::nullopt;
		return EntitlementId(std::string(Value));
	}

	std::string_view GetEntitlementStatusName(EntitlementStatus Status) {
		switch (Status) {
		case EntitlementStatus::Granted:
			return "Granted";
		case EntitlementStatus::Denied:
			return "Denied";
		case EntitlementStatus::Unavailable:
			return "Unavailable";
		}
		return "Unavailable";
	}

	std::string_view GetEntitlementProviderHealthName(EntitlementProviderHealth Health) {
		switch (Health) {
		case EntitlementProviderHealth::Unavailable:
			return "Unavailable";
		case EntitlementProviderHealth::Ready:
			return "Ready";
		case EntitlementProviderHealth::Degraded:
			return "Degraded";
		}
		return "Unavailable";
	}

	EntitlementProviderLifecycleResult IEntitlementProvider::Start(const EntitlementRequestContext &Context) {
		if (Context.IsCancelled())
			return std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::Cancelled});
		if (Context.IsExpired())
			return std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::DeadlineExceeded});
		return {};
	}

	void IEntitlementProvider::Stop(const EntitlementRequestContext &) noexcept {}

	EntitlementProviderHealth IEntitlementProvider::GetHealth() const noexcept {
		return EntitlementProviderHealth::Ready;
	}

	EntitlementProviderBatchResult IEntitlementProvider::CheckMany(
		const EntitlementRequestContext &Context,
		const PlayerIdentity &Identity,
		std::span<const EntitlementId> Entitlements
	) {
		std::vector<EntitlementDecision> Decisions;
		Decisions.reserve(Entitlements.size());
		for (const auto &Entitlement : Entitlements) {
			if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(ContextError(Context));
			auto Result = Check(Context, Identity, Entitlement);
			if (!Result) return std::unexpected(Result.error());
			Decisions.push_back(std::move(*Result));
		}
		return Decisions;
	}

	EntitlementProviderResult NoneEntitlementProvider::Check(
		const EntitlementRequestContext &Context, const PlayerIdentity &Identity, const EntitlementId &Entitlement
	) {
		if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(ContextError(Context));
		return EntitlementDecision{EntitlementStatus::Unavailable, Entitlement, Identity, std::nullopt};
	}

	LocalEntitlementProvider::LocalEntitlementProvider(std::vector<LocalEntitlementGrant> Values)
		: Grants(std::move(Values)) {
		for (const auto &Grant : Grants)
			ValidatePlayerIdentity(Grant.Identity);
		std::ranges::sort(Grants, [](const auto &Left, const auto &Right) {
			if (Left.Identity != Right.Identity) return Left.Identity < Right.Identity;
			return Left.Entitlement < Right.Entitlement;
		});
		if (std::ranges::adjacent_find(Grants, [](const auto &Left, const auto &Right) {
				return Left.Identity == Right.Identity && Left.Entitlement == Right.Entitlement;
			}) != Grants.end())
			throw std::invalid_argument("Configured local entitlement grants contain a duplicate");
	}

	EntitlementProviderResult LocalEntitlementProvider::Check(
		const EntitlementRequestContext &Context, const PlayerIdentity &Identity, const EntitlementId &Entitlement
	) {
		if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(ContextError(Context));
		const auto Match = std::ranges::find_if(Grants, [&](const auto &Grant) {
			return Grant.Identity == Identity && Grant.Entitlement == Entitlement;
		});
		if (Match == Grants.end())
			return EntitlementDecision{EntitlementStatus::Denied, Entitlement, Identity, std::nullopt};
		const auto Expired = Match->ExpiresAt && *Match->ExpiresAt <= std::chrono::system_clock::now();
		return EntitlementDecision{
			Expired ? EntitlementStatus::Denied : EntitlementStatus::Granted,
			Entitlement,
			Identity,
			Match->ExpiresAt,
		};
	}
}
