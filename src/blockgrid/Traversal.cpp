// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Traversal.hpp"
#include <algorithm>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		bool Contains(Region R,Cell P) {
			return P.X>=R.Min.X && P.X<R.MaxExclusive.X && P.Y>=R.Min.Y && P.Y<R.MaxExclusive.Y && P.Z>=R.Min.Z && P.Z<R.MaxExclusive.Z;
		}
		void ValidateIds(const Field& F,std::vector<std::uint16_t>& Ids) {
			if (Ids.size()>64) throw std::length_error("[BlockGrid:Limit] Too many definition selectors");
			std::ranges::sort(Ids);
			if (std::adjacent_find(Ids.begin(),Ids.end())!=Ids.end()) throw std::invalid_argument("[BlockGrid:Filter] Repeated definition");
			for (const auto Id:Ids) if (!Id || !F.IsValid({Id,0})) throw std::invalid_argument("[BlockGrid:Definition] Unknown filter definition");
		}
		unsigned FaceIndex(Cell D) { return D.X?unsigned(D.X<0):D.Y?2u+unsigned(D.Y<0):4u+unsigned(D.Z<0); }
		std::uint8_t WorldPorts(const Field& F,Block B) {
			const auto Mask=F.GetDefinition(B.Definition).Ports;
			if (Mask==0 || Mask==63 || !B.Orientation) return Mask;
			std::uint8_t Rotated{};
			for (unsigned I=0;I<6;++I) if (Mask&(1u<<I)) Rotated|=static_cast<std::uint8_t>(1u<<RotateFace(I,B.Orientation));
			return Rotated;
		}
	}
	void ValidateFilter(const Field& F,Filter& V) {
		if (V.Definitions) ValidateIds(F,*V.Definitions);
		if (V.Where) (void)ScalarValue(F.GetChannel(V.Where->Channel).Spec.Kind,V.Where->Bits);
	}
	bool Matches(const Field& F,const Filter& S,Cell P,Block B) {
		return B.Definition && (!S.Definitions || std::ranges::binary_search(*S.Definitions,B.Definition)) &&
			(!S.Where || F.GetChannel(S.Where->Channel).Read(P)==S.Where->Bits);
	}
	bool Traversal::Eligible(Cell P,Block B) const { return Matches(*Pin,Options.Select,P,B); }
	void Traversal::Admit(Cell P) {
		if (Seen.contains(P)) return;
		if (Seen.size()==Options.MaxVisited) { Finish(PageStatus::Limit); return; }
		Seen.insert(P); Queue.push_back(P);
	}
	Traversal::Traversal(std::shared_ptr<const Field> Snapshot,std::span<const Cell> Starts,TraversalOptions Params):
		Pin(std::move(Snapshot)), Options(std::move(Params)) {
		(void)Volume(Options.Bounds,MaximumOccupiedCells);
		if (Starts.empty() || Starts.size()>64 || Options.MaxVisited==0 || Options.MaxVisited>65'536 ||
			Options.Neighbors>Adjacency::Corner || (Options.UsePorts && Options.Neighbors!=Adjacency::Face))
			throw std::invalid_argument("[BlockGrid:Traversal] Invalid seeds, adjacency, ports or visit limit");
		ValidateFilter(*Pin,Options.Select); ValidateIds(*Pin,Options.StopAt);
		std::set<Cell,CellOrder> Seeds;
		for (auto P:Starts) if (!Contains(Options.Bounds,P) || !Seeds.insert(P).second)
			throw std::invalid_argument("[BlockGrid:Traversal] Duplicate or out-of-bounds seed");
		for (int Z=-1;Z<=1;++Z) for (int Y=-1;Y<=1;++Y) for (int X=-1;X<=1;++X) {
			const unsigned Axes=unsigned(X!=0)+unsigned(Y!=0)+unsigned(Z!=0);
			if (!Axes || Axes>1+static_cast<unsigned>(Options.Neighbors)) continue;
			const Cell D{X,Y,Z};
			Offsets[OffsetCount++]={D,Axes==1?static_cast<int>(FaceIndex(D)):-1};
		}
		for (Cell P:Starts) if (Eligible(P,Pin->Get(P))) {
			Admit(P); if (Status!=PageStatus::More) break;
		}
		if (Queue.empty()) Finish(PageStatus::Complete);
	}
	Traversal::Traversal(std::shared_ptr<const Field> Snapshot,std::uint8_t Id,Region Bounds):
		Pin(std::move(Snapshot)),Options{Bounds},Active(Id) {
		(void)Volume(Bounds,MaximumOccupiedCells);
		const auto& C=Pin->GetChannel(Id);
		if (!C.Spec.Indexed) throw std::invalid_argument("[BlockGrid:Unsupported] FindActive requires an indexed channel");
		if (!C.Pages) { Finish(PageStatus::Complete); return; }
		CurrentPage=C.Pages->lower_bound({-CoordinateLimit,-CoordinateLimit,LeafOrigin(Bounds.Min).Z});
	}
	void Traversal::Finish(PageStatus Reason) {
		if (Status!=PageStatus::More) return;
		Status=Reason;
		CurrentPage={};
		Seen.clear(); Queue.clear(); Expanding.reset(); Pin.reset();
	}
	Page Traversal::Step(std::size_t Budget) {
		if (!Budget || Budget>1024) throw std::invalid_argument("[BlockGrid:Traversal] Step budget must be 1 through 1024");
		Page Result{Status};
		if (Status==PageStatus::More) Result.Cells.reserve(Budget);
		for (std::size_t I=0;I<Budget && Status==PageStatus::More;++I) {
			if (Active) {
				const auto& C=Pin->GetChannel(*Active);
				if (CurrentPage==C.Pages->end() || CurrentPage->first.Z>=Options.Bounds.MaxExclusive.Z) { Finish(PageStatus::Complete); break; }
				const auto Base=CurrentPage->first;
				const auto& Entries=*CurrentPage->second;
				if (Base.X>=Options.Bounds.MaxExclusive.X || Base.X+int(LeafSide)<=Options.Bounds.Min.X ||
					Base.Y>=Options.Bounds.MaxExclusive.Y || Base.Y+int(LeafSide)<=Options.Bounds.Min.Y) {
					++CurrentPage; Entry=0; continue;
				}
				const auto P=OffsetCell(Base,Entries[Entry++].Index);
				if (Entry==Entries.size()) { ++CurrentPage; Entry=0; }
				if (Contains(Options.Bounds,P)) {
					if (Emitted==Options.MaxVisited) { Finish(PageStatus::Limit); break; }
					Result.Cells.push_back(P); ++Emitted;
				}
			} else if (!Expanding) {
				if (Queue.empty()) { Finish(PageStatus::Complete); break; }
				Expanding=Queue.front(); Queue.pop_front(); NextNeighbor=0;
				const auto B=Pin->Get(*Expanding);
				Result.Cells.push_back(*Expanding);
				if (std::ranges::binary_search(Options.StopAt,B.Definition)) {
					Result.Match=Expanding; Finish(PageStatus::Found); break;
				}
				if (Options.UsePorts) Ports=WorldPorts(*Pin,B);
			} else {
				const auto [D,Face]=Offsets[NextNeighbor++];
				const Cell P{Expanding->X+D.X,Expanding->Y+D.Y,Expanding->Z+D.Z};
				if (NextNeighbor==OffsetCount) Expanding.reset();
				if (!Contains(Options.Bounds,P) || Seen.contains(P) || (Options.UsePorts && !(Ports&(1u<<Face)))) continue;
				const auto B=Pin->Get(P);
				if (Eligible(P,B) && (!Options.UsePorts || (WorldPorts(*Pin,B)&(1u<<(Face^1))))) Admit(P);
			}
		}
		if (Status==PageStatus::More && !Active && Queue.empty() && !Expanding) Finish(PageStatus::Complete);
		Result.Status=Status;
		return Result;
	}
}
