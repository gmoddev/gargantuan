// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Picking.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace gargantuan::blockgrid {
	namespace {
		using Vec = std::array<double,3>;
		constexpr auto Inf=std::numeric_limits<double>::infinity();
		bool Tied(double A,double B) noexcept {
			return A==B || (std::isfinite(A) && std::isfinite(B) &&
				std::abs(A-B) <= 8*std::numeric_limits<double>::epsilon()*std::max(std::abs(A),std::abs(B)));
		}
		struct Interval { double Lo{-Inf}, Hi{Inf}; Vec Entry{}, Exit{}; };
		std::optional<Interval> Clip(Vec O, Vec D, Vec Min, Vec Max) {
			Interval Span;
			for (unsigned Axis=0; Axis<3; ++Axis) {
				if (D[Axis]==0) {
					if (O[Axis]<=Min[Axis] || O[Axis]>=Max[Axis]) return {};
					continue;
				}
				auto A=(Min[Axis]-O[Axis])/D[Axis], B=(Max[Axis]-O[Axis])/D[Axis];
				const double Sign=D[Axis]>0 ? -1.0 : 1.0;
				if (A>B) std::swap(A,B);
				if (A>Span.Lo && !Tied(A,Span.Lo)) { Span.Lo=A; Span.Entry={}; Span.Entry[Axis]=Sign; }
				if (B<Span.Hi && !Tied(B,Span.Hi)) { Span.Hi=B; Span.Exit={}; Span.Exit[Axis]=-Sign; }
			}
			if (Span.Hi<=Span.Lo || Tied(Span.Lo,Span.Hi)) return {};
			return Span;
		}
		bool CellPrecedes(Cell A,Cell B) { return std::tie(A.X,A.Y,A.Z)<std::tie(B.X,B.Y,B.Z); }
		std::optional<CellHit> HitShape(Cell P,const Definition& Spec,Block B,Vec Origin,Vec Direction) {
			if (!Spec.Queryable) return {};
			if (Spec.Geometry==Shape::Cube) {
				const Vec Min{static_cast<double>(P.X),static_cast<double>(P.Y),static_cast<double>(P.Z)};
				const Vec Max{Min[0]+1,Min[1]+1,Min[2]+1};
				const auto S=Clip(Origin,Direction,Min,Max);
				if (!S || S->Hi<=0) return {};
				const bool Inside=S->Lo<0;
				const auto T=Inside?S->Hi:S->Lo;
				if (T>1) return {};
				return CellHit{P,T,Inside?S->Exit:S->Entry};
			}
			const auto& Axes=Basis(B.Orientation);
			const Vec Offset{Origin[0]-P.X-.5,Origin[1]-P.Y-.5,Origin[2]-P.Z-.5};
			Vec O{.5,.5,.5},D{};
			for (unsigned I=0;I<3;++I) for (unsigned A=0;A<3;++A) {
				O[I]+=Axes[I][A]*Offset[A];
				D[I]+=Axes[I][A]*Direction[A];
			}
			const bool Low=Spec.Geometry==Shape::Slab || Spec.Geometry==Shape::Stairs;
			auto A=Clip(O,D,{0,0,0},{1,Low?.5:1.,1});
			if (A && Spec.Geometry==Shape::Wedge) {
				const auto Speed=D[1]-D[2], Gap=O[2]-O[1];
				if (Speed==0) {
					if (Gap<=0) return {};
				} else {
					const auto T=Gap/Speed;
					constexpr double K=.7071067811865475244;
					const Vec N{0,K,-K};
					if (Speed<0 && T>A->Lo && !Tied(T,A->Lo)) { A->Lo=T; A->Entry=N; }
					if (Speed>0 && T<A->Hi && !Tied(T,A->Hi)) { A->Hi=T; A->Exit=N; }
					if (A->Hi<=A->Lo || Tied(A->Lo,A->Hi)) return {};
				}
			}
			std::optional<Interval> Other;
			// Overlap both boxes inside the back half so a ray on the internal tread seam stays inside the union.
			if (Spec.Geometry==Shape::Stairs) Other=Clip(O,D,{0,0,.5},{1,1,1});
			if (!A || (Other && Other->Lo<A->Lo)) std::swap(A,Other);
			if (A && Other && (Other->Lo<=A->Hi || Tied(Other->Lo,A->Hi))) {
				if (Other->Hi>A->Hi) { A->Hi=Other->Hi; A->Exit=Other->Exit; }
				Other.reset();
			}
			if (A && A->Hi<=0) A=Other;
			if (!A || A->Hi<=0) return {};
			const bool Inside=A->Lo<0;
			const auto T=Inside?A->Hi:A->Lo;
			if (T>1) return {};
			const auto Normal=Inside?A->Exit:A->Entry;
			Vec N{};
			for (unsigned I=0;I<3;++I) for (unsigned J=0;J<3;++J) N[I]+=Axes[J][I]*Normal[J];
			return CellHit{P,T,N};
		}
	}
	std::optional<CellHit> Cast(const Field& Grid, Vec O, Vec D, unsigned Budget) {
		if (!Budget || Budget>MaximumRayVisits || D==Vec{})
			throw std::invalid_argument("[BlockGrid:Raycast] Invalid ray budget or zero direction");
		for (unsigned A=0; A<3; ++A)
			if (!std::isfinite(O[A]) || !std::isfinite(D[A])) throw std::invalid_argument("[BlockGrid:Raycast] Nonfinite ray");
		if (!Grid.GetOccupiedCount()) return {};
		const Vec Min{-CoordinateLimit,-CoordinateLimit,-CoordinateLimit}, Max{CoordinateLimit,CoordinateLimit,CoordinateLimit};
		const auto Domain=Clip(O,D,Min,Max);
		if (!Domain || Domain->Hi<=0 || Domain->Lo>1) return {};
		const auto Start=std::max(0.0,Domain->Lo);
		std::array<int,3> P{}, Step{};
		Vec Next{};
		const auto Boundary = [&](unsigned A) {
			return Step[A] ? (static_cast<double>(P[A]+(Step[A]>0))-O[A])/D[A] : Inf;
		};
		for (unsigned A=0; A<3; ++A) {
			const auto At=std::clamp(std::fma(Start,D[A],O[A]),Min[A],Max[A]);
			P[A]=static_cast<int>(std::floor(At));
			Step[A]=(D[A]>0)-(D[A]<0);
			if (Step[A]<0 && At==P[A]) --P[A];
			Next[A]=Boundary(A);
		}
		std::optional<CellHit> Best;
		for (unsigned Visit=0; Visit<Budget; ++Visit) {
			const Cell Here{P[0],P[1],P[2]};
			if (!ValidCell(Here)) return Best;
			if (const auto B=Grid.Get(Here);B.Definition!=Air) {
				const auto Hit=HitShape(Here,Grid.GetDefinition(B.Definition),B,O,D);
				if (Hit && (!Best || (Hit->Fraction<Best->Fraction && !Tied(Hit->Fraction,Best->Fraction)) ||
					(Tied(Hit->Fraction,Best->Fraction) && CellPrecedes(Here,Best->Position)))) Best=Hit;
			}
			const auto T=std::min({Next[0],Next[1],Next[2]});
			if (T>1 || (Best && T>Best->Fraction && !Tied(T,Best->Fraction))) return Best;
			for (unsigned A=0; A<3; ++A)
				if (Tied(Next[A],T)) { P[A]+=Step[A]; Next[A]=Boundary(A); }
		}
		throw std::length_error("[BlockGrid:Raycast] Cell visit budget exceeded before completion");
	}
}
