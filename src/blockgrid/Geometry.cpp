// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		constexpr std::array<Point,6> Axes{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
		constexpr auto Rotations=[] {
			std::array<FacePair,24> Result{};
			Result[0]={2,5};
			unsigned N=1;
			for (unsigned U=0;U<6;++U) for (unsigned F=0;F<6;++F)
				if (U/2!=F/2 && FacePair{U,F}!=Result[0]) Result[N++]={U,F};
			return Result;
		}();
		constexpr Point Cross(Point A,Point B) {
			return {A[1]*B[2]-A[2]*B[1],A[2]*B[0]-A[0]*B[2],A[0]*B[1]-A[1]*B[0]};
		}
		constexpr auto Bases=[] {
			std::array<RotationBasis,24> Result{};
			for (unsigned I=0;I<Result.size();++I) {
				const auto Up=Axes[Rotations[I][0]], Front=Axes[Rotations[I][1]];
				Result[I]={Cross(Front,Up),Up,Axes[Rotations[I][1]^1]};
			}
			return Result;
		}();
		Point Transform(Point P,const RotationBasis& B,float Pivot) {
			Point R{};
			for (unsigned I=0;I<3;++I) {
				R[I]=Pivot;
				for (unsigned A=0;A<3;++A) R[I]+=(P[A]-Pivot)*B[A][I];
			}
			return R;
		}
		struct Entry {
			std::array<Face,10> Faces;
			std::array<Piece,2> Pieces;
			unsigned FaceCount{}, PieceCount{};
			Geometry View;
			void Emit(std::span<const Point> Corners,std::uint8_t Orientation) {
				auto& F=Faces.at(FaceCount++);
				F.Count=static_cast<unsigned>(Corners.size());
				for (unsigned I=0;I<F.Count;++I) F.Corners[I]=Transform(Corners[I],Basis(Orientation),.5f);
				Point A{},B{};
				for (unsigned I=0;I<3;++I) { A[I]=F.Corners[1][I]-F.Corners[0][I]; B[I]=F.Corners[2][I]-F.Corners[0][I]; }
				F.Normal=Cross(A,B);
				const float Norm=std::sqrt(F.Normal[0]*F.Normal[0]+F.Normal[1]*F.Normal[1]+F.Normal[2]*F.Normal[2]);
				for (auto& V:F.Normal) V/=Norm;
				for (unsigned I=0;I<6;++I) {
					const auto Plane=(I%2==0)?1.0f:0.0f;
					if (F.Normal==Axes[I] && F.Corners[0][I/2]==Plane) F.Boundary=static_cast<int>(I);
				}
			}
		};
		using Section=std::array<float,2>; // y,z in positive winding as seen from +X.
		constexpr std::array<Section,4> Rect(float Y0,float Z0,float Y1,float Z1) {
			return {{{Y0,Z0},{Y1,Z0},{Y1,Z1},{Y0,Z1}}};
		}
		constexpr auto Cube=Rect(0,0,1,1), Slab=Rect(0,0,.5f,1), Upper=Rect(.5f,.5f,1,1);
		constexpr std::array<Section,3> Wedge{{{0,0},{1,1},{0,1}}};
		constexpr std::array<Section,6> Stairs{{{0,0},{.5f,0},{.5f,.5f},{1,.5f},{1,1},{0,1}}};
		struct GeometryTable {
			std::array<std::array<Entry,24>,4> Shapes{};
			GeometryTable() {
				for (unsigned S=0;S<Shapes.size();++S) for (std::uint8_t R=0;R<24;++R) {
					const auto ShapeValue=static_cast<Shape>(S);
					auto& E=Shapes[S][R];
					const std::span<const Section> Outline=ShapeValue==Shape::Cube?std::span<const Section>(Cube):
						ShapeValue==Shape::Slab?std::span<const Section>(Slab):
						ShapeValue==Shape::Wedge?std::span<const Section>(Wedge):std::span<const Section>(Stairs);
					for (std::size_t I=0;I<Outline.size();++I) {
						const auto [Y,Z]=Outline[I]; const auto [Y1,Z1]=Outline[(I+1)%Outline.size()];
						E.Emit(std::array<Point,4>{{{0,Y,Z},{0,Y1,Z1},{1,Y1,Z1},{1,Y,Z}}},R);
					}
					const auto Caps=[&](std::span<const Section> Polygon) {
						for (unsigned Side=0;Side<2;++Side) {
							std::array<Point,4> P{};
							for (std::size_t I=0;I<Polygon.size();++I) {
								const auto [Y,Z]=Polygon[Side?I:Polygon.size()-1-I];
								P[I]={static_cast<float>(Side),Y,Z};
							}
							E.Emit(std::span(P).first(Polygon.size()),R);
						}
					};
					if (ShapeValue==Shape::Stairs) { Caps(Slab); Caps(Upper); }
					else Caps(Outline);
					const auto Add=[&](PieceKind Kind,Point Center,Point Size,std::uint8_t PieceRotation) {
						E.Pieces[E.PieceCount++]={Kind,Transform(Center,Basis(R),.5f),Size,PieceRotation};
					};
					if (ShapeValue==Shape::Wedge) {
						const auto [Up,Front]=OrientationFaces(R);
						Add(PieceKind::Wedge,{.5f,.5f,.5f},{1,1,1},OrientationIndex(Up,Front^1));
					} else if (ShapeValue==Shape::Cube) Add(PieceKind::Box,{.5f,.5f,.5f},{1,1,1},R);
					else {
						Add(PieceKind::Box,{.5f,.25f,.5f},{1,.5f,1},R);
						if (ShapeValue==Shape::Stairs) Add(PieceKind::Box,{.5f,.75f,.75f},{1,.5f,.5f},R);
					}
					const auto LocalMask=ShapeValue==Shape::Cube?63u:ShapeValue==Shape::Slab?8u:24u;
					std::uint8_t Mask{};
					for (unsigned I=0;I<6;++I) if (LocalMask&(1u<<I)) Mask|=static_cast<std::uint8_t>(1u<<RotateFace(I,R));
					E.View={std::span(E.Faces).first(E.FaceCount),Mask,std::span(E.Pieces).first(E.PieceCount)};
				}
			}
		};
	}
	FacePair OrientationFaces(std::uint8_t Orientation) { return Rotations.at(Orientation); }
	std::uint8_t OrientationIndex(unsigned Up,unsigned Front) {
		const auto It=std::ranges::find(Rotations,FacePair{Up,Front});
		if (It==Rotations.end()) throw std::invalid_argument("[BlockGrid:Orientation] Up and Front must be perpendicular faces");
		return static_cast<std::uint8_t>(It-Rotations.begin());
	}
	const RotationBasis& Basis(std::uint8_t Orientation) { return Bases.at(Orientation); }
	unsigned RotateFace(unsigned LocalFace,std::uint8_t Orientation) {
		const auto Direction=Transform(Axes.at(LocalFace),Basis(Orientation),0);
		return static_cast<unsigned>(std::ranges::find(Axes,Direction)-Axes.begin());
	}
	const Geometry& GetGeometry(Shape Value,std::uint8_t Orientation) {
		static const GeometryTable Table;
		return Table.Shapes.at(static_cast<unsigned>(Value)).at(Orientation).View;
	}
}
