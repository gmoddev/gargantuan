// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Transfer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		Cell MapCell(Cell P,GridLattice Mapping) {
			const auto& B=Basis(Mapping.Orientation);
			const std::array<int,3> Source{P.X,P.Y,P.Z};
			std::array<std::int64_t,3> Result{Mapping.Origin.X,Mapping.Origin.Y,Mapping.Origin.Z};
			for (unsigned I=0;I<3;++I) for (unsigned J=0;J<3;++J) {
				if (B[I][J]>0) Result[J]+=Source[I];
				else if (B[I][J]<0) Result[J]-=static_cast<std::int64_t>(Source[I])+1;
			}
			for (auto V:Result) if (V < -CoordinateLimit || V >= CoordinateLimit)
				throw std::out_of_range("[BlockGrid:Bounds] Mapped cell is outside supported coordinates");
			return {static_cast<int>(Result[0]),static_cast<int>(Result[1]),static_cast<int>(Result[2])};
		}
		CellState AirState(const Field& F) {
			CellState Result;
			for (std::uint8_t I=0;I<F.GetChannelCount();++I) Result.Values[I]=F.GetChannel(I).Spec.Default;
			return Result;
		}
	}
	TransferFields PlanGridTransfer(const Field& Source,const Field& Destination,std::optional<Region> Bounds,GridLattice Mapping) {
		if (&Source==&Destination || Mapping.Orientation>=24)
			throw std::invalid_argument("[BlockGrid:Transfer] Expected two distinct grids and a proper lattice rotation");
		const auto Visits=Bounds?Volume(*Bounds,MaximumTransferCells):Source.GetOccupiedCount();
		if (Visits>MaximumTransferCells) throw std::length_error("[BlockGrid:Limit] Transfer exceeds the cell budget");
		std::vector<Cell> Selected;
		Selected.reserve(Visits);
		if (Bounds) {
			const auto [Low,High]=*Bounds;
			for (int Z=Low.Z;Z<High.Z;++Z) for (int Y=Low.Y;Y<High.Y;++Y) for (int X=Low.X;X<High.X;++X) {
				const Cell P{X,Y,Z}; if (Source.Get(P).Definition) Selected.push_back(P);
			}
		} else Source.ForEachOccupied([&](Cell P,Block){Selected.push_back(P);});
		if (Selected.empty()) throw std::invalid_argument("[BlockGrid:Transfer] Selection contains no blocks");
		TransferFields Result{Source,Destination};
		std::array<std::uint8_t,MaximumChannels> Channels{};
		for (std::uint8_t I=0;I<Source.GetChannelCount();++I) {
			const auto& Spec=Source.GetChannel(I).Spec;
			(void)Result.Destination.DefineChannel(Spec);
			Channels[I]=Result.Destination.GetChannelId(Spec.Name);
		}
		const auto SourceAir=AirState(Source),DestinationAir=AirState(Result.Destination);
		std::array<std::uint16_t,MaximumDefinitions+Brick+1> Definitions{};
		unsigned Imported=0;
		std::vector<StateChange> Remove,Insert;
		Remove.reserve(Selected.size()); Insert.reserve(Selected.size());
		for (Cell P:Selected) {
			const auto Previous=Source.ReadState(P);
			const Cell Q=MapCell(P,Mapping);
			if (Destination.Get(Q).Definition) throw std::domain_error("[BlockGrid:Conflict] Destination cell is occupied");
			const auto& Original=Previous.Value;
			auto& Id=Definitions[Original.Definition];
			if (!Id) {
				const auto& Spec=Source.GetDefinition(Original.Definition);
				if (const auto Existing=Result.Destination.FindDefinitionId(Spec.Key)) {
					if (Result.Destination.GetDefinition(*Existing)!=Spec)
						throw std::domain_error("[BlockGrid:Conflict] Destination definition differs");
					Id=*Existing;
				} else {
					if (++Imported>MaximumTransferDefinitions) throw std::length_error("[BlockGrid:Limit] Too many new transfer definitions");
					(void)Result.Destination.Define(Spec);
					Id=Result.Destination.GetDefinitionId(Spec.Key);
				}
			}
			const auto Faces=OrientationFaces(Original.Orientation);
			CellState Next=DestinationAir;
			Next.Value={Id,OrientationIndex(RotateFace(Faces[0],Mapping.Orientation),RotateFace(Faces[1],Mapping.Orientation))};
			for (std::uint8_t I=0;I<Source.GetChannelCount();++I) Next.Values[Channels[I]]=Previous.Values[I];
			Remove.push_back({P,Previous,SourceAir});
			Insert.push_back({Q,DestinationAir,Next});
		}
		std::ranges::sort(Remove,CellOrder{},&StateChange::Position);
		std::ranges::sort(Insert,CellOrder{},&StateChange::Position);
		(void)Result.Source.ApplyChanges(Remove);
		(void)Result.Destination.ApplyChanges(Insert);
		Result.Source.AdvanceTransferRevision(Source); Result.Destination.AdvanceTransferRevision(Destination);
		return Result;
	}
	void Field::AdvanceTransferRevision(const Field& Previous) {
		if (Previous.Revision==std::numeric_limits<std::uint64_t>::max() ||
			Previous.PublicRevision==std::numeric_limits<std::uint64_t>::max() ||
			Previous.GeometryRevision==std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("[BlockGrid:Revision] Transfer revision exhausted");
		Revision=Previous.Revision+1; PublicRevision=Previous.PublicRevision+1; GeometryRevision=Previous.GeometryRevision+1;
	}
	bool Field::AdmitTransferFrom(const Field& Previous,const Admission& Check) const {
		Directory Replaced;
		std::vector<Cell> Removed;
		auto A=Previous.Leaves.begin(),B=Leaves.begin();
		while (A!=Previous.Leaves.end() || B!=Leaves.end()) {
			if (B==Leaves.end() || (A!=Previous.Leaves.end() && CellOrder{}(A->first,B->first))) {
				Removed.push_back(A->first); ++A;
			} else if (A==Previous.Leaves.end() || CellOrder{}(B->first,A->first)) {
				Replaced.insert(*B++);
			} else {
				if (A->second!=B->second) Replaced.insert(*B);
				++A; ++B;
			}
		}
		return Check(Candidate(Previous,Definitions.get(),Replaced,Removed,Leaves.size(),Occupied),{});
	}
	bool Field::HasSameContents(const Field& Other) const {
		if (Leaves.size()!=Other.Leaves.size() || Occupied!=Other.Occupied || GetChannelCount()!=Other.GetChannelCount() ||
			GetDefinitionCount()!=Other.GetDefinitionCount() || GetOverrideCount()!=Other.GetOverrideCount()) return false;
		if (GetDefinitionCount() && Definitions!=Other.Definitions && Definitions->Entries!=Other.Definitions->Entries) return false;
		auto B=Other.Leaves.begin();
		for (const auto& [Origin,Page]:Leaves) {
			if (Origin!=B->first || (Page!=B->second && Page->Storage!=B->second->Storage)) return false;
			++B;
		}
		if (State!=Other.State) for (std::uint8_t I=0;I<GetChannelCount();++I) {
			const auto& X=GetChannel(I); const auto& Y=Other.GetChannel(I);
			if (X.Spec!=Y.Spec || X.Count!=Y.Count) return false;
			if (X.Pages==Y.Pages) continue;
			if (!X.Pages || !Y.Pages || X.Pages->size()!=Y.Pages->size()) return false;
			auto J=Y.Pages->begin();
			for (const auto& [Origin,Page]:*X.Pages) {
				if (Origin!=J->first || (Page!=J->second && *Page!=*J->second)) return false;
				++J;
			}
		}
		return true;
	}
}
