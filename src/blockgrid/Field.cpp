// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace gargantuan::blockgrid {
	namespace {
		std::int32_t LeafMin(std::int32_t N) noexcept {
			const auto Remainder = N % static_cast<std::int32_t>(LeafSide);
			return N - Remainder - (Remainder < 0 ? static_cast<std::int32_t>(LeafSide) : 0);
		}
		Cell Origin(Cell P) noexcept { return {LeafMin(P.X), LeafMin(P.Y), LeafMin(P.Z)}; }
		unsigned Index(unsigned X, unsigned Y, unsigned Z) noexcept { return X + LeafSide * (Y + LeafSide * Z); }
	}
	bool CellOrder::operator()(Cell A, Cell B) const noexcept {
		return std::tie(A.Z,A.Y,A.X) < std::tie(B.Z,B.Y,B.X);
	}
	bool ValidCell(Cell P) noexcept {
		return P.X >= -CoordinateLimit && P.X < CoordinateLimit && P.Y >= -CoordinateLimit &&
			P.Y < CoordinateLimit && P.Z >= -CoordinateLimit && P.Z < CoordinateLimit;
	}
	bool ValidBlock(Block B) noexcept {
		return B.Definition <= Brick && B.Orientation < 24 && (B.Definition != Air || B.Orientation == 0);
	}
	std::size_t Volume(Region R, std::size_t Limit) {
		if (!ValidCell(R.Min) || R.MaxExclusive.X > CoordinateLimit || R.MaxExclusive.Y > CoordinateLimit ||
			R.MaxExclusive.Z > CoordinateLimit || R.MaxExclusive.X <= R.Min.X || R.MaxExclusive.Y <= R.Min.Y ||
			R.MaxExclusive.Z <= R.Min.Z) throw std::invalid_argument("[BlockGrid:Bounds] Invalid half-open cell region");
		std::size_t Result = 1;
		for (auto Width : {R.MaxExclusive.X-R.Min.X, R.MaxExclusive.Y-R.Min.Y, R.MaxExclusive.Z-R.Min.Z}) {
			if (static_cast<std::size_t>(Width) > Limit/Result)
				throw std::length_error("[BlockGrid:Limit] Cell work budget exceeded");
			Result *= static_cast<std::size_t>(Width);
		}
		return Result;
	}
	Region SingleCell(Cell P) {
		if (!ValidCell(P)) throw std::out_of_range("[BlockGrid:Bounds] Cell outside supported coordinates");
		return {P, {P.X+1, P.Y+1, P.Z+1}};
	}
	Block Leaf::Get(unsigned X, unsigned Y, unsigned Z) const {
		if (X>=LeafSide || Y>=LeafSide || Z>=LeafSide) throw std::out_of_range("[BlockGrid:Leaf] Invalid offset");
		const auto I = Index(X,Y,Z);
		if (const auto* Uniform = std::get_if<Block>(&Storage)) return *Uniform;
		if (const auto* Values = std::get_if<Dense>(&Storage)) return (*Values)[I];
		const auto& Values = std::get<Sparse>(Storage);
		const auto Found = std::ranges::lower_bound(Values, I, {}, &Entry::Index);
		return Found != Values.end() && Found->Index == I ? Found->Value : Block{};
	}
	std::optional<Block> Leaf::GetUniform() const noexcept {
		if (const auto* Value = std::get_if<Block>(&Storage)) return *Value;
		return {};
	}
	std::size_t Leaf::EncodedSize() const noexcept {
		if (!Occupied) return 0;
		if (std::holds_alternative<Block>(Storage)) return 16;
		if (std::holds_alternative<Sparse>(Storage)) return 15 + 5 * Occupied;
		return 13 + 3 * LeafVolume;
	}
	void Leaf::Fill(Block Value) noexcept {
		Storage = Value;
		Occupied = Value.Definition == Air ? 0 : LeafVolume;
	}
	void Leaf::Set(unsigned I, Block Value) {
		if (const auto* Uniform = std::get_if<Block>(&Storage)) {
			if (*Uniform == Value) return;
			if (Uniform->Definition == Air) Storage.emplace<Sparse>();
			else {
				const auto Prior = *Uniform;
				Storage.emplace<Dense>(LeafVolume, Prior);
			}
		}
		if (auto* Values = std::get_if<Sparse>(&Storage)) {
			auto Found = std::ranges::lower_bound(*Values, I, {}, &Entry::Index);
			if (Found != Values->end() && Found->Index == I) {
				if (Value.Definition == Air) { Values->erase(Found); --Occupied; }
				else Found->Value = Value;
			} else if (Value.Definition != Air) {
				Values->insert(Found, Entry{static_cast<std::uint16_t>(I), Value});
				++Occupied;
				if (Occupied > SparseLimit) {
					Dense Expanded(LeafVolume);
					for (const auto& E : *Values) Expanded[E.Index] = E.Value;
					Storage = std::move(Expanded);
				}
			}
			return;
		}
		auto& Old = std::get<Dense>(Storage)[I];
		Occupied += static_cast<unsigned>(Value.Definition != Air);
		Occupied -= static_cast<unsigned>(Old.Definition != Air);
		Old = Value;
	}
	void Leaf::Compact() {
		if (!Occupied) { Fill({}); return; }
		auto* Values = std::get_if<Dense>(&Storage);
		if (!Values) return;
		if (Occupied <= SparseLimit) {
			Sparse Entries;
			Entries.reserve(Occupied);
			for (unsigned I=0; I<LeafVolume; ++I)
				if ((*Values)[I].Definition != Air) Entries.push_back({static_cast<std::uint16_t>(I), (*Values)[I]});
			Storage = std::move(Entries);
		} else if (Occupied == LeafVolume &&
			std::ranges::all_of(*Values, [&](Block B){ return B == Values->front(); })) {
			const auto Uniform = Values->front();
			Fill(Uniform);
		}
	}
	void Leaf::ForEachOccupied(const std::function<void(unsigned,unsigned,unsigned,Block)>& Visit) const {
		const auto Emit = [&](unsigned I, Block B){ Visit(I%LeafSide, (I/LeafSide)%LeafSide, I/(LeafSide*LeafSide), B); };
		if (const auto* Values = std::get_if<Sparse>(&Storage)) {
			for (const auto& E : *Values) Emit(E.Index, E.Value);
		} else if (const auto* Values = std::get_if<Dense>(&Storage)) {
			for (unsigned I=0; I<LeafVolume; ++I) if ((*Values)[I].Definition != Air) Emit(I, (*Values)[I]);
		} else if (const auto Uniform = std::get<Block>(Storage); Uniform.Definition != Air) {
			for (unsigned I=0; I<LeafVolume; ++I) Emit(I, Uniform);
		}
	}
	Block Field::Get(Cell P) const {
		if (!ValidCell(P)) throw std::out_of_range("[BlockGrid:Bounds] Cell outside supported coordinates");
		const auto Key = Origin(P);
		auto Found = Leaves.find(Key);
		return Found == Leaves.end() ? Block{} : Found->second->Get(P.X-Key.X, P.Y-Key.Y, P.Z-Key.Z);
	}
	void Field::ForEachLeaf(const std::function<void(Cell,const Leaf&)>& Visit) const {
		for (const auto& [Base, Page] : Leaves) Visit(Base, *Page);
	}
	void Field::ForEachOccupied(const std::function<void(Cell,Block)>& Visit) const {
		for (const auto& [Base, Page] : Leaves)
			Page->ForEachOccupied([&](unsigned X,unsigned Y,unsigned Z,Block B) {
				Visit({Base.X+static_cast<int>(X), Base.Y+static_cast<int>(Y), Base.Z+static_cast<int>(Z)}, B);
			});
	}
	const Leaf* Field::Candidate::GetLeaf(Cell P) const {
		if (!ValidCell(P)) throw std::out_of_range("[BlockGrid:Bounds] Cell outside supported coordinates");
		const auto Key = Origin(P);
		if (auto It=Replacements.find(Key); It!=Replacements.end()) return It->second.get();
		if (std::ranges::binary_search(Removed,Key,CellOrder{})) return nullptr;
		const auto It = Base.Leaves.find(Key);
		return It==Base.Leaves.end() ? nullptr : It->second.get();
	}
	Block Field::Candidate::Get(Cell P) const {
		const auto* Page = GetLeaf(P);
		if (!Page) return {};
		const auto Key=Origin(P);
		return Page->Get(P.X-Key.X,P.Y-Key.Y,P.Z-Key.Z);
	}
	void Field::Candidate::ForEachChangedLeaf(const std::function<void(Cell,const Leaf*)>& Visit) const {
		for (auto Key : Removed) Visit(Key,nullptr);
		for (const auto& [Key,Page] : Replacements) Visit(Key,Page.get());
	}
	void Field::Candidate::ForEachLeaf(const std::function<void(Cell,const Leaf&)>& Visit) const {
		auto A=Base.Leaves.begin(), B=Replacements.begin();
		while (A!=Base.Leaves.end() || B!=Replacements.end()) {
			if (B==Replacements.end() || (A!=Base.Leaves.end() && CellOrder{}(A->first,B->first))) {
				if (!std::ranges::binary_search(Removed,A->first,CellOrder{})) Visit(A->first,*A->second);
				++A;
			} else {
				if (A!=Base.Leaves.end() && A->first==B->first) ++A;
				Visit(B->first,*B->second);
				++B;
			}
		}
	}
	bool Field::Apply(std::span<const Write> Writes, std::optional<std::uint64_t> Expected, const Admission& BeforeCommit) {
		if (Expected && *Expected != Revision) throw std::domain_error("[BlockGrid:Conflict] Grid revision changed");
		if (Writes.size() > MaximumCommands) throw std::length_error("[BlockGrid:Limit] Too many edit commands");
		return ApplyChecked(Writes,BeforeCommit);
	}
	bool Field::ApplyCellPatch(std::span<const Write> Writes,const Admission& BeforeCommit) {
		if (Writes.size()>MaximumEditVisits) throw std::length_error("[BlockGrid:Limit] Cell patch limit exceeded");
		for (const auto& W : Writes)
			if (!(SingleCell(W.Bounds.Min).MaxExclusive==W.Bounds.MaxExclusive))
				throw std::invalid_argument("[BlockGrid:History] Expected individual cell writes");
		return ApplyChecked(Writes,BeforeCommit);
	}
	bool Field::ApplyChecked(std::span<const Write> Writes,const Admission& BeforeCommit) {
		std::size_t Visits = 0;
		for (const auto& W : Writes) {
			if (!ValidBlock(W.Value)) throw std::invalid_argument("[BlockGrid:Definition] Invalid definition or orientation");
			Visits += Volume(W.Bounds, MaximumEditVisits-Visits);
		}
		std::map<Cell, Leaf, CellOrder> Staged;
		for (const auto& W : Writes) {
			const auto First = Origin(W.Bounds.Min), End = W.Bounds.MaxExclusive;
			for (auto Z=First.Z; Z<End.Z; Z+=LeafSide) for (auto Y=First.Y; Y<End.Y; Y+=LeafSide)
				for (auto X=First.X; X<End.X; X+=LeafSide) {
					const Cell Key{X,Y,Z};
					auto Found = Staged.find(Key);
					if (Found == Staged.end()) {
						const auto Old = Leaves.find(Key);
						if (Old == Leaves.end() && W.Value.Definition == Air) continue;
						Found = Staged.emplace(Key, Old == Leaves.end() ? Leaf{} : *Old->second).first;
					}
					auto& Page = Found->second;
					constexpr auto Side = static_cast<int>(LeafSide);
					const Cell Lo{std::max(X,W.Bounds.Min.X)-X, std::max(Y,W.Bounds.Min.Y)-Y, std::max(Z,W.Bounds.Min.Z)-Z};
					const Cell Hi{std::min(X+Side,End.X)-X, std::min(Y+Side,End.Y)-Y, std::min(Z+Side,End.Z)-Z};
					if (Lo == Cell{} && Hi == Cell{Side,Side,Side}) Page.Fill(W.Value);
					else for (int K=Lo.Z; K<Hi.Z; ++K) for (int J=Lo.Y; J<Hi.Y; ++J)
						for (int I=Lo.X; I<Hi.X; ++I) Page.Set(Index(I,J,K), W.Value);
				}
		}
		Directory Replacements;
		std::vector<Cell> Removed;
		auto NewOccupied = Occupied, NewLeaves = Leaves.size(), NewBytes = EncodedBytes;
		for (auto& [Key, Page] : Staged) {
			Page.Compact();
			const auto Old = Leaves.find(Key);
			if (Old != Leaves.end() && Old->second->Storage == Page.Storage) continue;
			if (Old != Leaves.end()) {
				NewOccupied -= Old->second->Occupied;
				NewBytes -= Old->second->EncodedSize();
				--NewLeaves;
			}
			if (Page.Occupied) {
				NewOccupied += Page.Occupied;
				NewBytes += Page.EncodedSize();
				++NewLeaves;
				Replacements.emplace(Key, std::make_shared<const Leaf>(std::move(Page)));
			} else if (Old != Leaves.end()) Removed.push_back(Key);
		}
		if (Replacements.empty() && Removed.empty()) return false;
		if (NewOccupied > MaximumOccupiedCells || NewLeaves > MaximumLeaves || NewBytes > MaximumEncodedBytes ||
			Revision == std::numeric_limits<std::uint64_t>::max()) throw std::length_error("[BlockGrid:Limit] Grid capacity exhausted");
		const Candidate Next(*this,Replacements,Removed,NewLeaves,NewOccupied);
		if (BeforeCommit && !BeforeCommit(Next, Writes)) throw std::length_error("[BlockGrid:Admission] Provider capacity denied");
		// From this point installation only moves prepared map nodes/pointers or removes old nodes.
		for (const auto& Key : Removed) Leaves.erase(Key);
		for (auto& [Key, Page] : Replacements)
			if (const auto Old = Leaves.find(Key); Old != Leaves.end()) Old->second = std::move(Page);
		Leaves.merge(Replacements);
		Occupied = NewOccupied;
		EncodedBytes = NewBytes;
		++Revision;
		return true;
	}
}
