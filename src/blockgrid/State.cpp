// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		[[noreturn]] void InvalidValue(){ throw std::invalid_argument("[BlockGrid:Value] Invalid channel value"); }
		template<class Callback> void Each(Region R,Callback Visit) {
			for (int Z=R.Min.Z;Z<R.MaxExclusive.Z;++Z) for (int Y=R.Min.Y;Y<R.MaxExclusive.Y;++Y)
				for (int X=R.Min.X;X<R.MaxExclusive.X;++X) Visit(Cell{X,Y,Z});
		}
	}
	void ValidateChannel(const ChannelSpec& S) {
			const auto Lower=[](char C){return C>='a' && C<='z';};
			if (S.Name.empty() || S.Name.size()>48 || !Lower(S.Name.front()) || !std::ranges::all_of(S.Name,[&](char C) {
				return Lower(C) || (C>='0' && C<='9') || C=='-' || C=='_';
			})) throw std::invalid_argument("[BlockGrid:Channel] Invalid channel name");
			(void)ScalarValue(S.Kind,S.Default);
	}
	std::uint32_t ScalarBits(ChannelKind Kind,std::variant<bool,double> V) {
		if (Kind==ChannelKind::Bool) {
			if (const auto B=std::get_if<bool>(&V)) return static_cast<unsigned>(*B);
			InvalidValue();
		}
		const auto* N=std::get_if<double>(&V);
		if (!N || !std::isfinite(*N)) InvalidValue();
		switch (Kind) {
			case ChannelKind::UInt8:
				if (*N<0 || *N>255 || std::trunc(*N)!=*N) InvalidValue();
				return static_cast<std::uint32_t>(*N);
			case ChannelKind::Int32:
				if (*N<std::numeric_limits<std::int32_t>::min() || *N>std::numeric_limits<std::int32_t>::max() || std::trunc(*N)!=*N) InvalidValue();
				return std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(*N));
			case ChannelKind::Float32: {
				if (std::abs(*N)>std::numeric_limits<float>::max()) InvalidValue();
				const auto F=static_cast<float>(*N);
				return F==0?0:std::bit_cast<std::uint32_t>(F);
			}
			default: InvalidValue();
		}
	}
	std::variant<bool,double> ScalarValue(ChannelKind Kind,std::uint32_t Bits) {
		switch (Kind) {
			case ChannelKind::Bool: if (Bits>1) InvalidValue(); return Bits!=0;
			case ChannelKind::UInt8: if (Bits>255) InvalidValue(); return double(Bits);
			case ChannelKind::Int32: return double(std::bit_cast<std::int32_t>(Bits));
			case ChannelKind::Float32: {
				const float F=std::bit_cast<float>(Bits);
				if (!std::isfinite(F) || (F==0 && Bits)) InvalidValue();
				return double(F);
			}
			default: InvalidValue();
		}
	}
	std::size_t ChannelState::MaxEncodedBytes() const noexcept {
		std::size_t Bytes=10+16*Overrides;
		for (const auto& C:Channels) Bytes+=11+C.Spec.Name.size();
		return Bytes;
	}
	std::uint32_t ChannelData::Read(Cell P) const {
		if (Pages) {
			const auto It=Pages->find(LeafOrigin(P));
			if (It!=Pages->end()) {
				const auto Index=LeafIndex(P);
				const auto EntryIt=std::ranges::lower_bound(*It->second,Index,{},&Entry::Index);
				if (EntryIt!=It->second->end() && EntryIt->Index==Index) return EntryIt->Bits;
			}
		}
		return Spec.Default;
	}
	std::size_t Field::GetSnapshotBytes() const noexcept {
		// Conservative pin charge includes potential dense page padding, directory/index nodes and descriptor data.
		return 4096+EncodedBytes*2+Leaves.size()*160+(Dependencies?Dependencies->size()*96:0)+
			GetDefinitionCount()*256+(Definitions?Definitions->AssetBytes:0)+(State?State->MaxEncodedBytes()*8:0);
	}
	const ChannelData& Field::GetChannel(std::uint8_t Id) const {
		if (!State || Id>=State->Channels.size()) throw std::invalid_argument("[BlockGrid:Channel] Unknown channel");
		return State->Channels[Id];
	}
	std::uint8_t Field::GetChannelId(std::string_view Name) const {
		if (State) for (std::uint8_t I=0;I<State->Channels.size();++I) if (State->Channels[I].Spec.Name==Name) return I;
		throw std::invalid_argument("[BlockGrid:Channel] Unknown channel");
	}
	std::uint32_t Field::GetValue(Cell P,std::uint8_t Id) const {
		const auto& C=GetChannel(Id);
		if (!Get(P).Definition) throw std::invalid_argument("[BlockGrid:Value] State requires an occupied cell");
		return C.Read(P);
	}
	CellState Field::ReadState(Cell P) const {
		CellState Result{Get(P)};
		for (std::uint8_t I=0;I<GetChannelCount();++I) Result.Values[I]=State->Channels[I].Read(P);
		return Result;
	}
	bool Field::DefineChannel(ChannelSpec Spec) {
		ValidateChannel(Spec);
		if (State) for (const auto& C:State->Channels) if (C.Spec.Name==Spec.Name) {
			if (Spec==C.Spec) return false;
			throw std::domain_error("[BlockGrid:Conflict] Channel schema already differs");
		}
		if (GetChannelCount()>=MaximumChannels || Revision==std::numeric_limits<std::uint64_t>::max())
			throw std::length_error("[BlockGrid:Limit] Channel capacity exhausted");
		auto Next=State?std::make_shared<ChannelState>(*State):std::make_shared<ChannelState>();
		const bool Public=Spec.RelevantPeers;
		Next->Channels.push_back({std::move(Spec)});
		if (Next->MaxEncodedBytes()+EncodedBytes>MaximumEncodedBytes) throw std::length_error("[BlockGrid:Limit] Dataset capacity exhausted");
		State=std::move(Next);
		++Revision;
		if (Public) ++PublicRevision;
		return true;
	}
	void Field::UndoChannelInsertion(const ChannelSpec& Expected) {
		if (!State || State->Channels.back().Spec!=Expected || State->Channels.back().Count)
			throw std::domain_error("[BlockGrid:Conflict] Channel is in use or schema history has diverged");
		if (Revision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("[BlockGrid:Revision] Revision exhausted");
		std::shared_ptr<ChannelState> Next;
		if (State->Channels.size()>1) { Next=std::make_shared<ChannelState>(*State); Next->Channels.pop_back(); }
		State=std::move(Next);
		++Revision;
		if (Expected.RelevantPeers) ++PublicRevision;
	}
	std::vector<StateChange> Field::PlanOperations(std::span<const Operation> Commands,std::size_t CommandLimit) const {
		if (Commands.size()>CommandLimit) throw std::length_error("[BlockGrid:Limit] Edit command capacity exceeded");
		std::map<Cell,CellState,CellOrder> Staged;
		const auto Read=[&](Cell P)->CellState& {
			if (auto It=Staged.find(P);It!=Staged.end()) return It->second;
			return Staged.emplace(P,ReadState(P)).first->second;
		};
		CellState AirState;
		for (std::uint8_t I=0;I<GetChannelCount();++I) AirState.Values[I]=GetChannel(I).Spec.Default;
		const auto Place=[&](Cell P,Block Target) {
			if (!IsValid(Target)) throw std::invalid_argument("[BlockGrid:Definition] Invalid definition or orientation");
			auto& Prior=Read(P);
			if (Prior.Value.Definition!=Target.Definition) Prior.Values=AirState.Values;
			Prior.Value=Target;
		};
		std::size_t Work=MaximumEditVisits;
		for (const auto& Cmd:Commands) {
			if (const auto* W=std::get_if<Write>(&Cmd)) {
				Work-=Volume(W->Bounds,Work);
				Each(W->Bounds,[&](Cell P){Place(P,W->Value);});
			} else if (const auto* V=std::get_if<ValueWrite>(&Cmd)) {
				if (!Work) throw std::length_error("[BlockGrid:Limit] Edit cell visit capacity exceeded");
				--Work;
				(void)ScalarValue(GetChannel(V->Channel).Spec.Kind,V->Bits);
				auto& Prior=Read(V->Position);
				if (!Prior.Value.Definition) throw std::invalid_argument("[BlockGrid:Value] State requires a staged occupant");
				Prior.Values[V->Channel]=V->Bits;
			} else if (const auto* T=std::get_if<Transfer>(&Cmd)) {
				const auto Cells=Volume(T->Source,Work/2);
				if (!ValidCell(T->Destination)) throw std::invalid_argument("[BlockGrid:Bounds] Invalid transfer destination");
				const Cell Shift{T->Destination.X-T->Source.Min.X,T->Destination.Y-T->Source.Min.Y,T->Destination.Z-T->Source.Min.Z};
				const auto Shifted=[&](Cell P){return Cell{P.X+Shift.X,P.Y+Shift.Y,P.Z+Shift.Z};};
				(void)Volume({T->Destination,Shifted(T->Source.MaxExclusive)},Work/2);
				Work-=2*Cells;
				if (T->Move && Shift==Cell{}) continue;
				std::vector<CellState> Source;
				Source.reserve(Cells);
				Each(T->Source,[&](Cell P){ Source.push_back(Read(P)); if (T->Move) Read(P)=AirState; });
				std::size_t Index=0;
				Each(T->Source,[&](Cell P) {
					auto Value=Source[Index++];
					if (!T->Move) for (std::uint8_t I=0;I<GetChannelCount();++I)
						if (!GetChannel(I).Spec.CopyValue) Value.Values[I]=AirState.Values[I];
					Read(Shifted(P))=Value;
				});
			} else if (const auto* Packed=std::get_if<PackedWrite>(&Cmd)) {
				const auto Cells=Volume(Packed->Destination,Work);
				if (Packed->Values.size()!=Cells) throw std::invalid_argument("[BlockGrid:Packed] Incorrect packed cell count");
				Work-=Cells;
				std::size_t I=0;
				Each(Packed->Destination,[&](Cell P){Place(P,Packed->Values[I++]);});
			} else {
				const auto& PackedState=std::get<PackedStateWrite>(Cmd);
				const auto Cells=Volume(PackedState.Destination,Work);
				if (PackedState.Values.size()!=Cells) throw std::invalid_argument("[BlockGrid:Packed] Incorrect packed state count");
				Work-=Cells;
				std::size_t I=0;
				Each(PackedState.Destination,[&](Cell P) {
					const auto& Value=PackedState.Values[I++];
					if (!IsValid(Value.Value) || (!Value.Value.Definition && Value.Values!=AirState.Values))
						throw std::invalid_argument("[BlockGrid:Packed] Invalid packed cell state");
					for (std::uint8_t Id=0;Id<GetChannelCount();++Id) (void)ScalarValue(GetChannel(Id).Spec.Kind,Value.Values[Id]);
					for (auto Id=GetChannelCount();Id<MaximumChannels;++Id) if (Value.Values[Id])
						throw std::invalid_argument("[BlockGrid:Packed] State has an undeclared channel");
					Read(P)=Value;
				});
			}
		}
		std::vector<StateChange> Result;
		for (const auto& [P,V]:Staged) {
			auto Old=ReadState(P);
			if (Old!=V) Result.push_back({P,Old,V});
		}
		return Result;
	}
	bool Field::ApplyOperations(std::span<const Operation> Commands,const Admission& BeforeCommit) {
		if (!GetOverrideCount() && std::ranges::all_of(Commands,[](const auto& Op){return std::holds_alternative<Write>(Op);})) {
			if (Commands.size()>MaximumCommands) throw std::length_error("[BlockGrid:Limit] Edit command capacity exceeded");
			std::vector<Write> Writes;
			Writes.reserve(Commands.size());
			for (const auto& Op:Commands) Writes.push_back(std::get<Write>(Op));
			return ApplyChecked(Writes,BeforeCommit,State?State->MaxEncodedBytes():0);
		}
		return ApplyChanges(PlanOperations(Commands),BeforeCommit);
	}
	bool Field::ApplyChanges(std::span<const StateChange> Changes,const Admission& BeforeCommit) {
		if (Changes.empty()) return false;
		if (Changes.size()>MaximumEditVisits || Revision==std::numeric_limits<std::uint64_t>::max())
			throw std::length_error("[BlockGrid:Limit] State patch capacity exceeded");
		using Pages=std::map<Cell,ChannelData::Page,CellOrder>;
		std::array<std::unique_ptr<Pages>,MaximumChannels> ChangedPages;
		std::vector<Write> Writes;
		std::optional<Cell> Previous;
		bool StateChanged=false,PublicChanged=false;
		for (const auto& D:Changes) {
			if ((Previous && !CellOrder{}(*Previous,D.Position)) || !IsValid(D.After.Value))
				throw std::invalid_argument("[BlockGrid:Edit] Invalid semantic patch");
			if (ReadState(D.Position)!=D.Before) throw std::domain_error("[BlockGrid:Conflict] Cell or state has diverged");
			Previous=D.Position;
			if (D.Before.Value!=D.After.Value) Writes.push_back({SingleCell(D.Position),D.After.Value});
			for (std::uint8_t Id=0;Id<GetChannelCount();++Id) {
				const auto& Channel=GetChannel(Id); const auto Bits=D.After.Values[Id];
				if (!D.After.Value.Definition && Bits!=Channel.Spec.Default)
					throw std::invalid_argument("[BlockGrid:Value] Air cannot retain overrides");
				if (D.Before.Values[Id]==Bits) continue;
				(void)ScalarValue(Channel.Spec.Kind,Bits);
				if (!ChangedPages[Id]) ChangedPages[Id]=std::make_unique<Pages>();
				const auto Base=LeafOrigin(D.Position);
				auto [At,Added]=ChangedPages[Id]->try_emplace(Base);
				if (Added && Channel.Pages)
					if (auto It=Channel.Pages->find(Base);It!=Channel.Pages->end()) At->second=*It->second;
				auto& Page=At->second;
				const auto Index=static_cast<std::uint16_t>(LeafIndex(D.Position));
				auto E=std::ranges::lower_bound(Page,Index,{},&ChannelData::Entry::Index);
				if (E!=Page.end() && E->Index==Index) {
					if (Bits==Channel.Spec.Default) Page.erase(E); else E->Bits=Bits;
				} else if (Bits!=Channel.Spec.Default) Page.insert(E,{Index,Bits});
				StateChanged=true;
				PublicChanged|=Channel.Spec.RelevantPeers;
			}
		}
		std::shared_ptr<ChannelState> Next;
		if (StateChanged) {
			Next=std::make_shared<ChannelState>(*State);
			for (std::uint8_t Id=0;Id<GetChannelCount();++Id) if (ChangedPages[Id]) {
				auto& Channel=Next->Channels[Id];
				auto Directory=Channel.Pages?std::make_shared<ChannelData::Directory>(*Channel.Pages):std::make_shared<ChannelData::Directory>();
				Next->Overrides-=Channel.Count;
				for (auto& [Base,Page]:*ChangedPages[Id]) {
					if (auto Old=Directory->find(Base);Old!=Directory->end()) Channel.Count-=Old->second->size();
					Channel.Count+=Page.size();
					if (Page.empty()) Directory->erase(Base);
					else (*Directory)[Base]=std::make_shared<const ChannelData::Page>(std::move(Page));
				}
				Channel.Pages=Directory->empty()?nullptr:std::move(Directory);
				Next->Overrides+=Channel.Count;
			}
			if (Next->Overrides>MaximumOverrides) throw std::length_error("[BlockGrid:Limit] State override capacity exhausted");
		}
		const auto StateBytes=Next?Next->MaxEncodedBytes():State?State->MaxEncodedBytes():0;
		if (Writes.empty() && StateBytes+EncodedBytes>MaximumEncodedBytes) throw std::length_error("[BlockGrid:Limit] Dataset capacity exhausted");
		const bool GeometryChanged=!Writes.empty() && ApplyChecked(Writes,BeforeCommit,StateBytes);
		if (Next) {
			State=std::move(Next);
			if (!GeometryChanged) { ++Revision; if (PublicChanged) ++PublicRevision; }
		}
		return GeometryChanged || StateChanged;
	}
}
