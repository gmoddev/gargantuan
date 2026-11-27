// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridBinding.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include <cmath>
#include <limits>

namespace gargantuan::blockgrid {
	bool ValidAssemblyProjection(const AssemblyProjection& P) noexcept {
		const auto Bounded=[](glm::vec3 V,float Limit){return glm::all(glm::lessThanEqual(glm::abs(V),glm::vec3(Limit)));};
		if (P.Version!=AssemblyProjection::CurrentVersion || !P.Revision || !Bounded(P.Pose.Position,CoordinateLimit) ||
			!Bounded(P.LinearVelocity,1e6f) || !Bounded(P.AngularVelocity,1e6f) || !Bounded(P.LocalCenterOfMass,256) ||
			!(P.Mass>=0 && P.Mass<=2'048.f)) return false;
		for (unsigned I=0;I<3;++I) {
			if (!Bounded(P.Pose.Rotation[I],1.0001f) || !Bounded(P.LocalInertia[I],1e8f) || P.LocalInertia[I][I]<0) return false;
			for (unsigned J=0;J<3;++J) {
				if (std::abs(glm::dot(P.Pose.Rotation[I],P.Pose.Rotation[J])-(I==J?1.f:0.f))>1e-4f ||
					std::abs(P.LocalInertia[I][J]-P.LocalInertia[J][I])>1e-4f*(1.f+std::abs(P.LocalInertia[I][J]))) return false;
			}
		}
		if (std::abs(glm::determinant(P.Pose.Rotation)-1.f)>1e-4f) return false;
		return P.Dynamic?P.Mass>0:P.Mass==0 && P.LinearVelocity==glm::vec3(0) && P.AngularVelocity==glm::vec3(0) &&
			P.LocalCenterOfMass==glm::vec3(0) && P.LocalInertia==glm::mat3(0);
	}
}

namespace gargantuan {
	namespace {
		bool SameProjection(const blockgrid::AssemblyProjection& A,const blockgrid::AssemblyProjection& B) {
			return A.Dynamic==B.Dynamic && A.Pose.Position==B.Pose.Position && A.Pose.Rotation==B.Pose.Rotation &&
				A.LinearVelocity==B.LinearVelocity && A.AngularVelocity==B.AngularVelocity && A.Mass==B.Mass &&
				A.LocalCenterOfMass==B.LocalCenterOfMass && A.LocalInertia==B.LocalInertia;
		}
	}
	std::optional<blockgrid::AssemblyProjection> BlockGrid::GetAssemblyProjection() const {
		AssertAuthoritativeMutation("BlockGrid motion projection"); AssertIsAlive();
		if (AssemblyState) return *AssemblyState;
		if (IsReplicaMaterialization()) return {};
		blockgrid::AssemblyProjection P; P.Pose=Transform; P.Revision=AssemblyRevision; return P;
	}
	bool BlockGrid::ApplyReplicaAssemblyProjection(const blockgrid::AssemblyProjection& P) {
		AssertCanMutate();
		if (!IsReplicaMaterialization() || DynamicAssembly || !blockgrid::ValidAssemblyProjection(P))
			throw std::invalid_argument("[BlockGrid:Motion] Invalid replica motion projection");
		if (AssemblyState && P.Revision<AssemblyState->Revision) return false;
		if (AssemblyState && P.Revision==AssemblyState->Revision) {
			if (!SameProjection(*AssemblyState,P)) throw std::domain_error("[BlockGrid:Motion] Conflicting equal-revision motion projection");
			return false;
		}
		const bool Changed=!AssemblyState || AssemblyState->Dynamic!=P.Dynamic;
		const auto World=GetDataModel();
		if (!World) throw std::invalid_argument("[BlockGrid:Motion] Detached replica");
		if (const auto Protection=World->GetSpatialTransferProtection(); Protection && !Protection->AllowsResource(*this))
			throw std::domain_error("[BlockGrid:Motion] Native collision is reserved for a spatial transfer");
		if (!AssemblyState) AssemblyState=std::make_unique<blockgrid::AssemblyProjection>();
		const bool Moved=Transform.Position!=P.Pose.Position || Transform.Rotation!=P.Pose.Rotation;
		*AssemblyState=P;
		Transform=P.Pose;
		ScopedSignalDeferral Signals;
		if (Moved) NotifyPropertyCommitted("CFrame");
		if (Changed) NotifyPropertyCommitted("MotionMode");
		Signals.Commit();
		return true;
	}
	void BlockGrid::ClearReplicaAssemblyProjection() {
		AssertCanMutate();
		if (!IsReplicaMaterialization()) throw std::logic_error("[BlockGrid:Motion] Expected a replica");
		if (AssemblyState) { AssemblyState.reset(); NotifyPropertyCommitted("MotionMode"); }
	}
	void BlockGrid::PublishAssemblyState(const blockgrid::AssemblyProjection& Value) {
		if (AssemblyState && SameProjection(*AssemblyState,Value)) return;
		if (AssemblyRevision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("[BlockGrid:Motion] Projection revision exhausted");
		if (!AssemblyState) AssemblyState=std::make_unique<blockgrid::AssemblyProjection>();
		*AssemblyState=Value; AssemblyState->Revision=++AssemblyRevision;
	}
	void BlockGrid::RetireAssemblyState() {
		AssemblyState.reset(); ++AssemblyRevision;
	}
	std::string BlockGrid::GetMotionMode() const {
		if (IsReplicaMaterialization()) return AssemblyState?(AssemblyState->Dynamic?"Dynamic":"Static"):"Unavailable";
		return DynamicAssembly?"Dynamic":"Static";
	}
	int BlockGrid::SetMotionMode(lua_State* L,Instance* Object) {
		using namespace blockgrid::binding;
		auto& G=ReadTarget(L,Object,true);
		const auto Mode=StringAt(L,2);
		if (Mode!="Static" && Mode!="Dynamic") throw std::invalid_argument("[BlockGrid:Motion] Expected Static or Dynamic");
		auto E=std::make_shared<blockgrid::EditRequest>();
		E->ExpectedRevision=StringAt(L,3); E->Motion=blockgrid::MotionEdit{Mode=="Dynamic"};
		return SubmitRequest(L,G,std::move(E));
	}
	int BlockGrid::SetAssemblyVelocity(lua_State* L,Instance* Object) {
		using namespace blockgrid::binding;
		auto& G=ReadTarget(L,Object,true);
		const auto V=CheckStackValue<glm::vec3>(L,2), A=CheckStackValue<glm::vec3>(L,3);
		auto E=std::make_shared<blockgrid::EditRequest>();
		E->ExpectedRevision=StringAt(L,4);
		E->Motion=blockgrid::MotionEdit{true,std::pair{blockgrid::Point{V.x,V.y,V.z},blockgrid::Point{A.x,A.y,A.z}}};
		return SubmitRequest(L,G,std::move(E));
	}
	int BlockGrid::GetAssemblyState(lua_State* L,Instance* Object) {
		using namespace blockgrid::binding;
		auto& G=ReadTarget(L,Object);
		const auto State=G.GetAssemblyProjection();
		if (!State) throw std::runtime_error("[BlockGrid:Unavailable] No verified assembly state");
		lua_createtable(L,0,8);
		PushText(L,G.GetMotionMode()); lua_setfield(L,-2,"Mode");
		StackValue<gargantuan::CFrame>::Push(L,State->Pose); lua_setfield(L,-2,"CFrame");
		const auto Vector=[&](const char* Name,glm::vec3 Value){StackValue<glm::vec3>::Push(L,Value);lua_setfield(L,-2,Name);};
		Vector("LinearVelocity",State->LinearVelocity); Vector("AngularVelocity",State->AngularVelocity);
		Vector("LocalCenterOfMass",State->LocalCenterOfMass); Vector("CenterOfMass",State->Pose.PointToWorldSpace(State->LocalCenterOfMass));
		lua_pushnumber(L,State->Mass); lua_setfield(L,-2,"Mass");
		lua_createtable(L,3,0);
		for (int I=0;I<3;++I) { StackValue<glm::vec3>::Push(L,State->LocalInertia[I]);lua_rawseti(L,-2,I+1); }
		lua_setreadonly(L,-1,true);lua_setfield(L,-2,"LocalInertia");
		lua_setreadonly(L,-1,true); return 1;
	}
}
