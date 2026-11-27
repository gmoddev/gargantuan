// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"

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
		};
	}
	std::vector<std::uint8_t> Encode(const Field& F) {
		std::vector<std::uint8_t> Out(std::begin(Magic),std::end(Magic));
		Out.reserve(F.EncodedBytes);
		Out.push_back(F.Definitions?2:1);
		if (F.Definitions) {
			Put(Out,static_cast<std::uint32_t>(F.Definitions->Entries.size()),2);
			for (const auto& D:F.Definitions->Entries) {
				Put(Out,static_cast<std::uint32_t>(D.Key.size()),1);
				Out.insert(Out.end(),D.Key.begin(),D.Key.end());
				Put(Out,static_cast<unsigned>(D.Geometry),1);
				for (const auto C:D.Color) Put(Out,std::bit_cast<std::uint32_t>(C),4);
				Put(Out,unsigned(D.CanCollide)|(unsigned(D.Queryable)<<1),1);
				Put(Out,D.Ports,1);
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
	Field Decode(std::span<const std::uint8_t> Bytes) {
		if (Bytes.size() > MaximumEncodedBytes || Bytes.size() < sizeof(Magic)+5 ||
			!std::equal(std::begin(Magic),std::end(Magic),Bytes.begin())) Invalid();
		Reader In{Bytes, sizeof(Magic)};
		Field Result;
		const auto Version=In.Take(1);
		if (Version==2) {
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
				if (Flags>3) Invalid();
				D.CanCollide=(Flags&1)!=0; D.Queryable=(Flags&2)!=0;
				D.Ports=static_cast<std::uint8_t>(In.Take(1));
				ValidateDefinition(D);
				const auto Id=static_cast<std::uint16_t>(Brick+1+I);
				if (!Bindings->Names.emplace(D.Key,Id).second) Invalid();
				Bindings->Entries.push_back(std::move(D));
			}
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
}
