// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include <deque>

namespace gargantuan::blockgrid {
	struct ValueTest { std::uint8_t Channel{}; std::uint32_t Bits{}; };
	struct Filter { std::optional<std::vector<std::uint16_t>> Definitions; std::optional<ValueTest> Where; };
	enum class Adjacency : std::uint8_t { Face, Edge, Corner };
	struct TraversalOptions {
		Region Bounds;
		Filter Select;
		Adjacency Neighbors{Adjacency::Face};
		bool UsePorts{};
		std::vector<std::uint16_t> StopAt;
		std::size_t MaxVisited{16'384};
	};
	enum class PageStatus : std::uint8_t { More, Complete, Found, Limit, Unavailable, Expired, Cancelled };
	struct Page { PageStatus Status; std::vector<Cell> Cells; std::optional<Cell> Match; };
	void ValidateFilter(const Field& Grid,Filter& Value);
	[[nodiscard]] bool Matches(const Field& Grid,const Filter& Select,Cell P,Block B);
	class Traversal {
	  public:
		Traversal(std::shared_ptr<const Field> Snapshot,std::span<const Cell> Starts,TraversalOptions Parameters);
		Traversal(std::shared_ptr<const Field> Snapshot,std::uint8_t IndexedChannel,Region Bounds);
		[[nodiscard]] Page Step(std::size_t Budget);
		[[nodiscard]] PageStatus GetStatus() const noexcept { return Status; }
		void Finish(PageStatus Reason);
	  private:
		bool Eligible(Cell P,Block B) const;
		void Admit(Cell P);
		struct Offset { Cell Delta; int Face; };
		std::shared_ptr<const Field> Pin;
		TraversalOptions Options;
		std::set<Cell,CellOrder> Seen;
		std::deque<Cell> Queue;
		std::array<Offset,26> Offsets;
		unsigned OffsetCount{},NextNeighbor{};
		std::optional<Cell> Expanding;
		std::uint8_t Ports{};
		PageStatus Status{PageStatus::More};
		std::optional<std::uint8_t> Active;
		ChannelData::Directory::const_iterator CurrentPage;
		std::size_t Entry{},Emitted{};
	};
}
