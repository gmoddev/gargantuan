// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/blockgrid/Assembly.hpp"
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <optional>
#include <span>
#include <vector>

namespace gargantuan::network {
	inline constexpr std::size_t MaximumGridReplicaBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumGridStreamFragment = 16 * 1024;
	enum class GridStreamKind : std::uint8_t { Request = 1, Offer, Fragment, Installed, Forget, Unavailable };

	struct GridStreamScope {
		std::uint64_t Epoch{};
		ObjectId Grid{}, Domain{};
		std::uint64_t GrantGeneration{};
		bool operator==(const GridStreamScope &) const = default;
	};
	struct GridReuseRange { std::uint32_t From{}, To{}, Length{}; };
	struct GridTransferFence {
		std::uint64_t Generation{};
		ObjectId Other;
		bool operator==(const GridTransferFence&) const = default;
	};
	struct GridStreamMessage {
		GridStreamKind Kind{};
		GridStreamScope Scope;
		std::uint64_t Subscription{}, Serial{}, Revision{}, Configuration{}, MotionRevision{};
		AssetContentId Base, Target, Placement;
		std::optional<blockgrid::AssemblyProjection> Assembly;
		GridTransferFence Fence;
		std::uint32_t Size{}, Prefix{}, Suffix{}, Offset{};
		std::vector<GridReuseRange> Reuse;
		std::span<const std::uint8_t> Fragment;
	};
	[[nodiscard]] bool IsGridStream(std::span<const std::byte> Bytes);
	[[nodiscard]] std::vector<std::byte> EncodeGridStream(const GridStreamMessage &Value);
	[[nodiscard]] std::optional<GridStreamMessage> DecodeGridStream(std::span<const std::byte> Bytes);
	struct GridReplicaContent {
		AssetContentId Digest;
		std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
		std::shared_ptr<const blockgrid::ReplicaCoverage> Coverage;
		std::shared_ptr<const std::vector<blockgrid::ReplicaPageRange>> Records;
	};
	[[nodiscard]] GridReplicaContent CaptureGridReplica(const blockgrid::Field &Field);
	[[nodiscard]] std::optional<GridReplicaContent> CaptureGridReplica(const blockgrid::Field &Field,
		const blockgrid::ReplicaCoverage &Coverage, const std::function<bool(const blockgrid::Field&)>& AdmitSchema);
	[[nodiscard]] std::pair<std::size_t,std::size_t> NextGridLiteral(const GridStreamMessage& Offer,std::size_t Cursor);
	[[nodiscard]] GridStreamMessage MakeGridOffer(
		GridStreamScope Scope,
		std::uint64_t Subscription,
		std::uint64_t Serial,
		std::uint64_t Revision,
		std::uint64_t Configuration,
		AssetContentId Placement,
		const blockgrid::AssemblyProjection &Assembly,
		const GridReplicaContent &Next,
		const GridReplicaContent *Known = nullptr
	);

	// One receiver per active session transfer. Every fragment belongs to the same authenticated shell and serial.
	class GridStreamReceiver {
	  public:
		void Begin(const GridStreamMessage &Offer, const GridStreamScope &Granted, const GridReplicaContent *Known);
		void Append(const GridStreamMessage &Fragment);
		[[nodiscard]] bool Complete() const;
		struct Result {
			blockgrid::Field Field;
			GridReplicaContent Content;
		};
		[[nodiscard]] Result Finish();
		void Cancel() noexcept;

	  private:
		std::optional<GridStreamMessage> Active;
		std::vector<std::uint8_t> Bytes;
		std::size_t Written{};
	};
}
