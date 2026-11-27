// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		using Bytes=std::vector<std::uint8_t>;
		[[noreturn]] void Invalid() { throw std::invalid_argument("[BlockGrid:Replica] Invalid public page manifest"); }
		void Put(Bytes& Out,std::uint32_t Value,unsigned Width) {
			for (unsigned I=0;I<Width;++I) Out.push_back(static_cast<std::uint8_t>(Value>>(8*I)));
		}
		void PutCell(Bytes& Out,Cell P) {
			for (auto N:{P.X,P.Y,P.Z}) Put(Out,std::bit_cast<std::uint32_t>(N),4);
		}
		struct Reader {
			std::span<const std::uint8_t> Remaining;
			auto Take(std::size_t Count) {
				if (Count>Remaining.size()) Invalid();
				auto Result=Remaining.first(Count);Remaining=Remaining.subspan(Count);return Result;
			}
			std::uint32_t Number(unsigned Width) {
				std::uint32_t Value{};
				auto Data=Take(Width);
				for (unsigned I=0;I<Width;++I) Value|=std::uint32_t(Data[I])<<(8*I);
				return Value;
			}
			Cell Position() {
				return {std::bit_cast<std::int32_t>(Number(4)),std::bit_cast<std::int32_t>(Number(4)),
					std::bit_cast<std::int32_t>(Number(4))};
			}
		};
		bool Encloses(Region Box,Region R) {
			return Box.Min.X<=R.Min.X && Box.Min.Y<=R.Min.Y && Box.Min.Z<=R.Min.Z &&
				Box.MaxExclusive.X>=R.MaxExclusive.X && Box.MaxExclusive.Y>=R.MaxExclusive.Y && Box.MaxExclusive.Z>=R.MaxExclusive.Z;
		}
		std::size_t RectanglePages(Region Box) {
			const auto A=Box.Min,B=Box.MaxExclusive;
			if (!ValidCell(A) || B.X>CoordinateLimit || B.Y>CoordinateLimit || B.Z>CoordinateLimit ||
				A.X>=B.X || A.Y>=B.Y || A.Z>=B.Z || LeafOrigin(A)!=A || LeafOrigin(B)!=B) return 0;
			return std::size_t((B.X-A.X)/LeafSide)*((B.Y-A.Y)/LeafSide)*((B.Z-A.Z)/LeafSide);
		}
		bool ValidCoverage(const ReplicaCoverage& C) {
			if (C.Origins.size()>MaximumReplicaPages || (C.Whole && (!C.Origins.empty() || C.Rectangle))) return false;
			if (C.Rectangle) {
				const auto Count=RectanglePages(*C.Rectangle);
				if (!Count || Count>MaximumReplicaRectanglePages) return false;
			}
			for (std::size_t I=0;I<C.Origins.size();++I) {
				const auto P=C.Origins[I];
				if (!ValidCell(P) || LeafOrigin(P)!=P || (C.Rectangle && Encloses(*C.Rectangle,SingleCell(P))) ||
					(I && !CellOrder{}(C.Origins[I-1],P))) return false;
			}
			return true;
		}
	}
	bool ReplicaCoverage::Contains(Region Bounds) const {
		const auto A=Bounds.Min,B=Bounds.MaxExclusive;
		if (!ValidCell(A) || B.X>CoordinateLimit || B.Y>CoordinateLimit || B.Z>CoordinateLimit ||
			A.X>=B.X || A.Y>=B.Y || A.Z>=B.Z) return false;
		const auto RectPages=Rectangle?RectanglePages(*Rectangle):0;
		if (Rectangle && (!RectPages || RectPages>MaximumReplicaRectanglePages)) return false;
		if (Whole || (Rectangle && Encloses(*Rectangle,Bounds))) return true;
		const auto First=LeafOrigin(A),Last=LeafOrigin({B.X-1,B.Y-1,B.Z-1});
		constexpr auto Side=static_cast<int>(LeafSide);
		const auto Count=std::uint64_t((Last.X-First.X)/Side+1)*((Last.Y-First.Y)/Side+1)*((Last.Z-First.Z)/Side+1);
		if (Count>Origins.size()+RectPages) return false;
		for (int Z=First.Z;Z<=Last.Z;Z+=Side) for (int Y=First.Y;Y<=Last.Y;Y+=Side)
			for (int X=First.X;X<=Last.X;X+=Side)
				if (!(Rectangle && Encloses(*Rectangle,SingleCell({X,Y,Z}))) &&
					!std::ranges::binary_search(Origins,Cell{X,Y,Z},CellOrder{})) return false;
		return true;
	}
	std::optional<ReplicaPages> Field::CapturePages(const ReplicaCoverage& Coverage,
		const std::function<bool(const Field&)>& AdmitSchema) const {
		if (!ValidCoverage(Coverage) || (Coverage.Whole && Leaves.size()>WholeReplicaPages)) Invalid();
		std::vector<Directory::const_iterator> Selected;
		if (Coverage.Whole) {
			for (auto It=Leaves.begin();It!=Leaves.end();++It) Selected.push_back(It);
		} else for (const auto P:Coverage.Origins) {
			if (const auto It=Leaves.find(P);It!=Leaves.end()) Selected.push_back(It);
		}
		if (Coverage.Rectangle) {
			const auto [A,B]=*Coverage.Rectangle;
			constexpr int Side=LeafSide;
			for (int Z=A.Z;Z<B.Z;Z+=Side) for (int Y=A.Y;Y<B.Y;Y+=Side) for (int X=A.X;X<B.X;X+=Side) {
				const auto It=Leaves.find({X,Y,Z});
				if (It==Leaves.end()) continue;
				if (Selected.size()==MaximumReplicaPages) return {};
				Selected.push_back(It);
			}
			std::ranges::sort(Selected,CellOrder{},[](auto It){ return It->first; });
		}
		std::array<std::uint16_t,MaximumDefinitions+Brick+1> IdMap{};
		IdMap[Stone]=Stone;IdMap[Brick]=Brick;
		for (const auto It:Selected) {
			std::vector<std::uint16_t> Ids;
			It->second->CollectDefinitionIds(Ids);
			for (auto Id:Ids) IdMap[Id]=1;
		}
		Field Schema;
		for (std::uint16_t Id=Brick+1;Id<Brick+1+GetDefinitionCount();++Id) if (IdMap[Id]) {
			(void)Schema.Define(GetDefinition(Id));
			IdMap[Id]=static_cast<std::uint16_t>(Brick+Schema.GetDefinitionCount());
		}
		std::vector<const ChannelData*> Public;
		if (State) for (const auto& C:State->Channels) if (C.Spec.RelevantPeers) {
			(void)Schema.DefineChannel(C.Spec);Public.push_back(&C);
		}
		if (AdmitSchema && !AdmitSchema(Schema)) return {};
		ReplicaPages Result;
		auto& Out=Result.Bytes;
		Out={'B','G','R','W',static_cast<std::uint8_t>(Coverage.Rectangle?2:1),static_cast<std::uint8_t>(Coverage.Whole)};
		if (Coverage.Rectangle) { PutCell(Out,Coverage.Rectangle->Min); PutCell(Out,Coverage.Rectangle->MaxExclusive); }
		Put(Out,static_cast<unsigned>(Coverage.Origins.size()),1);
		for (auto P:Coverage.Origins) PutCell(Out,P);
		const auto Header=Encode(Schema,DataProfile::RelevantPeers);
		Put(Out,static_cast<unsigned>(Header.size()),4);
		Out.insert(Out.end(),Header.begin(),Header.end());
		Put(Out,static_cast<unsigned>(Selected.size()),1);
		auto PutBlock=[&](Block B){ Put(Out,IdMap[B.Definition],2);Put(Out,B.Orientation,1); };
		for (const auto It:Selected) {
			const auto& [Origin,Page]=*It;
			const auto Start=static_cast<std::uint32_t>(Out.size());
			Put(Out,static_cast<unsigned>(Page->EncodedSize()),2);PutCell(Out,Origin);
			if (auto Uniform=Page->GetUniform()) {
				Out.push_back(0);PutBlock(*Uniform);
			} else if (const auto* Entries=std::get_if<Leaf::Sparse>(&Page->Storage)) {
				Out.push_back(1);Put(Out,static_cast<unsigned>(Entries->size()),2);
				for (const auto& E:*Entries) { Put(Out,E.Index,2);PutBlock(E.Value); }
			} else {
				Out.push_back(2);
				for (auto B:std::get<Leaf::Dense>(Page->Storage)) PutBlock(B);
			}
			std::uint16_t Mask{};
			for (std::size_t I=0;I<Public.size();++I)
				if (Public[I]->Pages && Public[I]->Pages->contains(Origin)) Mask|=std::uint16_t(1u<<I);
			Put(Out,Mask,2);
			for (std::size_t I=0;I<Public.size();++I) if (Mask&(1u<<I)) {
				const auto& Overrides=*Public[I]->Pages->at(Origin);
				Put(Out,static_cast<unsigned>(Overrides.size()),2);
				for (const auto& E:Overrides) { Put(Out,E.Index,2);Put(Out,E.Bits,4); }
			}
			Result.Records.push_back({Origin,Start,static_cast<std::uint32_t>(Out.size()-Start)});
		}
		return Result;
	}
	Field Field::DecodePages(std::span<const std::uint8_t> Data,ReplicaCoverage& Coverage) {
		constexpr std::array<std::uint8_t,4> Magic{'B','G','R','W'};
		Reader In{Data};
		if (!std::ranges::equal(In.Take(Magic.size()),Magic)) Invalid();
		const auto Version=In.Number(1),Whole=In.Number(1);
		if ((Version!=1 && Version!=2) || Whole>1 || (Version==2 && Whole)) Invalid();
		ReplicaCoverage Checked{Whole!=0};
		if (Version==2) Checked.Rectangle=Region{In.Position(),In.Position()};
		const auto Covered=In.Number(1);
		if (Covered>MaximumReplicaPages) Invalid();
		for (unsigned I=0;I<Covered;++I) Checked.Origins.push_back(In.Position());
		if (!ValidCoverage(Checked)) Invalid();
		const auto Header=In.Take(In.Number(4));
		const auto Schema=Decode(Header);
		if (Schema.GetLeafCount() || Schema.GetOverrideCount() ||
			!std::ranges::equal(Encode(Schema,DataProfile::RelevantPeers),Header)) Invalid();
		const auto PageCount=In.Number(1);
		if (PageCount>(Checked.Whole?WholeReplicaPages:Checked.Rectangle?MaximumReplicaPages:Covered)) Invalid();
		auto Base=EncodeBase(Schema);
		Base.resize(Base.size()-4);Put(Base,PageCount,4);
		std::array<Bytes,MaximumChannels> Overrides;
		std::size_t Total{};
		for (unsigned I=0;I<PageCount;++I) {
			const auto Geometry=In.Take(In.Number(2));
			Reader GeometryHeader{Geometry}; const auto Origin=GeometryHeader.Position();
			if (Geometry.size()>13+3*LeafVolume || !ValidCell(Origin) || LeafOrigin(Origin)!=Origin ||
				!Checked.Contains(SingleCell(Origin))) Invalid();
			Base.insert(Base.end(),Geometry.begin(),Geometry.end());
			const auto Mask=In.Number(2);
			if (Mask>>Schema.GetChannelCount()) Invalid();
			for (std::size_t J=0;J<Schema.GetChannelCount();++J) if (Mask&(1u<<J)) {
				const auto Count=In.Number(2);
				if (!Count || Count>LeafVolume || Count>MaximumOverrides-Total) Invalid();
				Total+=Count;
				int Previous=-1;
				for (unsigned K=0;K<Count;++K) {
					const int Index=static_cast<int>(In.Number(2));
					if (Index<=Previous || Index>=static_cast<int>(LeafVolume)) Invalid();
					Previous=Index;PutCell(Overrides[J],OffsetCell(Origin,Index));Put(Overrides[J],In.Number(4),4);
				}
			}
		}
		if (!In.Remaining.empty()) Invalid();
		Bytes Canonical;
		if (Schema.GetChannelCount()) {
			Canonical={'B','G','R','D',3};Put(Canonical,static_cast<unsigned>(Base.size()),4);
			Canonical.insert(Canonical.end(),Base.begin(),Base.end());Put(Canonical,static_cast<unsigned>(Schema.GetChannelCount()),1);
			for (std::size_t I=0;I<Schema.GetChannelCount();++I) {
				const auto& S=Schema.GetChannel(static_cast<std::uint8_t>(I)).Spec;
				Put(Canonical,static_cast<unsigned>(S.Name.size()),1);
				Canonical.insert(Canonical.end(),S.Name.begin(),S.Name.end());
				Put(Canonical,static_cast<unsigned>(S.Kind),1);Put(Canonical,S.Default,4);
				Put(Canonical,unsigned(S.Persistent)|(unsigned(S.RelevantPeers)<<1)|(unsigned(S.Indexed)<<2)|(unsigned(S.CopyValue)<<3),1);
				Put(Canonical,static_cast<unsigned>(Overrides[I].size()/16),4);
				Canonical.insert(Canonical.end(),Overrides[I].begin(),Overrides[I].end());
			}
		} else Canonical=std::move(Base);
		// Reuse the persisted codec's coordinate, orientation, definition, pin, state-type and occupancy validation.
		auto Result=Decode(Canonical);
		// Also enforces page record boundaries, canonical schema and exclusion of unused private dependencies.
		const auto Encoded=Result.CapturePages(Checked);
		if (!Encoded || !std::ranges::equal(Encoded->Bytes,Data)) Invalid();
		Coverage=std::move(Checked);
		return Result;
	}
}
