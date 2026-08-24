#pragma once

#include "gargantuan/identity/PlayerIdentity.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	class EntitlementId final {
	  public:
		static constexpr std::size_t MaximumBytes = 128;

		[[nodiscard]] static std::optional<EntitlementId> Parse(std::string_view Value);
		[[nodiscard]] const std::string &Value() const {
			return Id;
		}
		auto operator<=>(const EntitlementId &) const = default;

	  private:
		explicit EntitlementId(std::string Value) : Id(std::move(Value)) {}
		std::string Id;
	};

	enum class EntitlementStatus : std::uint8_t { Granted, Denied, Unavailable };

	[[nodiscard]] std::string_view GetEntitlementStatusName(EntitlementStatus Status);

	struct EntitlementDecision {
		EntitlementStatus Status = EntitlementStatus::Unavailable;
		EntitlementId Entitlement;
		PlayerIdentity Identity;
		std::optional<std::chrono::system_clock::time_point> ExpiresAt;
	};

	struct EntitlementCancellationToken {
		std::shared_ptr<std::atomic_bool> Cancelled = std::make_shared<std::atomic_bool>(false);
		void Cancel() const {
			if (Cancelled) Cancelled->store(true, std::memory_order_release);
		}
		[[nodiscard]] bool IsCancelled() const {
			return Cancelled && Cancelled->load(std::memory_order_acquire);
		}
	};

	struct EntitlementRequestContext {
		EntitlementCancellationToken Cancellation;
		std::chrono::steady_clock::time_point Deadline;

		[[nodiscard]] bool IsCancelled() const {
			return Cancellation.IsCancelled();
		}
		[[nodiscard]] bool IsExpired() const {
			return std::chrono::steady_clock::now() >= Deadline;
		}
	};

	enum class EntitlementProviderErrorCode : std::uint8_t {
		Cancelled,
		DeadlineExceeded,
		Unavailable,
		Internal,
	};

	struct EntitlementProviderError {
		EntitlementProviderErrorCode Code = EntitlementProviderErrorCode::Internal;
	};

	using EntitlementProviderResult = std::expected<EntitlementDecision, EntitlementProviderError>;
	using EntitlementProviderBatchResult = std::expected<std::vector<EntitlementDecision>, EntitlementProviderError>;

	class IEntitlementProvider {
	  public:
		virtual ~IEntitlementProvider() = default;
		[[nodiscard]] virtual std::string_view Name() const = 0;
		[[nodiscard]] virtual EntitlementProviderResult Check(
			const EntitlementRequestContext &Context, const PlayerIdentity &Identity, const EntitlementId &Entitlement
		) = 0;
		[[nodiscard]] virtual EntitlementProviderBatchResult CheckMany(
			const EntitlementRequestContext &Context,
			const PlayerIdentity &Identity,
			std::span<const EntitlementId> Entitlements
		);
	};

	class NoneEntitlementProvider final : public IEntitlementProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "none";
		}
		[[nodiscard]] EntitlementProviderResult Check(
			const EntitlementRequestContext &Context, const PlayerIdentity &Identity, const EntitlementId &Entitlement
		) override;
	};

	struct LocalEntitlementGrant {
		PlayerIdentity Identity;
		EntitlementId Entitlement;
		std::optional<std::chrono::system_clock::time_point> ExpiresAt;
	};

	class LocalEntitlementProvider final : public IEntitlementProvider {
	  public:
		explicit LocalEntitlementProvider(std::vector<LocalEntitlementGrant> Grants);
		[[nodiscard]] std::string_view Name() const override {
			return "configured-local";
		}
		[[nodiscard]] EntitlementProviderResult Check(
			const EntitlementRequestContext &Context, const PlayerIdentity &Identity, const EntitlementId &Entitlement
		) override;

	  private:
		std::vector<LocalEntitlementGrant> Grants;
	};
}
