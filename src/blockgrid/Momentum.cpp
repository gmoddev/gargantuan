// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Momentum.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		constexpr double NativeEpsilon=std::numeric_limits<float>::epsilon();
		double Maximum(glm::dvec3 V) { return std::max({std::abs(V.x),std::abs(V.y),std::abs(V.z)}); }
		glm::dmat3 ParallelAxis(glm::dvec3 Offset) {
			return glm::dmat3(glm::dot(Offset,Offset))-glm::outerProduct(Offset,Offset);
		}
		void CheckPartition(const RigidMass& Whole,const RigidMass& A,const RigidMass& B) {
			for (const auto* M:{&Whole,&A,&B}) {
				const auto& I=M->WorldInertia;
				for (unsigned Row=0;Row<3;++Row) {
					if (!std::isfinite(M->Center[Row])) throw std::invalid_argument("[BlockGrid:Motion] Nonfinite center");
					for (unsigned Col=0;Col<3;++Col)
						if (!std::isfinite(I[Row][Col]) || std::abs(I[Row][Col]-I[Col][Row])>1e-5*(1+std::abs(I[Row][Col])))
							throw std::invalid_argument("[BlockGrid:Motion] Invalid native inertia tensor");
				}
				if (!(std::isfinite(M->Mass) && M->Mass>0 && I[0][0]>0 && I[0][0]*I[1][1]-I[0][1]*I[1][0]>0 && glm::determinant(I)>0))
					throw std::invalid_argument("[BlockGrid:Motion] Mass tensor is singular");
			}
			const auto OA=A.Center-Whole.Center,OB=B.Center-Whole.Center;
			const auto Shift=(OA*A.Mass+OB*B.Mass)/Whole.Mass;
			const auto Inertia=A.WorldInertia+B.WorldInertia+A.Mass*ParallelAxis(OA)+B.Mass*ParallelAxis(OB);
			// World pose storage is float. Account for representational error in the world centers and local tensors.
			const double PositionError=1e-5+4*NativeEpsilon*std::max({1.,Maximum(A.Center),Maximum(B.Center),Maximum(Whole.Center)});
			if (std::abs((A.Mass+B.Mass)-Whole.Mass)>1e-4*Whole.Mass || Maximum(Shift)>PositionError)
				throw std::domain_error("[BlockGrid:Motion] Native mass or world center would change across the transfer");
			const double TensorError=2e-4*std::max({1.,Maximum(Inertia[0]),Maximum(Inertia[1]),Maximum(Inertia[2])})+
				4*PositionError*(A.Mass*glm::length(OA)+B.Mass*glm::length(OB));
			for (unsigned I=0;I<3;++I) if (!(Maximum(Inertia[I]-Whole.WorldInertia[I])<=TensorError))
				throw std::domain_error("[BlockGrid:Motion] Native geometry does not preserve the inertia tensor");
		}
		RigidVelocity Bounded(RigidVelocity V) {
			if (!(glm::length(V.Linear)<=512. && glm::length(V.Angular)<=64.))
				throw std::domain_error("[BlockGrid:Motion] Conserved motion exceeds construction velocity limits");
			return V;
		}
	}
	std::array<RigidVelocity,2> SplitMomentum(const RigidMass& Original,RigidVelocity Velocity,const RigidMass& A,const RigidMass& B) {
		CheckPartition(Original,A,B);
		const auto Child=[&](const RigidMass& M) {
			return Bounded({Velocity.Linear+glm::cross(Velocity.Angular,M.Center-Original.Center),Velocity.Angular});
		};
		return {Child(A),Child(B)};
	}
	RigidVelocity MergeMomentum(const RigidMass& A,RigidVelocity VA,const RigidMass& B,RigidVelocity VB,const RigidMass& Merged) {
		CheckPartition(Merged,A,B);
		const auto PA=A.Mass*VA.Linear,PB=B.Mass*VB.Linear;
		const auto L=A.WorldInertia*VA.Angular+B.WorldInertia*VB.Angular+
			glm::cross(A.Center-Merged.Center,PA)+glm::cross(B.Center-Merged.Center,PB);
		return Bounded({(PA+PB)/Merged.Mass,glm::inverse(Merged.WorldInertia)*L});
	}
}
