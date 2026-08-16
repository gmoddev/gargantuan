#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace gargantuan::network {
	template <typename Domain> class MonotonicSequence {
	  public:
		constexpr MonotonicSequence() = default;
		explicit constexpr MonotonicSequence(std::uint64_t Value) : StoredValue(Value) {}

		[[nodiscard]] constexpr bool IsValid() const { return StoredValue != 0; }
		[[nodiscard]] constexpr std::uint64_t Value() const { return StoredValue; }
		[[nodiscard]] constexpr bool IsNewerThan(MonotonicSequence Other) const {
			return IsValid() && Other.IsValid() && StoredValue > Other.StoredValue;
		}
		[[nodiscard]] constexpr std::optional<MonotonicSequence> TryNext() const {
			if (!IsValid() || StoredValue == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
			return MonotonicSequence(StoredValue + 1);
		}
		auto operator<=>(const MonotonicSequence &) const = default;

	  private:
		std::uint64_t StoredValue = 0;
	};

	struct ReplicationEpochDomain;
	struct ReliableReplicationSequenceDomain;
	struct RealtimeStateSequenceDomain;
	struct RemoteEventSequenceDomain;
	struct RemotePublicationIdDomain;
	struct RemoteRequestIdDomain;
	struct StateChannelIdDomain;

	using ReplicationEpoch = MonotonicSequence<ReplicationEpochDomain>;
	using ReliableReplicationSequence = MonotonicSequence<ReliableReplicationSequenceDomain>;
	using RealtimeStateSequence = MonotonicSequence<RealtimeStateSequenceDomain>;
	using RemoteEventSequence = MonotonicSequence<RemoteEventSequenceDomain>;
	using RemotePublicationId = MonotonicSequence<RemotePublicationIdDomain>;
	using RemoteRequestId = MonotonicSequence<RemoteRequestIdDomain>;
	using StateChannelId = MonotonicSequence<StateChannelIdDomain>;
}
