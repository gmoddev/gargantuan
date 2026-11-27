// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridPeer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gargantuan::network {
	std::optional<blockgrid::Region> GridLocalBounds(const CFrame& Pose,float Size,const SpatialDestinationBounds& Bounds) {
		using namespace blockgrid;
		glm::dvec3 Low(std::numeric_limits<double>::infinity()),High(-std::numeric_limits<double>::infinity());
		if (!std::isfinite(Size) || Size<=0 || glm::any(glm::greaterThan(Bounds.Minimum,Bounds.Maximum))) return {};
		for (unsigned Mask=0;Mask<8;++Mask) {
			const glm::dvec3 Corner{(Mask&1)?Bounds.Maximum.x:Bounds.Minimum.x,(Mask&2)?Bounds.Maximum.y:Bounds.Minimum.y,
				(Mask&4)?Bounds.Maximum.z:Bounds.Minimum.z};
			const auto Local=glm::transpose(glm::dmat3(Pose.Rotation))*(Corner-glm::dvec3(Pose.Position))/double(Size);
			for (int I=0;I<3;++I) if (!std::isfinite(Local[I])) return {};
			Low=glm::min(Low,Local);High=glm::max(High,Local);
		}
		// Closed physical bounds include the cells on both sides of an exactly shared face.
		glm::ivec3 First,End;
		for (int I=0;I<3;++I) {
			if (Low[I]<-CoordinateLimit || High[I]>=CoordinateLimit) return {};
			First[I]=std::max(-CoordinateLimit,static_cast<int>(std::ceil(Low[I]))-1);
			End[I]=static_cast<int>(std::floor(High[I]))+1;
		}
		return Region{{First.x,First.y,First.z},{End.x,End.y,End.z}};
	}
	blockgrid::ReplicaCoverage SelectGridCoverage(const blockgrid::Field& Field,const CFrame& Pose,float Size,const GridInterest& Interest) {
		using namespace blockgrid;
		ReplicaCoverage Result;
		const auto Required=Interest.Destination?Interest.Destination:Interest.ControlledMotion;
		std::size_t Occupied{};
		const bool LimitOccupancy=Field.GetLeafCount()>MaximumReplicaPages;
		if (Required) {
			const auto R=GridLocalBounds(Pose,Size,*Required);
			if (!R) return Result;
			const auto A=LeafOrigin(R->Min),B=LeafOrigin({R->MaxExclusive.X-1,R->MaxExclusive.Y-1,R->MaxExclusive.Z-1});
			constexpr int Side=LeafSide;
			const auto Count=std::uint64_t(1+(B.X-A.X)/Side)*(1+(B.Y-A.Y)/Side)*(1+(B.Z-A.Z)/Side);
			if (Count>MaximumReplicaRectanglePages) return Result;
			Result.Rectangle=Region{A,{B.X+Side,B.Y+Side,B.Z+Side}};
			if (Interest.Destination || !Interest.Count) return Result;
			if (LimitOccupancy) {
				for (int Z=A.Z;Z<=B.Z;Z+=Side) for (int Y=A.Y;Y<=B.Y;Y+=Side) for (int X=A.X;X<=B.X;X+=Side)
					if (Field.HasOccupiedPage({X,Y,Z}) && ++Occupied>MaximumReplicaPages) return Result;
			}
		}
		if (!Interest.Count && !Required) {
			Result.Whole=Field.GetLeafCount()<=WholeReplicaPages;
			return Result;
		}
		for (std::size_t I=0;I<std::min(Interest.Count,Interest.Centers.size());++I) {
			const auto P=Interest.Centers[I];
			const auto R=GridLocalBounds(Pose,Size,{P,P});
			if (!R) continue;
			const auto A=LeafOrigin({R->MaxExclusive.X-1,R->MaxExclusive.Y-1,R->MaxExclusive.Z-1});
			constexpr int Side=LeafSide;
			for (int Z=-1;Z<=1;++Z) for (int Y=-1;Y<=1;++Y) for (int X=-1;X<=1;++X) {
				const Cell O{A.X+Side*X,A.Y+Side*Y,A.Z+Side*Z};
				if (!ValidCell(O)) continue;
				if (!Result.Rectangle) Result.Origins.push_back(O);
				else if (Result.Origins.size()<MaximumReplicaPages) {
					if (Result.Contains(SingleCell(O))) continue;
					const bool HasCells=LimitOccupancy && Field.HasOccupiedPage(O);
					if (HasCells && Occupied==MaximumReplicaPages) continue;
					const auto At=std::ranges::lower_bound(Result.Origins,O,CellOrder{});
					Result.Origins.insert(At,O);
					Occupied+=HasCells;
				}
			}
		}
		std::ranges::sort(Result.Origins,CellOrder{});
		Result.Origins.erase(std::unique(Result.Origins.begin(),Result.Origins.end()),Result.Origins.end());
		return Result;
	}
}
