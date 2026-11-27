// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Assets.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace gargantuan::editor_detail {
	struct GridClipboard {
		blockgrid::Cell Size;
		std::vector<blockgrid::CellState> Cells;
		std::vector<blockgrid::ChannelSpec> Channels;
		std::map<std::uint16_t,blockgrid::Definition> Definitions;

		static GridClipboard Capture(const blockgrid::Field& Source,blockgrid::Region Bounds) {
			using namespace blockgrid;
			const auto Count=Volume(Bounds,MaximumReadCells);
			GridClipboard Result;
			Result.Size={Bounds.MaxExclusive.X-Bounds.Min.X,Bounds.MaxExclusive.Y-Bounds.Min.Y,Bounds.MaxExclusive.Z-Bounds.Min.Z};
			Result.Cells.reserve(Count);
			for (std::uint8_t I=0;I<Source.GetChannelCount();++I) Result.Channels.push_back(Source.GetChannel(I).Spec);
			for (int Z=Bounds.Min.Z;Z<Bounds.MaxExclusive.Z;++Z) for (int Y=Bounds.Min.Y;Y<Bounds.MaxExclusive.Y;++Y)
				for (int X=Bounds.Min.X;X<Bounds.MaxExclusive.X;++X) {
					auto Value=Source.ReadState({X,Y,Z});
					const auto Id=Value.Value.Definition;
					if (Id>Brick && !Result.Definitions.contains(Id)) {
						auto Definition=Source.GetDefinition(Id);
						if (Definition.Assets) {
							auto Pins=std::make_shared<AssetBinding>(*Definition.Assets);
							Pins->Resolved.reset();
							Definition.Assets=std::move(Pins);
						}
						Result.Definitions.emplace(Id,std::move(Definition));
					}
					Result.Cells.push_back(std::move(Value));
				}
			return Result;
		}

		std::shared_ptr<blockgrid::EditRequest> Plan(BlockGrid& Target,blockgrid::Cell Min,unsigned Turns) const {
			using namespace blockgrid;
			if (Turns>3) throw std::invalid_argument("QuarterTurns must be 0 through 3");
			const Cell Extent=Turns%2?Cell{Size.Z,Size.Y,Size.X}:Size;
			const Region Bounds{Min,{Min.X+Extent.X,Min.Y+Extent.Y,Min.Z+Extent.Z}};
			const auto Count=Volume(Bounds,MaximumReadCells);
			const auto& Field=Target.GetField();
			auto Edit=std::make_shared<EditRequest>();
			Edit->ExpectedRevision=Target.GetBlockRevision();
			Edit->ImportDefinitions=true;
			std::map<std::uint16_t,std::uint16_t> Mapped{{Air,Air},{Stone,Stone},{Brick,Brick}};
			for (const auto& [Id,Definition]:Definitions) {
				const auto Found=Field.FindDefinitionId(Definition.Key);
				if (Found) {
					if (Field.GetDefinition(*Found)!=Definition) throw std::invalid_argument("Conflicting destination definition "+Definition.Key);
					Mapped.emplace(Id,*Found);
				} else {
					if (Edit->Materials.size()>=MaximumNewMaterials) throw std::length_error("A paste imports up to 16 new definitions");
					Mapped.emplace(Id,static_cast<std::uint16_t>(Brick+1+Field.GetDefinitionCount()+Edit->Materials.size()));
					Edit->Materials.push_back(Target.ResolveAssetDefinition(Definition));
				}
			}
			CellState Defaults{};
			std::map<std::string,std::uint8_t,std::less<>> DestinationChannels;
			for (std::uint8_t I=0;I<Field.GetChannelCount();++I) {
				const auto& Spec=Field.GetChannel(I).Spec;
				Defaults.Values[I]=Spec.Default;
				DestinationChannels.emplace(Spec.Name,I);
			}
			std::vector<std::optional<std::uint8_t>> ChannelIds(Channels.size());
			for (std::size_t I=0;I<Channels.size();++I) {
				const auto& Spec=Channels[I];
				if (!Spec.CopyValue) continue;
				auto It=DestinationChannels.find(Spec.Name);
				if (It!=DestinationChannels.end() && Field.GetChannel(It->second).Spec==Spec) ChannelIds[I]=It->second;
				else if (std::ranges::any_of(Cells,[&](const auto& C){return C.Value.Definition && C.Values[I]!=Spec.Default;}))
					throw std::invalid_argument("Define a compatible destination channel for "+Spec.Name);
			}
			// Positive Y quarter-turn: +X rotates toward -Z. Offset the resulting box back to Min.
			constexpr std::array<unsigned,4> Front{5,1,4,0};
			const auto Rotation=OrientationIndex(2,Front[Turns]);
			PackedStateWrite Packed{Bounds,std::vector<CellState>(Count,Defaults)};
			std::size_t Index{};
			for (int Z=0;Z<Size.Z;++Z) for (int Y=0;Y<Size.Y;++Y) for (int X=0;X<Size.X;++X) {
				const auto& Original=Cells.at(Index++);
				if (!Original.Value.Definition) continue;
				const std::array<int,4> DX{X,Z,Size.X-1-X,Size.Z-1-Z}, DZ{Z,Size.X-1-X,Size.Z-1-Z,X};
				auto& Next=Packed.Values[(DZ[Turns]*Extent.Y+Y)*Extent.X+DX[Turns]];
				const auto Faces=OrientationFaces(Original.Value.Orientation);
				Next.Value={Mapped.at(Original.Value.Definition),OrientationIndex(RotateFace(Faces[0],Rotation),RotateFace(Faces[1],Rotation))};
				for (std::size_t I=0;I<ChannelIds.size();++I) if (ChannelIds[I]) Next.Values[*ChannelIds[I]]=Original.Values[I];
			}
			Edit->Commands.emplace_back(std::move(Packed));
			return Edit;
		}
	};
}
