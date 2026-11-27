// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/network/BlockGridStream.hpp"

#include <functional>
#include <map>

namespace gargantuan {
	class BlockGrid;
}
namespace gargantuan::network {
	// One live transfer per peer, at most eight reusable public baselines.
	class BlockGridPeer {
	  public:
		static constexpr std::size_t MaximumGrids = 8;
		using Resolve = std::function<std::shared_ptr<BlockGrid>(ObjectId)>;
		using Send = std::function<bool(const GridStreamMessage &)>;
		BlockGridPeer(bool Server, std::uint64_t Epoch, ObjectId Domain);
		void Track(ObjectId Grid);
		void Forget(ObjectId Grid);
		void Receive(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick);
		void Pump(const Resolve &Find, const Send &Queue, std::uint64_t Tick);
		[[nodiscard]] bool IsInstalled(ObjectId Grid, std::uint64_t PublicRevision) const;

	  private:
		struct Entry {
			std::uint64_t Subscription{}, Revision{}, LastSerial{};
			bool Requested{};
			GridReplicaContent Known;
		};
		struct Transfer {
			GridStreamMessage Offer;
			GridReplicaContent Content;
			std::size_t Cursor{};
			std::uint64_t ProgressTick{};
			bool Offered{}, Installed{};
		};
		bool Server;
		std::uint64_t Epoch{}, NextToken{1};
		ObjectId Domain, LastServed;
		std::map<ObjectId, Entry> Grids;
		std::optional<Transfer> Active;
		GridStreamReceiver Receiver;
		[[nodiscard]] std::uint64_t Token();
		void ReceiveServer(const GridStreamMessage &Message, const Resolve &Find);
		void ReceiveClient(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick);
	};
}
