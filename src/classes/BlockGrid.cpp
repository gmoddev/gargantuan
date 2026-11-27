// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/blockgrid/Picking.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace gargantuan {
	namespace {
		using namespace blockgrid;
		using FacePair = std::array<unsigned,2>;
		constexpr std::array<std::string_view,6> Faces{"+X","-X","+Y","-Y","+Z","-Z"};
		constexpr auto Rotations = [] {
			std::array<FacePair,24> Values{};
			Values[0] = {2,5};
			unsigned I = 1;
			for (unsigned Up=0; Up<6; ++Up) for (unsigned Front=0; Front<6; ++Front)
				if (Up/2 != Front/2 && FacePair{Up,Front} != Values[0]) Values[I++]={Up,Front};
			return Values;
		}();
		BlockGrid& ReadTarget(lua_State* L, Instance* Object, bool Edit = false) {
			const auto* Script = ScriptEngine::Get(L);
			const auto& Security = GetCurrentScriptSecurityContext();
			if (!Script || Script->IsStopping() || !Script->DataModel || GetCurrentExecutionDomain()!=ExecutionDomain::Main ||
				!Security.HasCapability(ScriptCapability::ReadDataModel) || Object->GetDestroyed() || Object->IsDestroying() ||
				Object->GetDataModel()!=Script->DataModel)
				throw std::runtime_error("[BlockGrid:Access] Grid must be live in the current DataModel");
			if (Script->Mode==RuntimeMode::NetworkClient)
				throw std::runtime_error("[BlockGrid:Unavailable] No verified BlockGrid replica data");
			if (Edit && (Security.Domain!=ScriptExecutionDomain::Server || !Security.HasCapability(ScriptCapability::MutateDataModel)))
				throw std::runtime_error("[BlockGrid:Access] Authoritative server gameplay context required");
			return static_cast<BlockGrid&>(*Object);
		}
		Cell ReadCell(lua_State* L, int I, bool Exclusive = false) {
			const auto V = CheckStackValue<glm::vec3>(L,I);
			const auto Parse = [Exclusive](float N) {
				if (!std::isfinite(N) || std::trunc(N)!=N || N < -CoordinateLimit ||
					N > CoordinateLimit - static_cast<int>(!Exclusive))
					throw std::invalid_argument("[BlockGrid:Bounds] Expected in-range integer cell coordinates");
				return static_cast<std::int32_t>(N);
			};
			return {Parse(V.x),Parse(V.y),Parse(V.z)};
		}
		std::string_view StringAt(lua_State* L, int I) {
			if (lua_type(L,I)!=LUA_TSTRING) throw std::invalid_argument("[BlockGrid:Argument] Expected string");
			size_t N = 0;
			const auto* S = lua_tolstring(L,I,&N);
			return {S,N};
		}
		unsigned Face(lua_State* L, int I, const char* Key) {
			lua_pushstring(L,Key);
			lua_rawget(L,I);
			const auto Name=StringAt(L,-1);
			const auto It=std::ranges::find(Faces,Name);
			lua_pop(L,1);
			if (It==Faces.end()) throw std::invalid_argument("[BlockGrid:Orientation] Unknown face");
			return static_cast<unsigned>(It-Faces.begin());
		}
		Block ReadBlock(lua_State* L, int Definition, int Orientation) {
			const auto Name=StringAt(L,Definition);
			Block B;
			if (Name=="Stone") B.Definition=Stone;
			else if (Name=="Brick") B.Definition=Brick;
			else throw std::invalid_argument("[BlockGrid:Definition] Unknown block definition");
			if (lua_isnoneornil(L,Orientation)) return B;
			if (!lua_istable(L,Orientation)) throw std::invalid_argument("[BlockGrid:Orientation] Expected an Up and Front table");
			const FacePair Pair{Face(L,Orientation,"Up"),Face(L,Orientation,"Front")};
			const auto It=std::ranges::find(Rotations,Pair);
			if (It==Rotations.end()) throw std::invalid_argument("[BlockGrid:Orientation] Up and Front must be perpendicular");
			B.Orientation=static_cast<std::uint8_t>(It-Rotations.begin());
			return B;
		}
		void PushText(lua_State* L, std::string_view S) { lua_pushlstring(L,S.data(),S.size()); }
		void PushBlock(lua_State* L, Block B) {
			if (B.Definition==Air) { lua_pushnil(L); return; }
			lua_createtable(L,0,2);
			PushText(L,B.Definition==Stone?"Stone":"Brick"); lua_setfield(L,-2,"Definition");
			lua_createtable(L,0,2);
			PushText(L,Faces[Rotations[B.Orientation][0]]); lua_setfield(L,-2,"Up");
			PushText(L,Faces[Rotations[B.Orientation][1]]); lua_setfield(L,-2,"Front");
			lua_setreadonly(L,-1,true); lua_setfield(L,-2,"Orientation");
			lua_setreadonly(L,-1,true);
		}
		int Submit(lua_State* L, BlockGrid& Grid, blockgrid::Write W) {
			auto Edit=std::make_shared<EditRequest>();
			Edit->Writes.push_back(W);
			MutationGateway Gateway;
			const auto Result=Gateway.Apply(SubmitBlockGridEditCommand{Grid.GetObjectId(),Grid.GetReplicationScopeId(),std::move(Edit)});
			if (!Result.Succeeded() || !Result.BlockRevision)
				throw std::runtime_error(Result.Message.empty()?"[BlockGrid:Edit] Rejected":Result.Message);
			PushText(L,*Result.BlockRevision);
			return 1;
		}
		bool ValidTransform(const CFrame& Frame) {
			for (unsigned I=0; I<3; ++I) {
				if (!std::isfinite(Frame.Position[I]) || std::abs(Frame.Position[I])>CoordinateLimit) return false;
				for (unsigned J=0; J<3; ++J) {
					const float D=glm::dot(Frame.Rotation[I],Frame.Rotation[J]);
					if (!std::isfinite(D) || std::abs(D-(I==J?1.f:0.f))>1e-4f) return false;
				}
			}
			return std::abs(glm::determinant(Frame.Rotation)-1.f)<=1e-4f;
		}
	}
	BlockGrid::BlockGrid() { Name="BlockGrid"; }
	std::optional<std::shared_ptr<SpatialRegion>> BlockGrid::GetSpatialRegion() const { return GetAssignedRegion(); }
	void BlockGrid::SetSpatialRegion(std::optional<std::shared_ptr<SpatialRegion>> Value) {
		CheckConfigurationWrite();
		const auto Before=GetAssignedRegion();
		SetAssignedRegion(*this,std::move(Value));
		if (Before!=GetAssignedRegion()) {
			++ConfigurationRevision;
			NotifyPropertyCommitted("BlockRevision");
		}
	}
	const blockgrid::Field& BlockGrid::GetField() const {
		AssertAuthoritativeMutation("BlockGrid field access");
		AssertIsAlive();
		return Cells;
	}
	void BlockGrid::RestoreField(blockgrid::Field Value) {
		AssertCanMutate();
		if (GetDataModel() || Cells.GetRevision()!=1 || Cells.GetLeafCount() || Admission || Editing)
			throw std::logic_error("[BlockGrid:Restore] Only an unpublished detached grid may restore native data");
		Cells=std::move(Value);
	}
	void BlockGrid::SetAdmission(blockgrid::Field::Admission BeforeCommit) {
		AssertCanMutate();
		if (Editing) throw std::logic_error("[BlockGrid:Admission] Cannot replace an active admission hook");
		Admission=std::move(BeforeCommit);
	}
	std::string BlockGrid::GetBlockRevision() const {
		const auto Scope=GetReplicationScopeId(), Id=GetObjectId();
		const auto World=WorldRoot::GetEffectiveWorld(*this);
		const auto Domain=World?World->GetObjectId():ObjectId{};
		return std::format("{}.{}:{}.{}:{}.{}:{}:{}",Scope.Slot,Scope.Generation,Id.Slot,Id.Generation,Domain.Slot,Domain.Generation,
			ConfigurationRevision,Cells.GetRevision());
	}
	std::string BlockGrid::ApplyWrites(std::span<const blockgrid::Write> Writes, const std::optional<std::string>& Expected,
		bool ReplayingCells) {
		AssertCanMutate();
		const auto World=GetDataModel();
		if (!World || Editing) throw std::logic_error("[BlockGrid:Edit] Grid is unavailable for admission");
		if (Expected && *Expected!=GetBlockRevision()) throw std::domain_error("[BlockGrid:Conflict] Revision changed");
		World->EnsureAuthoritativeRevisionAvailable();
		bool Changed;
		{
			struct Guard { bool& Flag; ~Guard(){ Flag=false; } } Reset{Editing};
			Editing=true;
			Changed=ReplayingCells ? Cells.ApplyCellPatch(Writes,Admission) : Cells.Apply(Writes,{},Admission);
		}
		const auto Revision=GetBlockRevision();
		if (Changed) {
			World->AdvanceAuthoritativeRevision();
			NotifyPropertyCommitted("BlockRevision");
		}
		return Revision;
	}
	void BlockGrid::CheckConfigurationWrite() const {
		AssertCanMutate();
		if (Editing || GetCurrentScriptSecurityContext().Domain==ScriptExecutionDomain::Client)
			throw std::runtime_error("[BlockGrid:Access] Grid configuration requires local authority");
		if (ConfigurationRevision==std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("[BlockGrid:Revision] Configuration revision exhausted");
	}
	void BlockGrid::NotifyConfiguration(std::string_view Property) {
		++ConfigurationRevision;
		NotifyPropertyCommitted(Property);
		NotifyPropertyCommitted("BlockRevision");
	}
	float BlockGrid::GetCellSize() const { return Size; }
	void BlockGrid::SetCellSize(float Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CellSize",Value);
		if (Size==Value) return;
		if (Cells.GetOccupiedCount() || (Value!=0.5f && Value!=1.f && Value!=2.f))
			throw std::invalid_argument("[BlockGrid:CellSize] Choose 0.5, 1, or 2 while the grid is empty");
		Size=Value; NotifyConfiguration("CellSize");
	}
	CFrame BlockGrid::GetCFrame() const { return Transform; }
	void BlockGrid::SetCFrame(gargantuan::CFrame Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CFrame",Value);
		if (!ValidTransform(Value)) throw std::invalid_argument("[BlockGrid:CFrame] Expected an in-range proper rigid transform");
		if (Value.Position==Transform.Position && Value.Rotation==Transform.Rotation) return;
		Transform=Value; NotifyConfiguration("CFrame");
	}
	bool BlockGrid::GetVisible() const { return IsVisible; }
	void BlockGrid::SetVisible(bool Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("Visible",Value);
		if (IsVisible!=Value) { IsVisible=Value; NotifyConfiguration("Visible"); }
	}
	bool BlockGrid::GetCanCollide() const { return Collides; }
	void BlockGrid::SetCanCollide(bool Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CanCollide",Value);
		if (Collides!=Value) { Collides=Value; NotifyConfiguration("CanCollide"); }
	}
	bool BlockGrid::GetCanQuery() const { return Queryable; }
	void BlockGrid::SetCanQuery(bool Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CanQuery",Value);
		if (Queryable!=Value) { Queryable=Value; NotifyConfiguration("CanQuery"); }
	}
	int BlockGrid::GetRevision(lua_State* L,Instance* Object) {
		PushText(L,ReadTarget(L,Object).GetBlockRevision()); return 1;
	}
	int BlockGrid::SetBlock(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{SingleCell(ReadCell(L,2)),ReadBlock(L,3,4)});
	}
	int BlockGrid::RemoveBlock(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{SingleCell(ReadCell(L,2)),{}});
	}
	int BlockGrid::GetBlock(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		PushBlock(L,Grid.Cells.Get(ReadCell(L,2))); return 1;
	}
	int BlockGrid::Fill(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{{ReadCell(L,2),ReadCell(L,3,true)},ReadBlock(L,4,5)});
	}
	int BlockGrid::Clear(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{{ReadCell(L,2),ReadCell(L,3,true)},{}});
	}
	int BlockGrid::ReadRegion(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const Region R{ReadCell(L,2),ReadCell(L,3,true)};
		(void)Volume(R,MaximumReadCells);
		lua_createtable(L,0,2);
		PushText(L,Grid.GetBlockRevision()); lua_setfield(L,-2,"Revision");
		lua_newtable(L);
		int N=0;
		for (int Z=R.Min.Z; Z<R.MaxExclusive.Z; ++Z) for (int Y=R.Min.Y; Y<R.MaxExclusive.Y; ++Y)
			for (int X=R.Min.X; X<R.MaxExclusive.X; ++X) {
				const auto B=Grid.Cells.Get({X,Y,Z});
				if (B.Definition==Air) continue;
				lua_createtable(L,0,2);
				StackValue<glm::vec3>::Push(L,glm::vec3(X,Y,Z)); lua_setfield(L,-2,"Position");
				PushBlock(L,B); lua_setfield(L,-2,"Block"); lua_rawseti(L,-2,++N);
			}
		lua_setfield(L,-2,"Cells"); return 1;
	}
	int BlockGrid::CellToWorld(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const auto P=ReadCell(L,2);
		const auto Center=(glm::vec3(P.X,P.Y,P.Z)+glm::vec3(0.5f))*Grid.Size;
		return StackValue<glm::vec3>::Push(L,Grid.Transform.PointToWorldSpace(Center));
	}
	int BlockGrid::WorldToCell(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const auto P=CheckStackValue<glm::vec3>(L,2);
		const auto Q=glm::floor(glm::transpose(glm::dmat3(Grid.Transform.Rotation)) *
			(glm::dvec3(P)-glm::dvec3(Grid.Transform.Position))/static_cast<double>(Grid.Size));
		for (unsigned I=0; I<3; ++I)
			if (!std::isfinite(Q[I]) || Q[I]<-CoordinateLimit || Q[I]>=CoordinateLimit)
				throw std::invalid_argument("[BlockGrid:Bounds] World point has no supported cell");
		return StackValue<glm::vec3>::Push(L,glm::vec3(Q));
	}
	void BlockGrid::PushGridHit(lua_State* L,blockgrid::Cell Cell,glm::vec3 Position,glm::vec3 Normal) const {
		const auto B=GetField().Get(Cell);
		if (B.Definition==Air) throw std::logic_error("[BlockGrid:Raycast] Hit owner is no longer occupied");
		lua_createtable(L,0,5);
		StackValue<glm::vec3>::Push(L,glm::vec3(Cell.X,Cell.Y,Cell.Z)); lua_setfield(L,-2,"Cell");
		PushBlock(L,B); lua_setfield(L,-2,"Block");
		StackValue<glm::vec3>::Push(L,Position); lua_setfield(L,-2,"Position");
		StackValue<glm::vec3>::Push(L,Normal); lua_setfield(L,-2,"Normal");
		PushText(L,GetBlockRevision()); lua_setfield(L,-2,"Revision");
		lua_setreadonly(L,-1,true);
	}
	int BlockGrid::Raycast(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const glm::dvec3 O=CheckStackValue<glm::vec3>(L,2), D=CheckStackValue<glm::vec3>(L,3);
		const auto Length=glm::length(D);
		if (!std::isfinite(Length) || Length<0.0001 || Length>100000 ||
			!std::isfinite(O.x) || !std::isfinite(O.y) || !std::isfinite(O.z))
			throw std::invalid_argument("[BlockGrid:Raycast] Expected a finite segment of length 0.0001 through 100000");
		if (!Grid.Queryable) { lua_pushnil(L); return 1; }
		const auto R=glm::dmat3(Grid.Transform.Rotation);
		const auto Local=glm::transpose(R)*(O-glm::dvec3(Grid.Transform.Position))/static_cast<double>(Grid.Size);
		const auto Delta=glm::transpose(R)*D/static_cast<double>(Grid.Size);
		const auto Hit=blockgrid::Cast(Grid.Cells,{Local.x,Local.y,Local.z},{Delta.x,Delta.y,Delta.z});
		if (!Hit) { lua_pushnil(L); return 1; }
		const auto& N=Hit->Normal;
		Grid.PushGridHit(L,Hit->Position,glm::vec3(O+D*Hit->Fraction),glm::vec3(glm::normalize(R*glm::dvec3(N[0],N[1],N[2]))));
		return 1;
	}
}
