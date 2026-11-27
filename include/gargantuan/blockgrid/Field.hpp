// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Geometry.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace gargantuan::blockgrid {
	inline constexpr std::uint16_t Air = 0, Stone = 1, Brick = 2;
	inline constexpr unsigned LeafSide = 8, LeafVolume = LeafSide * LeafSide * LeafSide;
	inline constexpr std::int32_t CoordinateLimit = 1'048'576;
	inline constexpr std::size_t MaximumEditVisits = 32'768, MaximumReadCells = 4'096;
	inline constexpr std::size_t MaximumCommands = 1'024, MaximumLeaves = 16'384;
	inline constexpr std::size_t MaximumOccupiedCells = 1'048'576, MaximumEncodedBytes = 16 * 1024 * 1024;
	inline constexpr std::size_t MaximumDefinitions = 1024, MaximumDefinitionLeafLinks = 65'536;

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
	enum class ChannelKind : std::uint8_t { Bool, UInt8, Int32, Float32 };
	enum class DataProfile : std::uint8_t { Authored, Clone, ClientPersistent, RelevantPeers };
	inline constexpr std::size_t MaximumChannels=16, MaximumOverrides=65'536;
	struct ChannelSpec {
		std::string Name;
		ChannelKind Kind{};
		std::uint32_t Default{};
		bool Persistent{}, RelevantPeers{}, Indexed{}, CopyValue{};
		bool operator==(const ChannelSpec&) const = default;
	};
	struct ChannelData {
		struct Entry { std::uint16_t Index; std::uint32_t Bits; };
		using Page=std::vector<Entry>;
		using Directory=std::map<Cell,std::shared_ptr<const Page>,CellOrder>;
		ChannelSpec Spec;
		std::shared_ptr<const Directory> Pages;
		std::size_t Count{};
		[[nodiscard]] std::uint32_t Read(Cell P) const;
	};
	struct ChannelState {
		std::vector<ChannelData> Channels;
		std::size_t Overrides{};
		[[nodiscard]] std::size_t MaxEncodedBytes() const noexcept;
	};
	struct ValueWrite { Cell Position; std::uint8_t Channel; std::uint32_t Bits; };
	struct Transfer { Region Source; Cell Destination; bool Move{}; };
	struct PackedWrite { Region Destination; std::vector<Block> Values; };
	using Operation=std::variant<Write,ValueWrite,Transfer,PackedWrite>;
	using StateValues=std::array<std::uint32_t,MaximumChannels>;
	struct CellState { Block Value; StateValues Values{}; bool operator==(const CellState&) const = default; };
	struct StateChange { Cell Position; CellState Before,After; };
	[[nodiscard]] std::uint32_t ScalarBits(ChannelKind Kind,std::variant<bool,double> Value);
	[[nodiscard]] std::variant<bool,double> ScalarValue(ChannelKind Kind,std::uint32_t Bits);
	void ValidateChannel(const ChannelSpec& Spec);
	[[nodiscard]] Cell LeafOrigin(Cell Position) noexcept;
	[[nodiscard]] unsigned LeafIndex(Cell Position) noexcept;
	[[nodiscard]] Cell OffsetCell(Cell Origin,unsigned Index) noexcept;

	[[nodiscard]] bool ValidCell(Cell Position) noexcept;
	[[nodiscard]] bool ValidBlock(Block Value) noexcept;
	// Throws before an invalid or oversized input can allocate storage.
	[[nodiscard]] std::size_t Volume(Region Bounds, std::size_t Limit = MaximumEditVisits);
	[[nodiscard]] Region SingleCell(Cell Position);

	class Leaf {
	  public:
		[[nodiscard]] Block Get(unsigned X, unsigned Y, unsigned Z) const;
		[[nodiscard]] unsigned GetOccupiedCount() const noexcept { return Occupied; }
		[[nodiscard]] bool HasCustomCells() const noexcept { return CustomCells!=0; }
		[[nodiscard]] std::optional<Block> GetUniform() const noexcept;
		void ForEachOccupied(const std::function<void(unsigned, unsigned, unsigned, Block)>& Visit) const;

	  private:
		friend class Field;
		friend std::vector<std::uint8_t> Encode(const class Field&);
		friend class Field Decode(std::span<const std::uint8_t>);
		friend std::vector<std::uint8_t> EncodeBase(const class Field&);
		friend class Field DecodeBase(std::span<const std::uint8_t>);
		struct Entry { std::uint16_t Index; Block Value; bool operator==(const Entry&) const = default; };
		using Sparse = std::vector<Entry>;
		using Dense = std::vector<Block>;
		static constexpr unsigned SparseLimit = 64;
		std::variant<Block, Sparse, Dense> Storage{Block{}};
		unsigned Occupied{};
		unsigned CustomCells{};
		void CollectDefinitionIds(std::vector<std::uint16_t>& Ids) const;
		[[nodiscard]] std::size_t EncodedSize() const noexcept;
		void Set(unsigned Index, Block Value);
		void Fill(Block Value) noexcept;
		void Compact();
	};

	class Field {
		using Directory = std::map<Cell, std::shared_ptr<const Leaf>, CellOrder>;
		struct Catalog {
			std::vector<Definition> Entries;
			std::map<std::string, std::uint16_t, std::less<>> Names;
			std::size_t AssetCount{}, PinCount{}, AssetBytes{};
		};
		struct Link { std::uint16_t Id; Cell Origin; };
		struct LinkOrder {
			bool operator()(const Link& A,const Link& B) const noexcept {
				return A.Id!=B.Id ? A.Id<B.Id : CellOrder{}(A.Origin,B.Origin);
			}
		};
		using Links = std::set<Link,LinkOrder>;
	  public:
		class Candidate {
		  public:
			[[nodiscard]] Block Get(Cell Position) const;
			[[nodiscard]] const Leaf* GetLeaf(Cell AnyCell) const;
			[[nodiscard]] const Definition& GetDefinition(std::uint16_t Id) const;
			[[nodiscard]] std::size_t GetLeafCount() const noexcept { return LeafCount; }
			[[nodiscard]] std::size_t GetOccupiedCount() const noexcept { return OccupiedCount; }
			void ForEachLeaf(const std::function<void(Cell,const Leaf&)>& Visit) const;
			// An absent leaf means its last occupancy was removed. Changed origins are unique.
			void ForEachChangedLeaf(const std::function<void(Cell,const Leaf*)>& Visit) const;
		  private:
			friend class Field;
			Candidate(const Field& Prior, const Catalog* Definitions, const Directory& Added, std::span<const Cell> Erased,
				std::size_t Pages, std::size_t Cells) : Base(Prior), Definitions(Definitions), Replacements(Added), Removed(Erased),
				LeafCount(Pages), OccupiedCount(Cells) {}
			const Field& Base;
			const Catalog* Definitions;
			const Directory& Replacements;
			std::span<const Cell> Removed;
			std::size_t LeafCount, OccupiedCount;
		};
		[[nodiscard]] Block Get(Cell Position) const;
		[[nodiscard]] std::uint16_t GetDefinitionId(std::string_view Key) const;
		[[nodiscard]] std::optional<std::uint16_t> FindDefinitionId(std::string_view Key) const;
		[[nodiscard]] const Definition& GetDefinition(std::uint16_t Id) const;
		[[nodiscard]] std::size_t GetDefinitionCount() const noexcept;
		[[nodiscard]] bool HasAssetBindings() const noexcept { return Definitions && Definitions->AssetCount; }
		// Atomically replaces only runtime resource views. Source pins and public revision stay stable.
		void HydrateAssets(const std::function<std::shared_ptr<const AssetBinding>(const AssetBinding&)>& Resolve);
		[[nodiscard]] bool IsValid(Block Value) const noexcept;
		[[nodiscard]] std::uint64_t GetRevision() const noexcept { return Revision; }
		[[nodiscard]] std::uint64_t GetGeometryRevision() const noexcept { return GeometryRevision; }
		[[nodiscard]] std::uint64_t GetPublicRevision() const noexcept { return PublicRevision; }
		[[nodiscard]] std::size_t GetChannelCount() const noexcept { return State?State->Channels.size():0; }
		[[nodiscard]] std::size_t GetOverrideCount() const noexcept { return State?State->Overrides:0; }
		[[nodiscard]] std::size_t GetSnapshotBytes() const noexcept;
		[[nodiscard]] const ChannelData& GetChannel(std::uint8_t Id) const;
		[[nodiscard]] std::uint8_t GetChannelId(std::string_view Name) const;
		[[nodiscard]] std::uint32_t GetValue(Cell P,std::uint8_t Channel) const;
		[[nodiscard]] CellState ReadState(Cell P) const;
		[[nodiscard]] bool DefineChannel(ChannelSpec Value);
		void UndoChannelInsertion(const ChannelSpec& Expected);
		[[nodiscard]] std::size_t GetOccupiedCount() const noexcept { return Occupied; }
		[[nodiscard]] std::size_t GetLeafCount() const noexcept { return Leaves.size(); }
		// Visits immutable values in this Main-owned snapshot. The cell argument is the leaf's grid-local minimum.
		void ForEachLeaf(const std::function<void(Cell, const Leaf&)>& Visit) const;
		void ForEachOccupied(const std::function<void(Cell, Block)>& Visit) const;
		// All commands share a single commit. Unchanged data preserves the revision.
		using Admission = std::function<bool(const Candidate&, std::span<const Write>)>;
		void InstallReplica(Field Value, const Admission &BeforeCommit);
		[[nodiscard]] bool Define(Definition Spec,bool Replace=false,const Admission& BeforeCommit={});
		void UndoDefinitionInsertion(const Definition& Expected,const Admission& BeforeCommit={});
		[[nodiscard]] bool Apply(std::span<const Write> Writes, std::optional<std::uint64_t> Expected = {},
			const Admission& BeforeCommit = {});
		[[nodiscard]] bool ApplyCellPatch(std::span<const Write> Cells, const Admission& BeforeCommit = {});
		[[nodiscard]] bool ApplyOperations(std::span<const Operation> Commands,const Admission& BeforeCommit={});
		[[nodiscard]] std::vector<StateChange> PlanOperations(std::span<const Operation> Commands,std::size_t CommandLimit=MaximumCommands) const;
		[[nodiscard]] bool ApplyChanges(std::span<const StateChange> Changes,const Admission& BeforeCommit={});
		[[nodiscard]] bool ApplyMaterialChanges(std::span<const Definition> Materials,std::span<const StateChange> Changes,
			bool Remove,const Admission& BeforeCommit={});

	  private:
		friend std::vector<std::uint8_t> Encode(const Field&);
		friend Field Decode(std::span<const std::uint8_t>);
		friend std::vector<std::uint8_t> Encode(const Field&,DataProfile);
		friend std::vector<std::uint8_t> EncodeBase(const Field&);
		friend Field DecodeBase(std::span<const std::uint8_t>);
		[[nodiscard]] bool ApplyChecked(std::span<const Write> Writes, const Admission& BeforeCommit,std::size_t StateBytes=0);
		[[nodiscard]] static const Definition& Lookup(const Catalog* Bindings,std::uint16_t Id);
		Directory Leaves;
		std::shared_ptr<const Catalog> Definitions;
		std::shared_ptr<Links> Dependencies;
		std::shared_ptr<const ChannelState> State;
		std::size_t Occupied{};
		std::size_t EncodedBytes{9};
		std::uint64_t Revision{1}, GeometryRevision{1}, PublicRevision{1};
	};

	[[nodiscard]] std::vector<std::uint8_t> Encode(const Field& Value);
	[[nodiscard]] std::vector<std::uint8_t> Encode(const Field& Value,DataProfile Profile);
	[[nodiscard]] Field Decode(std::span<const std::uint8_t> Bytes);
	void ValidateDefinition(const Definition& Value);
	[[nodiscard]] std::size_t AssetBindingBytes(const AssetBinding& Value) noexcept;
	[[nodiscard]] std::size_t AssetPinCharge(const Definition& Value) noexcept;
	[[nodiscard]] std::string DefinitionDigest(const Definition& Value);
}
