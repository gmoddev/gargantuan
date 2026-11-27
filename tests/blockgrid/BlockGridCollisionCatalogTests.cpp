// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Geometry.hpp"
#include <box3d/box3d.h>
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
	void TestMovingHulls() {
		auto Configuration=b3DefaultWorldDef();
		Configuration.gravity={0,-10,0};
		const auto World=b3CreateWorld(&Configuration);
		struct Cleanup { b3WorldId World; ~Cleanup(){b3DestroyWorld(World);} } Guard{World};
		auto GroundDef=b3DefaultBodyDef(); GroundDef.position={0,-.5,0};
		const auto Ground=b3CreateBody(World,&GroundDef);
		auto Shape=b3DefaultShapeDef(); Shape.enableContactEvents=true; Shape.density=2.f;
		const auto Floor=b3MakeBoxHull(16,.5f,16);
		(void)b3CreateHullShape(Ground,&Shape,&Floor.base);
		auto Movement=b3DefaultBodyDef(); Movement.type=b3_dynamicBody; Movement.position={0,4,0};
		const auto Body=b3CreateBody(World,&Movement);
		const auto Cube=b3MakeBoxHull(.5f,.5f,.5f);
		auto Local=b3Transform_identity;
		const auto A=b3CreateTransformedHullShape(Body,&Shape,&Cube.base,Local,{1,1,1});
		Local.p.x=2.f;
		const auto B=b3CreateTransformedHullShape(Body,&Shape,&Cube.base,Local,{1,1,1});
		b3Body_ApplyMassFromShapes(Body);
		const auto Mass=b3Body_GetMassData(Body);
		if (!b3Shape_IsValid(A) || !b3Shape_IsValid(B) || b3Body_GetShapeCount(Body)!=2 ||
			std::abs(Mass.mass-4)>1e-4f || std::abs(Mass.center.x-1)>1e-4f)
			throw std::runtime_error("Two transformed hulls failed to produce the expected dynamic mass and center");
		b3Body_SetLinearVelocity(Body,{1,0,0});
		int Contacts=0;
		for (unsigned I=0;I<180;++I) {
			b3World_Step(World,1.f/60,4);
			Contacts+=b3World_GetContactEvents(World).beginCount;
		}
		const auto Rest=b3Body_GetPosition(Body);
		if (!Contacts || Rest.x<=.2 || Rest.y<.43 || Rest.y>.60) throw std::runtime_error("Moving hulls did not land together");
		b3DestroyShape(B,true);
		const auto Reduced=b3Body_GetMassData(Body);
		if (!b3Body_IsValid(Body) || !b3Shape_IsValid(A) || b3Body_GetShapeCount(Body)!=1 ||
			std::abs(Reduced.mass-2)>1e-4f || std::abs(Reduced.center.x)>1e-4f)
			throw std::runtime_error("Removing a hull failed to update mass without changing the body identity");
		std::cout<<"[BlockGrid:MovingHulls] mass="<<Mass.mass<<" centerX="<<Mass.center.x<<" contacts="<<Contacts
			<<" restY="<<Rest.y<<" afterRemoveMass="<<Reduced.mass<<'\n';
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
	TestMovingHulls();
	std::cout<<"[BlockGrid:CollisionTest] "<<Trials<<" oriented Box3D primitive ray contacts passed\n";
} catch (const std::exception& E) { std::cerr<<E.what()<<'\n'; return 1; }
