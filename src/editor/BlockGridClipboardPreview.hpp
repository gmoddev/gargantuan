// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "editor/BlockGridClipboard.hpp"
#include "serialization/JsonCodec.hpp"

namespace gargantuan::editor_detail {
	inline JsonCodec::Json DescribePaste(BlockGrid& Grid,const blockgrid::EditRequest& Edit) {
		using namespace blockgrid;
		using Json=JsonCodec::Json;
		const auto& Packed=std::get<PackedStateWrite>(Edit.Commands.at(0));
		const auto& Min=Packed.Destination.Min;
		const auto& Max=Packed.Destination.MaxExclusive;
		const auto& Field=Grid.GetField();
		const auto Pose=Grid.GetCFrame();
		const auto Scale=Grid.GetCellSize();
		const auto World=[&](glm::vec3 Local){return Pose.PointToWorldSpace(Local*Scale);};
		Json Outline=Json::array(), Geometry=Json::array();
		const auto Edge=[&](Json& Lines,glm::vec3 A,glm::vec3 B) {
			const auto P=World(A),Q=World(B);
			Lines.push_back({P.x,P.y,P.z,Q.x,Q.y,Q.z});
		};
		const auto Corner=[&](unsigned I){return glm::vec3(I&1?Max.X:Min.X,I&2?Max.Y:Min.Y,I&4?Max.Z:Min.Z);};
		for (unsigned I=0;I<8;++I) for (const unsigned Axis:{1u,2u,4u}) if (!(I&Axis))
			Edge(Outline,Corner(I),Corner(I|Axis));
		constexpr std::size_t LineLimit=192;
		std::size_t Index{}, Occupied{}, Replaced{};
		bool Clipped=false;
		for (int Z=Min.Z;Z<Max.Z;++Z) for (int Y=Min.Y;Y<Max.Y;++Y) for (int X=Min.X;X<Max.X;++X) {
			const auto& State=Packed.Values[Index++];
			const Cell Position{X,Y,Z};
			if (const auto Before=Field.ReadState(Position);Before.Value.Definition && Before!=State) ++Replaced;
			if (!State.Value.Definition) continue;
			++Occupied;
			if (Geometry.size()>=LineLimit) {Clipped=true;continue;}
			const auto P=glm::vec3(X,Y,Z);
			auto Line=[&](glm::vec3 A,glm::vec3 B) {
				if (Geometry.size()<LineLimit) Edge(Geometry,P+A,P+B);
				else Clipped=true;
			};
			const auto First=Brick+1+Field.GetDefinitionCount();
			const auto& Def=State.Value.Definition<First?Field.GetDefinition(State.Value.Definition):Edit.Materials.at(State.Value.Definition-First);
			if (Def.Geometry==Shape::Mesh) {
				if (!Def.Assets || !Def.Assets->Resolved || !Def.Assets->Resolved->VisualMesh)
					throw std::invalid_argument("A preview mesh is not available");
				const auto& Mesh=*Def.Assets->Resolved->VisualMesh;
				const auto& BasisValue=Basis(State.Value.Orientation);
				const auto Vertex=[&](std::uint32_t Id) {
					const auto V=Mesh.Vertices->at(Id).Position;
					glm::vec3 Result(.5f);
					for (unsigned A=0;A<3;++A) for (unsigned B=0;B<3;++B) Result[B]+=V[A]*BasisValue[A][B];
					return Result;
				};
				for (std::size_t I=0;I<Mesh.Indices->size();I+=3) {
					if (Geometry.size()>=LineLimit) {Clipped=true;break;}
					for (unsigned J=0;J<3;++J) Line(Vertex(Mesh.Indices->at(I+J)),Vertex(Mesh.Indices->at(I+(J+1)%3)));
				}
			} else for (const auto& Face:GetGeometry(Def.Geometry,State.Value.Orientation).Faces) {
				for (unsigned I=0;I<Face.Count;++I) {
					const auto& A=Face.Corners[I];const auto& B=Face.Corners[(I+1)%Face.Count];
					Line({A[0],A[1],A[2]},{B[0],B[1],B[2]});
				}
			}
		}
		return {{"Outline",std::move(Outline)},{"Geometry",std::move(Geometry)},{"Clipped",Clipped},
			{"Occupied",Occupied},{"Replaced",Replaced},{"Cells",Index},{"Size",{Max.X-Min.X,Max.Y-Min.Y,Max.Z-Min.Z}}};
	}
}
