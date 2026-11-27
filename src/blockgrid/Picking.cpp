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
			if (Grid.Get(Here).Definition!=Air) {
				const Vec Lo{double(P[0]),double(P[1]),double(P[2])};
				const Vec Hi{Lo[0]+1,Lo[1]+1,Lo[2]+1};
				if (const auto S=Clip(O,D,Lo,Hi); S && S->Hi>0) {
					const bool Inside=S->Lo<0;
					const auto T=Inside?S->Hi:S->Lo;
					if (T<=1 && (!Best || (T<Best->Fraction && !Tied(T,Best->Fraction)) ||
						(Tied(T,Best->Fraction) && CellPrecedes(Here,Best->Position))))
						Best=CellHit{Here,T,Inside?S->Exit:S->Entry};
				}
			}
			const auto T=std::min({Next[0],Next[1],Next[2]});
			if (T>1 || (Best && T>Best->Fraction && !Tied(T,Best->Fraction))) return Best;
			for (unsigned A=0; A<3; ++A)
				if (Tied(Next[A],T)) { P[A]+=Step[A]; Next[A]=Boundary(A); }
		}
		throw std::length_error("[BlockGrid:Raycast] Cell visit budget exceeded before completion");
	}
}
