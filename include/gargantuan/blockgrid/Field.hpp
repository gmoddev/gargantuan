// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace gargantuan::blockgrid {
	inline constexpr std::uint16_t Air = 0, Stone = 1, Brick = 2;
	inline constexpr unsigned LeafSide = 8, LeafVolume = LeafSide * LeafSide * LeafSide;
	inline constexpr std::int32_t CoordinateLimit = 1'048'576;
	inline constexpr std::size_t MaximumEditVisits = 32'768, MaximumReadCells = 4'096;
	inline constexpr std::size_t MaximumCommands = 1'024, MaximumLeaves = 16'384;
	inline constexpr std::size_t MaximumOccupiedCells = 1'048'576, MaximumEncodedBytes = 16 * 1024 * 1024;

	struct Cell {
		std::int32_t X{}, Y{}, Z{};
		bool operator==(const Cell&) const = default;
	};
	struct CellOrder {
		bool operator()(Cell Left, Cell Right) const noexcept;
	};
	struct Region { Cell Min, MaxExclusive; };
	struct Block {
		std::uint16_t Definition{};
		std::uint8_t Orientation{};
		bool operator==(const Block&) const = default;
	};
	struct Write { Region Bounds; Block Value; };

	[[nodiscard]] bool ValidCell(Cell Position) noexcept;
	[[nodiscard]] bool ValidBlock(Block Value) noexcept;
	// Throws before an invalid or oversized input can allocate storage.
	[[nodiscard]] std::size_t Volume(Region Bounds, std::size_t Limit = MaximumEditVisits);
	[[nodiscard]] Region SingleCell(Cell Position);

	class Leaf {
	  public:
		[[nodiscard]] Block Get(unsigned X, unsigned Y, unsigned Z) const;
		[[nodiscard]] unsigned GetOccupiedCount() const noexcept { return Occupied; }
		[[nodiscard]] std::optional<Block> GetUniform() const noexcept;
		void ForEachOccupied(const std::function<void(unsigned, unsigned, unsigned, Block)>& Visit) const;

	  private:
		friend class Field;
		friend std::vector<std::uint8_t> Encode(const class Field&);
		friend class Field Decode(std::span<const std::uint8_t>);
		struct Entry { std::uint16_t Index; Block Value; bool operator==(const Entry&) const = default; };
		using Sparse = std::vector<Entry>;
		using Dense = std::vector<Block>;
		static constexpr unsigned SparseLimit = 64;
		std::variant<Block, Sparse, Dense> Storage{Block{}};
		unsigned Occupied{};
		[[nodiscard]] std::size_t EncodedSize() const noexcept;
		void Set(unsigned Index, Block Value);
		void Fill(Block Value) noexcept;
		void Compact();
	};

	class Field {
		using Directory = std::map<Cell, std::shared_ptr<const Leaf>, CellOrder>;
	  public:
		class Candidate {
		  public:
			[[nodiscard]] Block Get(Cell Position) const;
			[[nodiscard]] const Leaf* GetLeaf(Cell AnyCell) const;
			[[nodiscard]] std::size_t GetLeafCount() const noexcept { return LeafCount; }
			[[nodiscard]] std::size_t GetOccupiedCount() const noexcept { return OccupiedCount; }
			void ForEachLeaf(const std::function<void(Cell,const Leaf&)>& Visit) const;
			// An absent leaf means its last occupancy was removed. Changed origins are unique.
			void ForEachChangedLeaf(const std::function<void(Cell,const Leaf*)>& Visit) const;
		  private:
			friend class Field;
			Candidate(const Field& Prior, const Directory& Added, std::span<const Cell> Erased,
				std::size_t Pages, std::size_t Cells) : Base(Prior), Replacements(Added), Removed(Erased),
				LeafCount(Pages), OccupiedCount(Cells) {}
			const Field& Base;
			const Directory& Replacements;
			std::span<const Cell> Removed;
			std::size_t LeafCount, OccupiedCount;
		};
		[[nodiscard]] Block Get(Cell Position) const;
		[[nodiscard]] std::uint64_t GetRevision() const noexcept { return Revision; }
		[[nodiscard]] std::size_t GetOccupiedCount() const noexcept { return Occupied; }
		[[nodiscard]] std::size_t GetLeafCount() const noexcept { return Leaves.size(); }
		// Visits immutable values in this Main-owned snapshot. The cell argument is the leaf's grid-local minimum.
		void ForEachLeaf(const std::function<void(Cell, const Leaf&)>& Visit) const;
		void ForEachOccupied(const std::function<void(Cell, Block)>& Visit) const;
		// All commands share a single commit. Unchanged data preserves the revision.
		using Admission = std::function<bool(const Candidate&, std::span<const Write>)>;
		[[nodiscard]] bool Apply(std::span<const Write> Writes, std::optional<std::uint64_t> Expected = {},
			const Admission& BeforeCommit = {});
		[[nodiscard]] bool ApplyCellPatch(std::span<const Write> Cells, const Admission& BeforeCommit = {});

	  private:
		friend std::vector<std::uint8_t> Encode(const Field&);
		friend Field Decode(std::span<const std::uint8_t>);
		[[nodiscard]] bool ApplyChecked(std::span<const Write> Writes, const Admission& BeforeCommit);
		Directory Leaves;
		std::size_t Occupied{};
		std::size_t EncodedBytes{9};
		std::uint64_t Revision{1};
	};

	[[nodiscard]] std::vector<std::uint8_t> Encode(const Field& Value);
	[[nodiscard]] Field Decode(std::span<const std::uint8_t> Bytes);
}
