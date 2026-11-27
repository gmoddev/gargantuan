// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/classes/BlockGrid.hpp"
#include "BlockGridBinding.hpp"
#include "gargantuan/blockgrid/Assets.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/blockgrid/Picking.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/datatypes/Color3.hpp"
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
	namespace blockgrid::binding {
		using namespace blockgrid;
		constexpr std::array<std::string_view,6> Faces{"+X","-X","+Y","-Y","+Z","-Z"};
		BlockGrid& ReadTarget(lua_State* L, Instance* Object, bool Edit,bool AllowMissing) {
			const auto* Script = ScriptEngine::Get(L);
			const auto& Security = GetCurrentScriptSecurityContext();
			if (!Script || Script->IsStopping() || !Script->DataModel || GetCurrentExecutionDomain()!=ExecutionDomain::Main ||
				!Security.HasCapability(ScriptCapability::ReadDataModel) || Object->GetDestroyed() || Object->IsDestroying() ||
				Object->GetDataModel()!=Script->DataModel)
				throw std::runtime_error("[BlockGrid:Access] Grid must be live in the current DataModel");
			if (Script->Mode==RuntimeMode::NetworkClient && (Edit || (!AllowMissing && !static_cast<BlockGrid&>(*Object).HasReplicaField())))
				throw std::runtime_error("[BlockGrid:Unavailable] No verified BlockGrid replica data");
			if (Edit && (Security.Domain!=ScriptExecutionDomain::Server || !Security.HasCapability(ScriptCapability::MutateDataModel)))
				throw std::runtime_error("[BlockGrid:Access] Authoritative server gameplay context required");
			return static_cast<BlockGrid&>(*Object);
		}
		Cell ReadCell(lua_State* L, int I, bool Exclusive) {
			if (!StackValue<glm::vec3>::Is(L,I)) throw std::invalid_argument("[BlockGrid:Bounds] Expected a cell Vector3");
			const auto V = StackValue<glm::vec3>::From(L,I);
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
		unsigned ReadFace(lua_State* L, int I, const char* Key) {
			lua_pushstring(L,Key);
			lua_rawget(L,I);
			const auto Name=StringAt(L,-1);
			const auto It=std::ranges::find(Faces,Name);
			lua_pop(L,1);
			if (It==Faces.end()) throw std::invalid_argument("[BlockGrid:Orientation] Unknown face");
			return static_cast<unsigned>(It-Faces.begin());
		}
		Block WithOrientation(lua_State* L,std::uint16_t Id,int Orientation) {
			Block B{Id};
			if (lua_isnoneornil(L,Orientation)) return B;
			if (!lua_istable(L,Orientation)) throw std::invalid_argument("[BlockGrid:Orientation] Expected an Up and Front table");
			B.Orientation=OrientationIndex(ReadFace(L,Orientation,"Up"),ReadFace(L,Orientation,"Front"));
			return B;
		}
		Block ReadBlock(lua_State* L, const Field& Source, int Key, int Orientation) {
			return WithOrientation(L,Source.GetDefinitionId(StringAt(L,Key)),Orientation);
		}
		void PushText(lua_State* L, std::string_view S) { lua_pushlstring(L,S.data(),S.size()); }
		void PushBlock(lua_State* L, const Field& Source, Block B) {
			if (B.Definition==Air) { lua_pushnil(L); return; }
			lua_createtable(L,0,2);
			PushText(L,Source.GetDefinition(B.Definition).Key); lua_setfield(L,-2,"Definition");
			lua_createtable(L,0,2);
			const auto [Up,Front]=OrientationFaces(B.Orientation);
			PushText(L,Faces[Up]); lua_setfield(L,-2,"Up");
			PushText(L,Faces[Front]); lua_setfield(L,-2,"Front");
			lua_setreadonly(L,-1,true); lua_setfield(L,-2,"Orientation");
			lua_setreadonly(L,-1,true);
		}
		int SubmitRequest(lua_State* L, BlockGrid& Grid, std::shared_ptr<EditRequest> Edit) {
			MutationGateway Gateway;
			const auto Result=Gateway.Apply(SubmitBlockGridEditCommand{Grid.GetObjectId(),Grid.GetReplicationScopeId(),std::move(Edit)});
			if (!Result.Succeeded() || !Result.BlockRevision)
				throw std::runtime_error(Result.Message.empty()?"[BlockGrid:Edit] Rejected":Result.Message);
			PushText(L,*Result.BlockRevision);
			return 1;
		}
		int Submit(lua_State* L, BlockGrid& Grid, blockgrid::Write W) {
			auto Edit=std::make_shared<EditRequest>();
			Edit->Writes.push_back(W);
			return SubmitRequest(L,Grid,std::move(Edit));
		}
		int Place(lua_State* L,BlockGrid& Grid,Region Bounds,int Key,int Orientation) {
			(void)Volume(Bounds);
			const auto Name=StringAt(L,Key);
			const auto& Source=Grid.GetField();
			if (const auto Id=Source.FindDefinitionId(Name)) return Submit(L,Grid,{Bounds,WithOrientation(L,*Id,Orientation)});
			auto Edit=std::make_shared<EditRequest>();
			Edit->Materials.push_back(Grid.ResolveMaterialAlias(Name));
			Edit->Writes.push_back({Bounds,WithOrientation(L,static_cast<std::uint16_t>(Brick+1+Source.GetDefinitionCount()),Orientation)});
			Edit->ExpectedRevision=Grid.GetBlockRevision();
			return SubmitRequest(L,Grid,std::move(Edit));
		}
		Definition ReadDefinition(lua_State* L) {
			if (!lua_istable(L,3)) throw std::invalid_argument("[BlockGrid:Definition] Expected a definition table");
			const auto Key=StringAt(L,2);
			if (Key.size()>56) throw std::invalid_argument("[BlockGrid:Definition] Oversized definition key");
			Definition D{std::string(Key)};
			AssetBinding A;
			std::string_view Collision;
			lua_pushnil(L);
			while (lua_next(L,3)) {
				const auto Name=StringAt(L,-2);
				if (Name=="Shape") {
					constexpr std::array<std::string_view,5> Shapes{"Cube","Slab","Wedge","Stairs","Mesh"};
					const auto It=std::ranges::find(Shapes,StringAt(L,-1));
					if (It==Shapes.end()) throw std::invalid_argument("[BlockGrid:Unsupported] Unknown or unavailable shape");
					D.Geometry=static_cast<Shape>(It-Shapes.begin());
				} else if (Name=="Color") {
					const auto C=CheckStackValue<Color3>(L,-1);
					D.Color={C.R,C.G,C.B};
				} else if (Name=="Material" || Name=="Mesh" || Name=="CollisionAsset") {
					auto Value=StringAt(L,-1);
					if (Value.starts_with("asset://")) Value.remove_prefix(8);
					if (!AssetId::Parse(Value)) throw std::invalid_argument("[BlockGrid:Asset] Expected a project asset ID");
					(Name=="Material"?A.Material:Name=="Mesh"?A.Mesh:A.Collision).assign(Value);
				} else if (Name=="Collision") {
					Collision=StringAt(L,-1);
					if (Collision!="Shape" && Collision!="None" && Collision!="Asset")
						throw std::invalid_argument("[BlockGrid:Unsupported] Unknown collision representation");
					D.CanCollide=Collision!="None";
				} else if (Name=="Queryable") {
					if (lua_type(L,-1)!=LUA_TBOOLEAN) throw std::invalid_argument("[BlockGrid:Definition] Expected Queryable boolean");
					D.Queryable=lua_toboolean(L,-1)!=0;
				} else if (Name=="Ports") {
					if (!lua_istable(L,-1)) throw std::invalid_argument("[BlockGrid:Definition] Expected a face array");
					const auto Count=lua_objlen(L,-1);
					if (Count>6) throw std::invalid_argument("[BlockGrid:Definition] Too many ports");
					D.Ports=0;
					int Seen=0;
					const int Ports=lua_gettop(L);
					lua_pushnil(L);
					while (lua_next(L,Ports)) {
						if (lua_type(L,-2)!=LUA_TNUMBER) throw std::invalid_argument("[BlockGrid:Definition] Invalid face array");
						const double K=lua_tonumber(L,-2);
						if (++Seen>6 || K<1 || K>Count || std::trunc(K)!=K) throw std::invalid_argument("[BlockGrid:Definition] Invalid face array");
						const auto It=std::ranges::find(Faces,StringAt(L,-1));
						if (It==Faces.end() || (D.Ports&(1u<<(It-Faces.begin())))) throw std::invalid_argument("[BlockGrid:Definition] Invalid or duplicate port");
						D.Ports|=static_cast<std::uint8_t>(1u<<(It-Faces.begin()));
						lua_pop(L,1);
					}
					if (Seen!=Count) throw std::invalid_argument("[BlockGrid:Definition] Sparse face array");
				} else throw std::invalid_argument("[BlockGrid:Unsupported] Unknown or unavailable definition property");
				lua_pop(L,1);
			}
			if ((D.Geometry==Shape::Mesh && Collision!="Asset" && Collision!="None") ||
				(Collision=="Asset")!=!A.Collision.empty())
				throw std::invalid_argument("[BlockGrid:Definition] Mesh collision must explicitly use None or a companion Asset");
			if (!A.Material.empty() || !A.Mesh.empty() || !A.Collision.empty()) {
				D.Assets=std::make_shared<const AssetBinding>(std::move(A));
			}
			return D;
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
	using namespace blockgrid;
	using namespace blockgrid::binding;
	BlockGrid::BlockGrid() { Name="BlockGrid"; }
	std::optional<std::shared_ptr<SpatialRegion>> BlockGrid::GetSpatialRegion() const { return GetAssignedRegion(); }
	void BlockGrid::SetSpatialRegion(std::optional<std::shared_ptr<SpatialRegion>> Value) {
		CheckConfigurationWrite();
		const auto Before=GetAssignedRegion();
		if (DynamicAssembly && Before!=Value) throw std::domain_error("[BlockGrid:Motion] Freeze the assembly before changing its spatial domain");
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
		if (GetDataModel() || Cells.GetRevision()!=1 || Cells.GetLeafCount() || Cells.GetDefinitionCount() || Cells.GetChannelCount() || Admission || Editing)
			throw std::logic_error("[BlockGrid:Restore] Only an unpublished detached grid may restore native data");
		Cells=std::move(Value);
	}
	void BlockGrid::SetAdmission(blockgrid::Field::Admission BeforeCommit) {
		AssertCanMutate();
		if (Editing) throw std::logic_error("[BlockGrid:Admission] Cannot replace an active admission hook");
		Admission=std::move(BeforeCommit);
		WorldOwnedAdmission=false;
	}
	void BlockGrid::RequireReplicaCoverage(blockgrid::Region Bounds) const {
		if (IsReplicaMaterialization() && (!ReplicaFieldAvailable || (InstalledCoverage && !InstalledCoverage->Contains(Bounds))))
			throw std::runtime_error("[BlockGrid:Unavailable] Requested cells are outside installed replica coverage");
	}
	void BlockGrid::InstallReplicaField(blockgrid::Field Value,std::shared_ptr<const blockgrid::ReplicaCoverage> Coverage) {
		AssertCanMutate();
		if (!IsReplicaMaterialization() || !GetDataModel() || Editing) throw std::logic_error("[BlockGrid:Replica] Shell is unavailable");
		if (const auto Protection=GetDataModel()->GetSpatialTransferProtection(); Protection && !Protection->AllowsResource(*this))
			throw std::runtime_error("[BlockGrid:Replica] Collision is reserved for an active transfer");
		Cells.InstallReplica(std::move(Value), Admission);
		LastAssetService.reset();
		AssetSequence = 0;
		AssetsReady = false;
		LastCellEdit.reset();
		ReplicaFieldAvailable = true;
		InstalledCoverage=std::move(Coverage);
		NotifyPropertyCommitted("BlockRevision");
	}
	void BlockGrid::RetireReplicaContent() {
		AssertCanMutate();
		if (!IsReplicaMaterialization()) throw std::logic_error("[BlockGrid:Replica] Expected a replica");
		if (ReplicaFieldAvailable) {
			Cells.InstallReplica(blockgrid::Field{}, Admission);
			LastAssetService.reset();
			LastCellEdit.reset();
			AssetsReady = false;
			ReplicaFieldAvailable = false;
			InstalledCoverage.reset();
			NotifyPropertyCommitted("BlockRevision");
		}
		ClearReplicaAssemblyProjection();
	}
	std::string BlockGrid::GetBlockRevision() const { return RevisionToken(Cells.GetRevision()); }
	std::string BlockGrid::GetPublicBlockRevision() const { return RevisionToken(Cells.GetPublicRevision())+"/peers"; }
	std::string BlockGrid::RevisionToken(std::uint64_t SourceRevision) const {
		const auto Scope=GetReplicationScopeId(), Id=GetObjectId();
		const auto World=WorldRoot::GetEffectiveWorld(*this);
		const auto Domain=World?World->GetObjectId():ObjectId{};
		return std::format("{}.{}:{}.{}:{}.{}:{}:{}:{}",Scope.Slot,Scope.Generation,Id.Slot,Id.Generation,Domain.Slot,Domain.Generation,
			World ? World->GetSpatialActivation() : 0,ConfigurationRevision,SourceRevision);
	}
	std::string BlockGrid::ApplyEdit(const blockgrid::EditRequest& Edit,const blockgrid::Patch* Prepared) {
		AssertCanMutate();
		const auto World=GetDataModel();
		if (!World || Editing || IsTransferParked()) throw std::logic_error("[BlockGrid:Edit] Grid is unavailable for admission");
		if (const auto Protection=World->GetSpatialTransferProtection(); Protection && !Protection->AllowsResource(*this))
			throw std::runtime_error("[BlockGrid:Edit] Collision is reserved for an active transfer");
		if (Edit.ExpectedRevision && *Edit.ExpectedRevision!=GetBlockRevision()) throw std::domain_error("[BlockGrid:Conflict] Revision changed");
		if (Edit.Motion) {
			CheckConfigurationWrite();
			World->EnsureAuthoritativeRevisionAvailable();
			const auto Domain=WorldRoot::GetEffectiveWorld(*this);
			if (!Domain || !EnsureAssetBindings()) throw std::runtime_error("[BlockGrid:Motion] Construction has no ready simulation world");
			bool Changed;
			if (const auto& V=Edit.Motion->Velocities) {
				const auto Vector=[](const Point& P){return glm::vec3(P[0],P[1],P[2]);};
				Changed=Domain->SetGridVelocity(*this,Vector(V->first),Vector(V->second));
			} else Changed=Domain->SetGridMotion(*this,Edit.Motion->Dynamic);
			if (Changed) {
				World->AdvanceAuthoritativeRevision();
				if (Edit.Motion->Velocities) { ++ConfigurationRevision; NotifyPropertyCommitted("BlockRevision"); }
				else NotifyConfiguration("MotionMode");
			}
			return GetBlockRevision();
		}
		if (!Edit.Binding && !(Edit.Replay && (Edit.Replay->Binding || !Edit.Replay->Materials.empty())) && !EnsureAssetBindings())
			throw std::runtime_error("[BlockGrid:Unavailable] Required block assets do not match this project");
		World->EnsureAuthoritativeRevisionAvailable();
		std::optional<blockgrid::CellDelta> PointEdit;
		const auto PriorGeometry = Cells.GetGeometryRevision();
		if (!Edit.Binding && !Edit.Channel && !Edit.Replay && Edit.Commands.empty() && Edit.Writes.size() == 1) {
			const auto &Write = Edit.Writes.front();
			if (blockgrid::ValidCell(Write.Bounds.Min) &&
				blockgrid::SingleCell(Write.Bounds.Min).MaxExclusive == Write.Bounds.MaxExclusive)
				PointEdit = blockgrid::CellDelta{Write.Bounds.Min, Cells.Get(Write.Bounds.Min), Write.Value};
		}
		bool Changed;
		{
			struct Guard { bool& Flag; ~Guard(){ Flag=false; } } Reset{Editing};
			Editing=true;
			if (Edit.Replay && !Edit.Replay->Materials.empty()) {
				const auto& P=*Edit.Replay;
				if (Edit.Forward) {
					auto Materials=P.Materials;
					for (auto& D:Materials) D=ResolveAssetDefinition(D);
					Changed=Cells.ApplyMaterialChanges(Materials,P.StateCells,false,Admission);
				} else {
					auto Undo=P.StateCells; for (auto& D:Undo) std::swap(D.Before,D.After);
					Changed=Cells.ApplyMaterialChanges(P.Materials,Undo,true,Admission);
				}
			} else if (!Edit.Materials.empty()) {
				const auto Owned=Prepared?nullptr:PlanMaterialEdit(Edit);
				const auto& P=Prepared?*Prepared:*Owned;
				Changed=Cells.ApplyMaterialChanges(P.Materials,P.StateCells,false,Admission);
			} else if (Edit.Replay && Edit.Replay->Binding) {
				const auto& D=*Edit.Replay->Binding;
				const bool Forward=Edit.Forward;
				const auto Id=Cells.FindDefinitionId(D.After.Key);
				const auto* Before=Forward?(D.Before?&*D.Before:nullptr):&D.After;
				if (bool(Id)!=bool(Before) || (Id && Cells.GetDefinition(*Id)!=*Before))
					throw std::domain_error("[BlockGrid:Conflict] Definition history has diverged");
				if (Forward || D.Before) Changed=Cells.Define(ResolveAssetDefinition(Forward?D.After:*D.Before),bool(Id),Admission);
				else { Cells.UndoDefinitionInsertion(D.After,Admission); Changed=true; }
			} else if (Edit.Replay && Edit.Replay->Channel) {
				if (Edit.Forward) {
					for (std::uint8_t I=0;I<Cells.GetChannelCount();++I)
						if (Cells.GetChannel(I).Spec.Name==Edit.Replay->Channel->Name) throw std::domain_error("[BlockGrid:Conflict] Channel already exists");
					Changed=Cells.DefineChannel(*Edit.Replay->Channel);
				} else { Cells.UndoChannelInsertion(*Edit.Replay->Channel); Changed=true; }
			} else if (Edit.Replay && !Edit.Replay->StateCells.empty()) {
				if (Edit.Forward) Changed=Cells.ApplyChanges(Edit.Replay->StateCells,Admission);
				else {
					auto Undo=Edit.Replay->StateCells;
					for (auto& D:Undo) std::swap(D.Before,D.After);
					Changed=Cells.ApplyChanges(Undo,Admission);
				}
			} else if (Edit.Replay) Changed=Cells.ApplyCellPatch(blockgrid::ReplayWrites(Cells,*Edit.Replay,Edit.Forward),Admission);
			else if (Edit.Binding) Changed=Cells.Define(ResolveAssetDefinition(Edit.Binding->Spec),Edit.Binding->Replace,Admission);
			else if (Edit.Channel) Changed=Cells.DefineChannel(*Edit.Channel);
			else if (!Edit.Commands.empty()) Changed=Cells.ApplyOperations(Edit.Commands,Admission);
			else Changed=Cells.Apply(Edit.Writes,{},Admission);
		}
		const auto Revision=GetBlockRevision();
		if (Changed) {
			if (PriorGeometry != Cells.GetGeometryRevision()) LastCellEdit = PointEdit;
			if (Edit.Binding || !Edit.Materials.empty() || (Edit.Replay && (Edit.Replay->Binding || !Edit.Replay->Materials.empty()))) AssetSequence=0;
			if (DynamicAssembly) if (auto Domain=WorldRoot::GetEffectiveWorld(*this)) Domain->RefreshGridAssembly(*this);
			World->AdvanceAuthoritativeRevision();
			NotifyPropertyCommitted("BlockRevision");
		}
		return Revision;
	}
	void BlockGrid::CheckConfigurationWrite() const {
		AssertCanMutate();
		if (Editing || IsTransferParked() || GetCurrentScriptSecurityContext().Domain==ScriptExecutionDomain::Client)
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
		if ((!IsReplicaMaterialization() && Cells.GetOccupiedCount()) || (Value!=0.5f && Value!=1.f && Value!=2.f))
			throw std::invalid_argument("[BlockGrid:CellSize] Choose 0.5, 1, or 2 while the grid is empty");
		if (ReplicaFieldAvailable) RetireReplicaContent();
		Size=Value; NotifyConfiguration("CellSize");
	}
	CFrame BlockGrid::GetCFrame() const { return Transform; }
	void BlockGrid::SetCFrame(gargantuan::CFrame Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CFrame",Value);
		if (!ValidTransform(Value)) throw std::invalid_argument("[BlockGrid:CFrame] Expected an in-range proper rigid transform");
		// A verified pose receipt owns this replica until it is retired. A delayed shell property cannot rewind a moving grid.
		if (IsReplicaMaterialization() && AssemblyState) return;
		if (Value.Position==Transform.Position && Value.Rotation==Transform.Rotation) return;
		if (DynamicAssembly) {
			const auto Domain=WorldRoot::GetEffectiveWorld(*this);
			if (!Domain) throw std::runtime_error("[BlockGrid:Motion] Assembly has no simulation world");
			Domain->SetGridPose(*this,Value);
		}
		Transform=Value;
		if (!DynamicAssembly && !IsReplicaMaterialization()) ++AssemblyRevision;
		NotifyConfiguration("CFrame");
	}
	bool BlockGrid::GetVisible() const { return IsVisible; }
	void BlockGrid::SetVisible(bool Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("Visible",Value);
		if (IsVisible!=Value) { IsVisible=Value; NotifyConfiguration("Visible"); }
	}
	bool BlockGrid::GetCanCollide() const { return Collides; }
	void BlockGrid::SetCanCollide(bool Value) {
		CheckConfigurationWrite(); ValidatePropertyMutation("CanCollide",Value);
		if (DynamicAssembly && !Value) throw std::domain_error("[BlockGrid:Motion] Freeze the assembly before disabling collision");
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
		return Place(L,Grid,SingleCell(ReadCell(L,2)),3,4);
	}
	int BlockGrid::RemoveBlock(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{SingleCell(ReadCell(L,2)),{}});
	}
	int BlockGrid::GetBlock(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const auto P=ReadCell(L,2);Grid.RequireReplicaCoverage(SingleCell(P));
		PushBlock(L,Grid.Cells,Grid.Cells.Get(P)); return 1;
	}
	int BlockGrid::Fill(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Place(L,Grid,{ReadCell(L,2),ReadCell(L,3,true)},4,5);
	}
	int BlockGrid::Clear(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		return Submit(L,Grid,{{ReadCell(L,2),ReadCell(L,3,true)},{}});
	}
	int BlockGrid::ReadRegion(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object);
		const Region R{ReadCell(L,2),ReadCell(L,3,true)};
		(void)Volume(R,MaximumReadCells);
		Grid.RequireReplicaCoverage(R);
		const auto Select=ReadFilter(L,4,Grid.Cells);
		lua_createtable(L,0,2);
		PushText(L,Grid.GetBlockRevision()); lua_setfield(L,-2,"Revision");
		lua_newtable(L);
		int N=0;
		for (int Z=R.Min.Z; Z<R.MaxExclusive.Z; ++Z) for (int Y=R.Min.Y; Y<R.MaxExclusive.Y; ++Y)
			for (int X=R.Min.X; X<R.MaxExclusive.X; ++X) {
				const auto B=Grid.Cells.Get({X,Y,Z});
				if (!Matches(Grid.Cells,Select,{X,Y,Z},B)) continue;
				lua_createtable(L,0,2);
				StackValue<glm::vec3>::Push(L,glm::vec3(X,Y,Z)); lua_setfield(L,-2,"Position");
				PushBlock(L,Grid.Cells,B); lua_setfield(L,-2,"Block"); lua_rawseti(L,-2,++N);
			}
		lua_setfield(L,-2,"Cells"); return 1;
	}
	int BlockGrid::IsRegionAvailable(lua_State* L,Instance* Object) {
		const auto& Grid=ReadTarget(L,Object,false,true);
		const Region R{ReadCell(L,2),ReadCell(L,3,true)};
		if (R.Min.X>=R.MaxExclusive.X || R.Min.Y>=R.MaxExclusive.Y || R.Min.Z>=R.MaxExclusive.Z)
			throw std::invalid_argument("[BlockGrid:Bounds] Expected a nonempty half-open region");
		const bool NeedsReplica=Grid.IsReplicaMaterialization() || ScriptEngine::Get(L)->Mode==RuntimeMode::NetworkClient;
		const bool Available=!NeedsReplica || (Grid.ReplicaFieldAvailable && (!Grid.InstalledCoverage || Grid.InstalledCoverage->Contains(R)));
		lua_pushboolean(L,Available);return 1;
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
		PushBlock(L,Cells,B); lua_setfield(L,-2,"Block");
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
		if (!Grid.EnsureAssetBindings()) throw std::runtime_error("[BlockGrid:Unavailable] Required block assets are unavailable");
		const auto R=glm::dmat3(Grid.Transform.Rotation);
		const auto Local=glm::transpose(R)*(O-glm::dvec3(Grid.Transform.Position))/static_cast<double>(Grid.Size);
		const auto Delta=glm::transpose(R)*D/static_cast<double>(Grid.Size);
		const auto Hit=blockgrid::Cast(Grid.Cells,{Local.x,Local.y,Local.z},{Delta.x,Delta.y,Delta.z},
			blockgrid::MaximumRayVisits,Grid.InstalledCoverage.get());
		if (!Hit) { lua_pushnil(L); return 1; }
		const auto& N=Hit->Normal;
		Grid.PushGridHit(L,Hit->Position,glm::vec3(O+D*Hit->Fraction),glm::vec3(glm::normalize(R*glm::dvec3(N[0],N[1],N[2]))));
		return 1;
	}
	int BlockGrid::DefineBlock(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		auto Edit=std::make_shared<EditRequest>();
		Edit->Binding=BindingEdit{ReadDefinition(L),false};
		return SubmitRequest(L,Grid,std::move(Edit));
	}
	int BlockGrid::ReplaceDefinition(lua_State* L,Instance* Object) {
		auto& Grid=ReadTarget(L,Object,true);
		auto Edit=std::make_shared<EditRequest>();
		Edit->Binding=BindingEdit{ReadDefinition(L),true};
		Edit->ExpectedRevision=std::string(StringAt(L,4));
		return SubmitRequest(L,Grid,std::move(Edit));
	}
	int BlockGrid::GetDefinitionDigest(lua_State* L,Instance* Object) {
		const auto& F=ReadTarget(L,Object).GetField();
		PushText(L,DefinitionDigest(F.GetDefinition(F.GetDefinitionId(StringAt(L,2)))));
		return 1;
	}
}
