// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Assets.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		constexpr std::uint8_t Magic[]{'B','G','R','D'};
		void Put(std::vector<std::uint8_t>& Bytes, std::uint32_t Value, unsigned Width) {
			for (unsigned I=0; I<Width; ++I) Bytes.push_back(static_cast<std::uint8_t>(Value>>(8*I)));
		}
		void PutBlock(std::vector<std::uint8_t>& Bytes, Block B) {
			Put(Bytes, B.Definition, 2);
			Bytes.push_back(B.Orientation);
		}
		[[noreturn]] void Invalid() { throw std::invalid_argument("[BlockGrid:Codec] Invalid or unsupported dataset"); }
		struct Reader {
			std::span<const std::uint8_t> Bytes;
			std::size_t Offset{};
			std::uint16_t MaximumId{Brick};
			std::uint32_t Take(unsigned Width) {
				if (Bytes.size()-Offset < Width) Invalid();
				std::uint32_t Result = 0;
				for (unsigned I=0; I<Width; ++I) Result |= static_cast<std::uint32_t>(Bytes[Offset++]) << (8*I);
				return Result;
			}
			Block TakeBlock() {
				const Block Value{static_cast<std::uint16_t>(Take(2)), static_cast<std::uint8_t>(Take(1))};
				if (Value.Definition>MaximumId || !ValidBlock(Value)) Invalid();
				return Value;
			}
			std::string Text(std::size_t Length) {
				if (Length>Bytes.size()-Offset) Invalid();
				std::string Result(reinterpret_cast<const char*>(Bytes.data()+Offset),Length);
				Offset+=Length;
				return Result;
			}
		};
	}
	std::vector<std::uint8_t> EncodeBase(const Field& F) {
		std::vector<std::uint8_t> Out(std::begin(Magic),std::end(Magic));
		Out.reserve(F.EncodedBytes);
		Out.push_back(F.HasAssetBindings()?4:F.Definitions?2:1);
		if (F.Definitions) {
			Put(Out,static_cast<std::uint32_t>(F.Definitions->Entries.size()),2);
			for (const auto& D:F.Definitions->Entries) {
				Put(Out,static_cast<std::uint32_t>(D.Key.size()),1);
				Out.insert(Out.end(),D.Key.begin(),D.Key.end());
				Put(Out,static_cast<unsigned>(D.Geometry),1);
				for (const auto C:D.Color) Put(Out,std::bit_cast<std::uint32_t>(C),4);
				Put(Out,unsigned(D.CanCollide)|(unsigned(D.Queryable)<<1)|(unsigned(bool(D.Assets))<<2),1);
				Put(Out,D.Ports,1);
				if (const auto& A=D.Assets) {
					Put(Out,unsigned(!A->Material.empty())|(unsigned(!A->Mesh.empty())<<1)|(unsigned(!A->Collision.empty())<<2),1);
					for (const auto* S:{&A->Material,&A->Mesh,&A->Collision}) Out.insert(Out.end(),S->begin(),S->end());
					Put(Out,static_cast<unsigned>(A->Pins.size()),1);
					for (const auto& Pin:A->Pins) {
						Put(Out,Pin.Kind,1); Out.insert(Out.end(),Pin.Id.begin(),Pin.Id.end());
						Out.insert(Out.end(),Pin.Digest.begin(),Pin.Digest.end());
					}
				}
			}
		}
		Put(Out, static_cast<std::uint32_t>(F.Leaves.size()), 4);
		for (const auto& [Base, Page] : F.Leaves) {
			for (auto N : {Base.X,Base.Y,Base.Z}) Put(Out,std::bit_cast<std::uint32_t>(N),4);
			if (auto Uniform = Page->GetUniform()) {
				Out.push_back(0);
				PutBlock(Out,*Uniform);
			} else if (const auto* Entries = std::get_if<Leaf::Sparse>(&Page->Storage)) {
				Out.push_back(1);
				Put(Out, static_cast<std::uint32_t>(Entries->size()), 2);
				for (const auto& E : *Entries) { Put(Out,E.Index,2); PutBlock(Out,E.Value); }
			} else {
				Out.push_back(2);
				for (auto B : std::get<Leaf::Dense>(Page->Storage)) PutBlock(Out,B);
			}
			if (Out.size() > MaximumEncodedBytes) throw std::length_error("[BlockGrid:Codec] Grid exceeds encoded size limit");
		}
		return Out;
	}
	Field DecodeBase(std::span<const std::uint8_t> Bytes) {
		if (Bytes.size() > MaximumEncodedBytes || Bytes.size() < sizeof(Magic)+5 ||
			!std::equal(std::begin(Magic),std::end(Magic),Bytes.begin())) Invalid();
		Reader In{Bytes, sizeof(Magic)};
		Field Result;
		const auto Version=In.Take(1);
		if (Version==2 || Version==4) {
			const auto N=In.Take(2);
			if (!N || N>MaximumDefinitions || N>(Bytes.size()-In.Offset)/25) Invalid();
			auto Bindings=std::make_shared<Field::Catalog>();
			Bindings->Entries.reserve(N);
			for (unsigned I=0;I<N;++I) {
				const auto Length=In.Take(1);
				if (Length>56 || Length>Bytes.size()-In.Offset) Invalid();
				Definition D; D.Key.assign(reinterpret_cast<const char*>(Bytes.data()+In.Offset),Length); In.Offset+=Length;
				D.Geometry=static_cast<Shape>(In.Take(1));
				for (auto& C:D.Color) C=std::bit_cast<float>(In.Take(4));
				const auto Flags=In.Take(1);
				if (Flags>(Version==4?7u:3u)) Invalid();
				D.CanCollide=(Flags&1)!=0; D.Queryable=(Flags&2)!=0;
				D.Ports=static_cast<std::uint8_t>(In.Take(1));
				if (Flags&4) {
					const auto Roles=In.Take(1);
					if (!Roles || Roles>7) Invalid();
					auto A=std::make_shared<AssetBinding>();
					if (Roles&1) A->Material=In.Text(32);
					if (Roles&2) A->Mesh=In.Text(32);
					if (Roles&4) A->Collision=In.Text(32);
					const auto Count=In.Take(1);
					if (!Count || Count>MaximumAssetPins || Count>MaximumCatalogPins-Bindings->PinCount ||
						Count>(Bytes.size()-In.Offset)/97) Invalid();
					A->Pins.reserve(Count);
					for (unsigned J=0;J<Count;++J) {
						const auto Kind=static_cast<std::uint8_t>(In.Take(1));
						A->Pins.push_back({In.Text(32),In.Text(64),Kind});
					}
					++Bindings->AssetCount; Bindings->PinCount+=Count;
					D.Assets=std::move(A);
				}
				ValidateDefinition(D);
				Bindings->AssetBytes+=AssetPinCharge(D);
				const auto Id=static_cast<std::uint16_t>(Brick+1+I);
				if (!Bindings->Names.emplace(D.Key,Id).second) Invalid();
				Bindings->Entries.push_back(std::move(D));
			}
			if (Version==4 && !Bindings->AssetCount) Invalid();
			Result.Definitions=std::move(Bindings);
			In.MaximumId=static_cast<std::uint16_t>(Brick+N);
		} else if (Version!=1) Invalid();
		const auto Count = In.Take(4);
		if (Count > MaximumLeaves || Count > (Bytes.size()-In.Offset)/16) Invalid();
		std::optional<Cell> Previous;
		for (unsigned L=0; L<Count; ++L) {
			const Cell Base{std::bit_cast<std::int32_t>(In.Take(4)), std::bit_cast<std::int32_t>(In.Take(4)),
				std::bit_cast<std::int32_t>(In.Take(4))};
			if (!ValidCell(Base) || Base.X%static_cast<int>(LeafSide) || Base.Y%static_cast<int>(LeafSide) ||
				Base.Z%static_cast<int>(LeafSide) || (Previous && !CellOrder{}(*Previous,Base))) Invalid();
			Previous = Base;
			Leaf Page;
			switch (In.Take(1)) {
			case 0: {
				const auto B = In.TakeBlock();
				if (B.Definition == Air) Invalid();
				Page.Fill(B);
				break;
			}
			case 1: {
				const auto N = In.Take(2);
				if (!N || N>Leaf::SparseLimit || N>(Bytes.size()-In.Offset)/5) Invalid();
				auto& Entries = Page.Storage.emplace<Leaf::Sparse>();
				Entries.reserve(N);
				for (unsigned J=0; J<N; ++J) {
					const auto Slot = In.Take(2);
					const auto B = In.TakeBlock();
					if (Slot>=LeafVolume || B.Definition==Air || (!Entries.empty() && Entries.back().Index>=Slot)) Invalid();
					Entries.push_back({static_cast<std::uint16_t>(Slot), B});
					Page.CustomCells+=static_cast<unsigned>(B.Definition>Brick);
				}
				Page.Occupied = N;
				break;
			}
			case 2: {
				if (Bytes.size()-In.Offset < 3 * LeafVolume) Invalid();
				auto& Values = Page.Storage.emplace<Leaf::Dense>();
				Values.reserve(LeafVolume);
				for (unsigned J=0; J<LeafVolume; ++J) {
					const auto B = In.TakeBlock();
					Values.push_back(B);
					Page.Occupied += static_cast<unsigned>(B.Definition != Air);
					Page.CustomCells += static_cast<unsigned>(B.Definition>Brick);
				}
				if (Page.Occupied <= Leaf::SparseLimit ||
					std::ranges::all_of(Values,[&](Block B){return B==Values.front();})) Invalid();
				break;
			}
			default: Invalid();
			}
			Result.Occupied += Page.Occupied;
			if (Result.Occupied > MaximumOccupiedCells) Invalid();
			if (Page.CustomCells) {
				std::vector<std::uint16_t> Ids;
				Page.CollectDefinitionIds(Ids);
				if (!Result.Dependencies) Result.Dependencies=std::make_shared<Field::Links>();
				if (Ids.size()>MaximumDefinitionLeafLinks-Result.Dependencies->size()) Invalid();
				for (auto Id:Ids) Result.Dependencies->insert({Id,Base});
			}
			Result.Leaves.emplace_hint(Result.Leaves.end(), Base, std::make_shared<const Leaf>(std::move(Page)));
		}
		if (In.Offset != Bytes.size()) Invalid();
		Result.EncodedBytes = Bytes.size();
		return Result;
	}
	std::vector<std::uint8_t> Encode(const Field& F,DataProfile Profile) {
		if (Profile>DataProfile::RelevantPeers) throw std::invalid_argument("[BlockGrid:Codec] Invalid profile");
		auto Base=EncodeBase(F);
		if (!F.State) return Base;
		const auto Eligible=[&](const ChannelData& C) {
			if (Profile==DataProfile::Clone) return true;
			if (Profile==DataProfile::Authored) return C.Spec.Persistent;
			return C.Spec.RelevantPeers && (Profile==DataProfile::RelevantPeers || C.Spec.Persistent);
		};
		const auto Count=std::ranges::count_if(F.State->Channels,Eligible);
		if (!Count) return Base;
		std::vector<std::uint8_t> Out{'B','G','R','D',3};
		Put(Out,static_cast<std::uint32_t>(Base.size()),4);
		Out.insert(Out.end(),Base.begin(),Base.end());
		Put(Out,static_cast<std::uint32_t>(Count),1);
		for (const auto& C:F.State->Channels) {
			if (!Eligible(C)) continue;
			const auto& S=C.Spec;
			Out.push_back(static_cast<std::uint8_t>(S.Name.size()));
			Out.insert(Out.end(),S.Name.begin(),S.Name.end());
			Put(Out,static_cast<unsigned>(S.Kind),1);
			Put(Out,S.Default,4);
			Put(Out,unsigned(S.Persistent)|(unsigned(S.RelevantPeers)<<1)|(unsigned(S.Indexed)<<2)|(unsigned(S.CopyValue)<<3),1);
			const bool IncludeValues=Profile!=DataProfile::Clone || S.CopyValue;
			Put(Out,IncludeValues?static_cast<std::uint32_t>(C.Count):0,4);
			if (IncludeValues && C.Pages) for (const auto& [Origin,Page]:*C.Pages) for (const auto& E:*Page) {
				const auto P=OffsetCell(Origin,E.Index);
				for (auto U:{P.X,P.Y,P.Z}) Put(Out,std::bit_cast<std::uint32_t>(U),4);
				Put(Out,E.Bits,4);
			}
		}
		if (Out.size()>MaximumEncodedBytes) throw std::length_error("[BlockGrid:Codec] Dataset exceeds byte budget");
		return Out;
	}
	std::vector<std::uint8_t> Encode(const Field& F) { return Encode(F,DataProfile::Authored); }
	Field Decode(std::span<const std::uint8_t> Bytes) {
		if (Bytes.size()<5 || Bytes[4]!=3) return DecodeBase(Bytes);
		if (Bytes.size()>MaximumEncodedBytes || !std::equal(std::begin(Magic),std::end(Magic),Bytes.begin())) Invalid();
		Reader In{Bytes,5};
		const auto Length=In.Take(4);
		if (Length>Bytes.size()-In.Offset) Invalid();
		auto F=DecodeBase(Bytes.subspan(In.Offset,Length)); In.Offset+=Length;
		const auto Count=In.Take(1);
		if (Count==0 || Count>MaximumChannels) Invalid();
		auto State=std::make_shared<ChannelState>();
		std::set<std::string> Names;
		for (unsigned I=0;I<Count;++I) {
			ChannelSpec S;
			const auto N=In.Take(1);
			if (N>48 || N>Bytes.size()-In.Offset) Invalid();
			S.Name.assign(reinterpret_cast<const char*>(Bytes.data()+In.Offset),N);In.Offset+=N;
			S.Kind=static_cast<ChannelKind>(In.Take(1));S.Default=In.Take(4);
			const auto Flags=In.Take(1);
			if (Flags>15) Invalid();
			S.Persistent=(Flags&1)!=0; S.RelevantPeers=(Flags&2)!=0; S.Indexed=(Flags&4)!=0; S.CopyValue=(Flags&8)!=0;
			ValidateChannel(S);
			if (!Names.insert(S.Name).second) Invalid();
			ChannelData C{std::move(S)};
			C.Count=In.Take(4);
			if (C.Count>MaximumOverrides-State->Overrides || C.Count>(Bytes.size()-In.Offset)/16) Invalid();
			State->Overrides+=C.Count;
			if (C.Count) {
				auto Pages=std::make_shared<ChannelData::Directory>();
				std::shared_ptr<ChannelData::Page> Page;
				std::optional<Cell> Last;
				for (unsigned J=0;J<C.Count;++J) {
					const Cell P{std::bit_cast<std::int32_t>(In.Take(4)),std::bit_cast<std::int32_t>(In.Take(4)),
						std::bit_cast<std::int32_t>(In.Take(4))};
					const auto Bits=In.Take(4);
					if (!ValidCell(P) || !F.Get(P).Definition || Bits==C.Spec.Default) Invalid();
					(void)ScalarValue(C.Spec.Kind,Bits);
					const auto Origin=LeafOrigin(P);
					if (!Last || *Last!=Origin) {
						if (Last && !CellOrder{}(*Last,Origin)) Invalid();
						Page=std::make_shared<ChannelData::Page>();
						Pages->emplace_hint(Pages->end(),Origin,Page);
						Last=Origin;
					}
					const auto Index=static_cast<std::uint16_t>(LeafIndex(P));
					if (!Page->empty() && Page->back().Index>=Index) Invalid();
					Page->push_back({Index,Bits});
				}
				C.Pages=std::move(Pages);
			}
			State->Channels.push_back(std::move(C));
		}
		if (In.Offset!=Bytes.size() || State->MaxEncodedBytes()+F.EncodedBytes>MaximumEncodedBytes) Invalid();
		F.State=std::move(State);
		return F;
	}
}
