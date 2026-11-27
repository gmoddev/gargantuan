#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/network/Connection.hpp"
#include "gargantuan/runtime/SpatialDestination.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace gargantuan {
	class BasePart;
	class WorldRoot;
}

namespace gargantuan::network {
	struct SpatialTransferScope {
		std::uint64_t SessionEpoch{}, WorldEpoch{}, FocusEpoch{};
		std::uint32_t PlayerId{};
		WireObjectId Subject, Source, Destination, Controller;
		bool operator==(const SpatialTransferScope &) const = default;
	};

	struct SpatialWireProvider {
		WireObjectId Owner;
		SpatialCollisionProvider Kind{};
		auto operator<=>(const SpatialWireProvider &) const = default;
	};

	enum class SpatialTransferPhase : std::uint8_t { Offer = 1, Ready, Reserve, Held, Commit, Applied, Cancel };

	struct SpatialTransferFrame {
		SpatialTransferPhase Phase{};
		SpatialTransferScope Scope;
		std::uint64_t Serial{};
		SpatialDestinationBounds Bounds;
		WireCFrame SourcePose{}, DestinationPose{};
		glm::vec3 LinearVelocity{}, AngularVelocity{};
		std::vector<SpatialWireProvider> Owners;
		AssetContentId Collision, SubjectGeometry;
	};

	inline constexpr std::size_t MaximumSpatialTransferProviders = 64;
	inline constexpr std::size_t MaximumSpatialTransferBytes = 1'024;
	[[nodiscard]] bool IsSpatialTransfer(std::span<const std::byte> Bytes);
	[[nodiscard]] std::vector<std::byte> EncodeSpatialTransfer(const SpatialTransferFrame &Frame);
[[nodiscard]] std::optional<SpatialTransferFrame> DecodeSpatialTransfer(std::span<const std::byte> Bytes);
	[[nodiscard]] CFrame GetSpatialTransferPose(const WireCFrame &Pose);

	// Supplied by the authoritative content owner, or the replica's verified installed provider.
	// Empty means positively certified empty for the requested bounds. A missing provider cannot certify it.
	struct SpatialAppliedBinding {
		SpatialWireProvider Owner;
		AssetContentId Content;
		std::uint64_t LogicalRevision{}, CoverageRevision{};
		bool Applied{}, Empty{};
	};

	// The owner retains its reserved native/content resources until the last lease reference is released.
	// The predicate also observes authorization, provider generation and coverage invalidation.
	using SpatialTransferLease = SpatialMovementLease;
	struct SpatialTransferAdapters {
		std::function<
			std::optional<SpatialAppliedBinding>(const SpatialProviderReceipt &, const SpatialDestinationBounds &)>
			Project;
		std::function<std::optional<ObjectId>(SpatialWireProvider)> Resolve;
		std::function<std::optional<ObjectId>(WireObjectId)> ResolveObject;
		std::function<SpatialTransferLease(const SpatialTransferFrame &, const SpatialDestinationReceipt &)> Reserve;
	};

	// An early reliable offer can precede its structural/leaf installation. This read-only check
	// never consumes its serial, grants a lease, or moves the subject.
	[[nodiscard]] bool IsSpatialOfferInstalled(const SpatialTransferFrame &Offer, WorldRoot &Destination,
		const SpatialTransferAdapters &Adapters);

	// All calls run on Main. Mutation owners check the old and new bounds before changing collision,
	// and check the whole provider on retirement or content replacement.
	class SpatialTransferReservations final {
	  public:
		SpatialTransferReservations();
		~SpatialTransferReservations();
		[[nodiscard]] SpatialTransferLease Reserve(
			ObjectId Subject,
			const SpatialTransferFrame &Frame,
			const SpatialDestinationReceipt &Receipt,
			std::function<bool()> Authorized,
			std::vector<std::shared_ptr<const void>> Resources = {}
		);
		[[nodiscard]] bool HasHolds() const;
		[[nodiscard]] bool HoldsMember(ObjectId Member) const;
		[[nodiscard]] bool HoldsDomain(ObjectId Domain) const;
		[[nodiscard]] bool HoldsDestination(ObjectId Domain) const;
		[[nodiscard]] bool Admits(
			const SpatialTransferLease &Lease,
			const SpatialMovementRequest &Movement,
			const SpatialDestinationReceipt &Receipt
		) const;
		[[nodiscard]] bool HoldsProvider(ObjectId Owner) const;
		[[nodiscard]] bool HoldsBounds(ObjectId Domain, const SpatialDestinationBounds &Bounds) const;
		void Reset();

