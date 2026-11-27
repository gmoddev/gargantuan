// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Geometry.hpp"
#include <box3d/collision.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace gargantuan::blockgrid;
namespace {
	Point Rotate(Point P,std::uint8_t R,float Center) {
		Point V{Center,Center,Center};
		for (int I=0;I<3;++I) for (int J=0;J<3;++J) V[I]+=Basis(R)[J][I]*(P[J]-Center);
		return V;
	}
	b3Vec3 ToLocal(Point P,Point Center,std::uint8_t R) {
		Point V{};
		for (int I=0;I<3;++I) for (int J=0;J<3;++J) V[I]+=Basis(R)[I][J]*(P[J]-Center[J]);
		return {V[0],V[1],V[2]};
	}
}
int main() try {
	const b3Vec3 WedgePoints[]={{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{-.5f,-.5f,.5f},
		{.5f,.5f,-.5f},{-.5f,.5f,-.5f}};
	const auto Wedge=b3CreateHull(WedgePoints,6,6);
	if (!Wedge) throw std::runtime_error("Box3D did not build the wedge");
	unsigned Trials=0;
	for (unsigned S=0;S<4;++S) for (std::uint8_t R=0;R<24;++R) for (float Z:{.25f,.75f}) {
		const auto Kind=static_cast<Shape>(S);
		const auto Origin=Rotate({.25f,2,Z},R,.5f), Direction=Rotate({0,-2,0},R,0);
		float Nearest=2;
		for (const auto& Piece:GetGeometry(Kind,R).Collision) {
			const auto Box=b3MakeBoxHull(Piece.Size[0]/2,Piece.Size[1]/2,Piece.Size[2]/2);
			const b3RayCastInput Ray{ToLocal(Origin,Piece.Center,Piece.Orientation),ToLocal(Direction,{},Piece.Orientation),1};
			const auto Hit=b3RayCastHull(Piece.Kind==PieceKind::Wedge?Wedge:&Box.base,&Ray);
			if (Hit.hit) Nearest=std::min(Nearest,Hit.fraction);
		}
		const float Top=Kind==Shape::Cube?1.f:Kind==Shape::Wedge?Z:Kind==Shape::Stairs && Z>.5f?1.f:.5f;
		if (std::abs(Nearest-(2-Top)/2)>1e-5f) throw std::runtime_error("Canonical height disagrees with Box3D contact");
		++Trials;
	}
	b3DestroyHull(Wedge);
	std::cout<<"[BlockGrid:CollisionTest] "<<Trials<<" oriented Box3D primitive ray contacts passed\n";
} catch (const std::exception& E) { std::cerr<<E.what()<<'\n'; return 1; }
