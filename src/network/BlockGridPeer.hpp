// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/AssetAudience.hpp"
#include "gargantuan/network/BlockGridStream.hpp"
#include "gargantuan/network/SpatialTransfer.hpp"

#include <functional>
#include <map>

namespace gargantuan {
	class BlockGrid;
}
namespace gargantuan::network {
	struct GridInterest {
		std::array<glm::vec3,4> Centers{};
		std::size_t Count{};
		std::optional<SpatialDestinationBounds> Destination;
		std::optional<SpatialDestinationBounds> ControlledMotion;
	};
	[[nodiscard]] blockgrid::ReplicaCoverage SelectGridCoverage(const blockgrid::Field& Field,const CFrame& Pose,
		float CellSize,const GridInterest& Interest);
	[[nodiscard]] std::optional<blockgrid::Region> GridLocalBounds(const CFrame& Pose,float CellSize,const SpatialDestinationBounds& Bounds);
	// One live transfer per peer, at most eight reusable public baselines.
	class BlockGridPeer {
	  public:
		static constexpr std::size_t MaximumGrids = 8;
		using Resolve = std::function<std::shared_ptr<BlockGrid>(ObjectId)>;
		using Send = std::function<bool(const GridStreamMessage &)>;
		BlockGridPeer(bool Server, std::uint64_t Epoch, ObjectId Domain,
			std::shared_ptr<const blockgrid::SharedAssetPins> SharedAssets = {}, std::uint64_t GrantGeneration = 0);
		void Track(ObjectId Grid);
		void Forget(ObjectId Grid);
		void Retire();
		void Prune(const Resolve &Find);
		void Receive(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick);
		void Pump(const Resolve &Find, const Send &Queue, std::uint64_t Tick,const GridInterest& Interest={});
		[[nodiscard]] bool IsInstalled(ObjectId Source, const BlockGrid &Grid) const;
		[[nodiscard]] std::optional<SpatialAppliedBinding> GetBinding(ObjectId Source, const BlockGrid &Grid, bool Empty) const;
		[[nodiscard]] std::optional<SpatialAppliedBinding> GetBinding(ObjectId Source,const BlockGrid& Grid,bool Empty,
			const SpatialDestinationBounds& Bounds) const;

	  private:
		struct Entry {
			std::uint64_t Subscription{}, Revision{}, Configuration{}, MotionRevision{}, LastSerial{}, MotionTick{};
			AssetContentId Placement;
			std::weak_ptr<BlockGrid> Replica;
			std::weak_ptr<BlockGrid> Owner;
			bool Requested{};
			std::uint64_t SourceRevision{};
			std::uint64_t LocalMembership{},WireMembership{};
			bool Unavailable{};
			std::shared_ptr<const blockgrid::ReplicaCoverage> DeniedCoverage;
			GridTransferFence Fence;
			GridReplicaContent Known;
			mutable std::optional<blockgrid::Region> ReceiptRectangle;
			mutable std::uint64_t ReceiptFieldRevision{},ReceiptMotionRevision{},ReceiptFenceGeneration{};
			mutable AssetContentId ReceiptDigest;
		};
		struct Transfer {
			GridStreamMessage Offer;
			GridReplicaContent Content;
			std::optional<blockgrid::Field> PendingField;
			std::size_t Cursor{};
			std::uint64_t ProgressTick{};
			std::uint64_t SourceRevision{};
			bool Offered{}, Installed{};
		};
		bool Server;
		std::uint64_t Epoch{}, NextToken{1};
		ObjectId Domain, LastServed;
		std::uint64_t GrantGeneration{};
		std::shared_ptr<const blockgrid::SharedAssetPins> SharedAssets;
		std::map<ObjectId, Entry> Grids;
		std::optional<Transfer> Active;
		GridStreamReceiver Receiver;
		[[nodiscard]] bool FencesInstalled(ObjectId Source,const SpatialDestinationBounds* Bounds=nullptr) const;
		[[nodiscard]] GridTransferFence CurrentFence(ObjectId Id,const BlockGrid& Grid,const Resolve& Find) const;
		[[nodiscard]] std::optional<SpatialAppliedBinding> Binding(ObjectId Source,const BlockGrid& Grid,bool Empty,
			const SpatialDestinationBounds* Bounds) const;
		[[nodiscard]] std::uint64_t Token();
		void ReceiveServer(const GridStreamMessage &Message, const Resolve &Find);
		void ReceiveClient(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick);
	};
}