	  private:
		friend class SpatialTransferServer;
		friend class SpatialTransferReplica;
		bool ApplyBilateral(
			const SpatialTransferLease &Lease, const SpatialTransferFrame &Frame, const std::function<bool()> &Apply
		);
		struct Table;
		std::unique_ptr<Table> Records;
	};

	// CommittedUnconfirmed means the authoritative native mutation already happened.
	// A disconnected or failed replica must resynchronize before regaining movement authority.
	enum class SpatialTransferStatus {
		Progress,
		Applied,
		CommittedUnconfirmed,
		Canceled,
		Stale,
		Denied,
		Busy,
		Expired
	};
	struct SpatialTransferStep {
		SpatialTransferStatus Status{SpatialTransferStatus::Stale};
		std::optional<SpatialTransferFrame> Reply;
	};

	using SpatialTransferClock = std::chrono::steady_clock;
	class SpatialTransferServer final {
	  public:
		SpatialTransferServer();
		~SpatialTransferServer();
		// Connection and Scope come from the authenticated, controlling GameSession.
		[[nodiscard]] SpatialTransferStep Begin(
			ConnectionId Connection,
			const SpatialTransferScope &Scope,
			const std::shared_ptr<BasePart> &Subject,
			const std::shared_ptr<WorldRoot> &Destination,
			const SpatialMovementRequest &Movement,
			const SpatialDestinationBounds &Bounds,
			std::span<const ObjectId> RequiredOwners,
			const SpatialTransferAdapters &Adapters,
			SpatialTransferClock::time_point Now, std::uint64_t RequestedSerial = 0
		);
		[[nodiscard]] SpatialTransferStep Receive(
			ConnectionId Connection,
			const SpatialTransferScope &Current,
			const SpatialTransferFrame &Frame,
			const SpatialTransferAdapters &Adapters,
			SpatialTransferClock::time_point Now
		);
		[[nodiscard]] SpatialTransferStep Poll(
			ConnectionId Connection,
			const SpatialTransferScope &Current,
			const SpatialTransferAdapters &Adapters,
			SpatialTransferClock::time_point Now
		);
		[[nodiscard]] SpatialTransferStep Cancel();
		void Reset();

	  private:
		struct State;
		std::shared_ptr<State> Current;
		std::uint64_t Mutation{};
		std::uint64_t LastSerial{};
	};

	class SpatialTransferReplica final {
	  public:
		SpatialTransferReplica();
		~SpatialTransferReplica();
		// Apply is invoked only after the server commits, with the still-held local native receipt.
		using ApplyMovement = std::function<
			bool(const SpatialTransferFrame &, const SpatialDestinationReceipt &, const SpatialTransferLease &)>;
		[[nodiscard]] SpatialTransferStep Receive(
			const SpatialTransferScope &Current,
			const SpatialTransferFrame &Frame,
			const std::shared_ptr<WorldRoot> &Destination,
			const SpatialTransferAdapters &Adapters,
			const ApplyMovement &Apply
		);
		[[nodiscard]] SpatialTransferStep
		Poll(const SpatialTransferScope &Current, const SpatialTransferAdapters &Adapters);
		[[nodiscard]] SpatialTransferStep Cancel();
		void Reset();

	  private:
		struct State;
		std::shared_ptr<State> Active;
		std::uint64_t Mutation{};
		std::uint64_t LastEpoch{}, LastSerial{};
	};
}
