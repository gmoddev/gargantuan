// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/blockgrid/Assets.hpp"
#include "../../src/network/BlockGridPeer.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include <luacode.h>
#include <fstream>
#include <format>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	void Check(bool Value,const char* Message) { if (!Value) throw std::runtime_error(Message); }
	void Run(const std::shared_ptr<DataModel>& World, RuntimeMode Mode, ScriptSecurityContext Context, std::string_view Text) {
		ScriptEngine Engine(World,{},Mode);
		ScriptSecurityScope Security(std::move(Context));
		std::size_t Size=0;
		auto* Code=luau_compile(Text.data(),Text.size(),&Engine.CompileOptions,&Size);
		Check(Code!=nullptr,"compile failed");
		auto Status=luau_load(Engine.L,"=BlockGridNative",Code,Size,0);
		std::free(Code);
		if (!Status) {
			lua_pushcfunction(Engine.L,[](lua_State *L) {
				const char *Message=lua_tostring(L,1);
				const char *Trace=lua_debugtrace(L);
				lua_pushfstring(L,"%s\n%s",Message?Message:"VM error",Trace?Trace:"");
				return 1;
			},"BlockGridTestTrace");
			lua_insert(Engine.L,-2);
			Status=lua_pcall(Engine.L,0,0,lua_gettop(Engine.L)-1);
		}
		if (Status) throw std::runtime_error(lua_tostring(Engine.L,-1));
	}
	void TestAssetDefinitions() {
		const auto Root=std::filesystem::path(GARGANTUAN_BLOCKGRID_EXAMPLE).parent_path().parent_path()/"fixtures";
		DiskFilesystem Files(Root);
		SourceMount Mount(Files);
		auto World=std::make_shared<DataModel>();
		const auto Service=std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		const auto Tile=Service->ImportProjectAsset(Mount,"SlopedTile.gltf",AssetKind::Mesh);
		if (!Tile.Ok || !Tile.Record) throw std::runtime_error("Sloped tile import: "+Tile.Diagnostic.Message);
		const auto Oversized=Service->ImportProjectAsset(Mount,"OversizedTile.obj",AssetKind::Mesh);
		Check(Oversized.Ok && Oversized.Record,"oversized fixture still must import as an ordinary Mesh");
		const auto Visual=Service->ResolveMesh(Tile.Record->Reference.Value);
		Check(Visual && Visual->CollisionAsset && Visual->Primitives && Visual->Primitives->size()==2 &&
			Visual->Primitives->front().Material.has_value(),"fixture companion and two material slots");
		const auto Material=*Visual->Primitives->front().Material;
		const auto Script=std::format("local Mesh='{}'\nlocal Collider='{}'\nlocal Mat='{}'\nlocal Accent='{}'\nlocal Large='{}'\n",
			Tile.Record->Id.ToString(),Visual->CollisionAsset->ToString(),Material.ToString(),Visual->Primitives->back().Material->ToString(),
			Oversized.Record->Id.ToString())+R"(
			local G=Instance.new("BlockGrid")
			G.Name="AssetTiles"
			G.Parent=game.Workspace
			G:DefineBlock("project:tile",{Shape="Mesh",Mesh=Mesh,Collision="Asset",CollisionAsset=Collider})
			G:DefineBlock("project:ghost",{Shape="Mesh",Mesh=Mesh,Collision="None"})
			G:DefineBlock("project:tinted",{Shape="Slab",Material="asset://"..Mat})
			local A=Vector3.new(6,0,0)
			local Alias,Other="material:"..Mat,"material:"..Accent
			local Original=G:GetRevision()
			assert(not pcall(function() G:Fill(A,A,Alias) end))
			assert(G:GetRevision()==Original and not pcall(function() G:GetDefinitionDigest(Alias) end))
			local None=G:BeginEdit()
			None:SetBlock(A,Other)
			None:RemoveBlock(A)
			assert(None:Commit()==Original and not pcall(function() G:GetDefinitionDigest(Other) end))
			local First=G:SetBlock(A,Alias)
			assert(First~=Original and G:GetBlock(A).Definition==Alias and G:SetBlock(A,Alias)==First)
			local Destination=Instance.new("BlockGrid")
			Destination.Parent=game.Workspace
			local Max=A+Vector3.new(1,1,1)
			local Buffer=G:ReadPacked(A,Max)
			local Bulk=Destination:BeginEdit()
			Bulk:SetBlock(A,Other)
			Bulk:WritePacked(A,Max,Buffer)
			Bulk:Commit()
			assert(Destination:GetBlock(A).Definition==Alias and not pcall(function() Destination:GetDefinitionDigest(Other) end))
			G:RemoveBlock(A)
			local Rev=G:GetRevision()
			for _, Bad in {{Shape="Mesh",Mesh=Mesh},{Shape="Mesh",Mesh=Large,Collision="None"},
				{Shape="Mesh",Mesh=Mat,Collision="None"},{Shape="Mesh",Mesh=Mesh,Collision="Asset",CollisionAsset=Mat},
				{Shape="Cube",Material=Collider}} do
				assert(not pcall(function() G:DefineBlock("project:invalid",Bad) end) and G:GetRevision()==Rev)
			end
			G:SetBlock(Vector3.zero,"project:tile")
			G:SetBlock(Vector3.new(2,0,0),"project:ghost")
			local Params=RaycastParams.new()
			Params.IncludeBlockGrids=true
			local D=Vector3.new(0,-3,0)
			for _,X in {0,2} do
				local O=Vector3.new(X+.25,2,.5)
				local H=G:Raycast(O,D)
				local W=game.Workspace:Raycast(O,D,Params)
				assert(H and W and W.GridHit and math.abs(H.Position.Y-.375)<.001)
				assert(W.GridHit.Cell==H.Cell and math.abs(H.Position.Y-W.Position.Y)<.02)
				local Gap=Vector3.new(X+.65,2,.2)
				assert(G:Raycast(Gap,D)==nil and game.Workspace:Raycast(Gap,D,Params)==nil)
			end
		)";
		Run(World,RuntimeMode::Offline,ScriptSecurityContext::ServerRuntime(),Script);
		const auto Grid=std::dynamic_pointer_cast<BlockGrid>(World->GetService("Workspace")->FindFirstChild("AssetTiles",false));
		Check(Grid && Grid->EnsureAssetBindings(),"asset grid bound to trusted catalog");
		const auto& F=Grid->GetField();
		const auto& Binding=*F.GetDefinition(F.GetDefinitionId("project:tile")).Assets;
		const auto& D=*Binding.Resolved;
		auto Shared=std::make_shared<blockgrid::SharedAssetPins>();
		for (std::size_t I=0; I<F.GetDefinitionCount(); ++I) {
			const auto &Assets=F.GetDefinition(static_cast<std::uint16_t>(blockgrid::Brick+1+I)).Assets;
			if (Assets) for (const auto &Pin:Assets->Pins) Shared->emplace(Pin.Id,Pin.Digest);
		}
		auto FirstFrame=[&](std::shared_ptr<const blockgrid::SharedAssetPins> Pins) {
			using namespace network;
			BlockGridPeer Peer(true,31,{},std::move(Pins));
			const auto Id=Grid->GetObjectId();
			auto Find=[&](ObjectId Source) { return Source==Id?Grid:nullptr; };
			Peer.Receive({.Kind=GridStreamKind::Request,.Scope={31,Id,{}},.Subscription=1,.Serial=1},Find,1);
			std::vector<std::byte> Packet;
			Peer.Pump(Find,[&](const GridStreamMessage &M){ Packet=EncodeGridStream(M); return true; },2);
			return Packet;
		};
		auto Admit=network::DecodeGridStream(FirstFrame(Shared));
		Check(Admit && Admit->Kind==network::GridStreamKind::Offer,"explicit transitive pins did not authorize the native source");
		const auto Unshared=FirstFrame({});
		Shared->erase(Binding.Pins.back().Id);
		Check(FirstFrame(Shared)==Unshared,"partial allowlist disclosed an asset dependency");
		(*Shared)[Binding.Pins.back().Id]=std::string(64,'a');
		Check(FirstFrame(Shared)==Unshared,"stale allowed dependency digest authorized content");
		const auto Closed=network::DecodeGridStream(Unshared);
		Check(Closed && Closed->Kind==network::GridStreamKind::Unavailable && Unshared.size()<64,
			"unavailable shell leaked catalog metadata");
		auto Space=std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		RenderPublisher Publisher;
		auto Textures=Service->DrainTextureChanges();
		Publisher.SetUiTextureChanges(std::move(Textures.Creates),std::move(Textures.Updates),std::move(Textures.Removes));
		const auto Camera=MakeLookAtRenderCameraInput({4,3,6},{1,0,0},{0,1,0},65);
		const auto Packet=Publisher.Publish(*Space,Camera,256,144);
		const auto Object=std::ranges::find_if(Packet->Creates,[&](const auto &Created){return Created.Item.Object==Grid->GetObjectId();});
		Check(Object!=Packet->Creates.end() && Object->Mesh && Object->Primitives && Object->Primitives->size()==4,
			"both placed tiles publish two contiguous material slots each");
		const auto Surface=std::ranges::find_if(Packet->MeshCreates,[&](const auto &Mesh){return Mesh.Mesh==*Object->Mesh;});
		Check(Surface!=Packet->MeshCreates.end() && D.VisualMesh && Surface->Vertices->size()==2*D.VisualMesh->Vertices->size() &&
			Surface->Indices->size()==2*D.VisualMesh->Indices->size(),"published triangles preserve imported mesh topology");
		for (std::size_t I=0; I<Object->Primitives->size(); ++I) {
			const auto &Source=(*D.Primitives)[I%2];
			const auto &Range=(*Object->Primitives)[I];
			Check(Range.FirstIndex==(I/2)*D.VisualMesh->Indices->size()+Source.FirstIndex &&
				Range.IndexCount==Source.IndexCount && Range.Material==Source.Material,"texture, opacity and emission survive range rebasing");
		}
		for (std::size_t I=0; I<D.VisualMesh->Vertices->size(); ++I) {
			const auto &Source=(*D.VisualMesh->Vertices)[I];
			const auto &Vertex=(*Surface->Vertices)[I];
			Check(glm::length(Vertex.Position-(glm::vec3(.5f)+Source.Position))<1e-5f &&
				Vertex.TextureCoordinate==Source.TextureCoordinate && Vertex.Normal==Source.Normal &&
				Vertex.Tangent==Source.Tangent,"mesh center, UV, normal and tangent survive grid publication");
		}
		Check(Binding.Pins.size()>=5 && D.VisualMesh && D.QueryMesh && !D.FullyOpaque && D.Primitives && D.Primitives->size()==2,
			"whole dependency closure and immutable material slots");
		Check(D.Primitives->front().Material.BaseColorTexture.has_value() && D.Primitives->back().Material.OpacityMode==RenderOpacityMode::Transparent &&
			D.Primitives->back().Material.EmissiveFactor.x>.7f,"material textures, transparency and emissive inputs");
		auto Authored=std::make_shared<BlockGrid>();
		Authored->SetParent(World->GetService("Workspace"));
		const auto Key="material:"+Material.ToString();
		auto Input=std::make_shared<blockgrid::EditRequest>();
		Input->ExpectedRevision=Authored->GetBlockRevision();
		Input->Materials.push_back(Authored->ResolveMaterialAlias(Key));
		Input->Writes.push_back({blockgrid::SingleCell({0,0,0}),{3,0}});
		const SubmitBlockGridEditCommand Cmd{Authored->GetObjectId(),World->GetObjectId(),Input};
		const auto Security=ScriptSecurityContext::StudioCoreUi();
		MutationGateway Gateway;
		Authored->SetAdmission([](const auto&,auto){return false;});
		Check(!Gateway.Apply(Cmd,MutationAuthorityContext::Studio(Security,World->GetObjectId(),{},7)).Succeeded() &&
			!Authored->GetField().FindDefinitionId(Key),"failed admission leaked a material alias");
		Authored->SetAdmission({});
		const auto Rev=Authored->GetField().GetRevision();
		Check(Gateway.Apply(Cmd,MutationAuthorityContext::Studio(Security,World->GetObjectId(),{},7)).Succeeded() &&
			Authored->GetField().GetRevision()==Rev+1,"first-use material must be a single authoring edit");
		Check(Gateway.Undo(*World,Security).Succeeded() && !Authored->GetField().FindDefinitionId(Key) &&
			!Authored->GetField().GetOccupiedCount(),"undo must remove the placement and its implicit binding");
		Check(Gateway.Redo(*World,Security).Succeeded() && Authored->GetField().Get({0,0,0}).Definition==3,
			"redo must restore the same material placement");
		const auto Bytes=blockgrid::Encode(F);
		const auto SavedAssets=Service->CaptureProjectAssets();
		auto Reload=std::make_shared<DataModel>();
		const auto NewService=std::dynamic_pointer_cast<AssetService>(Reload->GetService("AssetService"));
		auto Copy=std::make_shared<BlockGrid>();
		Copy->RestoreField(blockgrid::Decode(Bytes));
		Copy->SetParent(Reload->GetService("Workspace"));
		Check(!Copy->EnsureAssetBindings(),"missing project assets block publication");
		NewService->LoadProjectAssetSnapshot(SavedAssets);
		Check(Copy->EnsureAssetBindings() && blockgrid::Encode(Copy->GetField())==Bytes,"project reload must verify every content pin");
		const auto Before=Grid->GetBlockRevision();
		Check(Service->DeleteProjectAsset(Tile.Record->Reference.Value).Ok && !Grid->EnsureAssetBindings() && Grid->GetBlockRevision()==Before,
			"retired project resources invalidate publication without changing authored data");
		const auto Retired=Publisher.Publish(*Space,Camera,256,144);
		Check(std::ranges::any_of(Retired->Removes,[&](const auto &Removal){return Removal.Object==Grid->GetObjectId();}),
			"deleting a required asset retires the published grid resource");
	}
	void TestGridTransfers() {
		auto World=std::make_shared<DataModel>();
		const auto Server=ScriptSecurityContext::ServerRuntime();
		Run(World,RuntimeMode::Offline,Server,R"(
			local Room=Instance.new("SpatialRegion")
			Room.Parent=game.Workspace
			local S=Instance.new("BlockGrid")
			S.Parent=game.Workspace
			S.SpatialRegion=Room
			S.CellSize=.5
			S.CFrame=CFrame.new(8,2,-3)*CFrame.Angles(0,math.pi/2,0)
			S:DefineBlock("project:transfer",{Shape="Stairs"})
			S:DefineChannel("private",{Kind="UInt8",Default=3,Indexed=true,OnCopy="Default"})
			local P=Vector3.new(-3,2,1)
			S:SetBlock(P,"project:transfer",{Up="+Y",Front="+X"})
			S:SetBlock(Vector3.zero,"Stone")
			local Set=S:BeginEdit()
			Set:SetValue(P,"private",118)
			Set:Commit()
			local WorldPoint=S:CellToWorld(P)
			local Before=S:GetRevision()
			local D=S:ExtractRegion(P,P+Vector3.one,Before)
			assert(D~=S and D.Parent==S.Parent and D.SpatialRegion==Room and D.MotionMode=="Static" and D.CellSize==.5)
			assert(S:GetRevision()~=Before and S:GetBlock(P)==nil and S:GetBlock(Vector3.zero).Definition=="Stone")
			assert(D:GetBlock(Vector3.zero).Orientation.Front=="+X" and D:GetValue(Vector3.zero,"private")==118)
			assert((D:CellToWorld(Vector3.zero)-WorldPoint).Magnitude<.0001)
			local F=D:FindActive("private",{Min=Vector3.zero,MaxExclusive=Vector3.one}):Step(8)
			assert(#F.Cells==1 and F.Cells[1]==Vector3.zero)
			local A,B=S:GetRevision(),D:GetRevision()
			assert(not pcall(function() S:MergeFrom(D,A,A) end) and S:GetRevision()==A and D:GetRevision()==B)
			S:SetBlock(P,"Brick")
			A=S:GetRevision()
			assert(not pcall(function() S:MergeFrom(D,A,B) end) and S:GetRevision()==A and D:GetRevision()==B)
			S:RemoveBlock(P)
			D.CFrame=D.CFrame*CFrame.new(.125,0,0)
			assert(not pcall(function() S:MergeFrom(D,S:GetRevision(),D:GetRevision()) end))
			D.CFrame=D.CFrame*CFrame.new(-.125,0,0)*CFrame.Angles(0,math.pi/2,0)
			local Q=S:WorldToCell(D:CellToWorld(Vector3.zero))
			local M=S:MergeFrom(D,S:GetRevision(),D:GetRevision())
			assert(M==S:GetRevision() and S:GetBlock(Q).Definition=="project:transfer" and S:GetValue(Q,"private")==118)
			assert(D.Parent==S.Parent and D:GetBlock(Vector3.zero)==nil)
			S:SetMotionMode("Dynamic",S:GetRevision())
			local Token=S:GetRevision()
			assert(not pcall(function() S:ExtractRegion(Q,Q+Vector3.one,Token) end) and Token==S:GetRevision())
			S:SetMotionMode("Static",Token)
		)");

		const auto Parent=World->GetService("Workspace");
		auto Source=std::make_shared<BlockGrid>(); Source->SetParent(Parent);
		MutationGateway Gateway;
		const auto Studio=ScriptSecurityContext::StudioCoreUi();
		const auto Auth=MutationAuthorityContext::Studio(Studio,World->GetObjectId(),{},41);
		auto Fill=std::make_shared<blockgrid::EditRequest>();
		Fill->Writes={{{{0,0,0},{2,1,1}},{blockgrid::Stone,0}}};
		auto Command=[&](auto E){return SubmitBlockGridEditCommand{Source->GetObjectId(),World->GetObjectId(),std::move(E)};};
		Check(Gateway.Apply(Command(Fill),Server).Succeeded(),"prepare extraction source");
		auto Extract=std::make_shared<blockgrid::EditRequest>();
		Extract->Bilateral=blockgrid::GridTransferEdit{blockgrid::SingleCell({1,0,0})}; Extract->ExpectedRevision=Source->GetBlockRevision();
		const auto Children=Parent->Children.size(),Owned=World->GetOwnedInstanceCount(),Start=World->GetAuthoritativeRevision();
		unsigned Added=0;
		auto Watch=Parent->ChildAdded->Connect([&](const auto& Child) {
			const auto G=std::dynamic_pointer_cast<BlockGrid>(Child);
			if (G && G!=Source && Source->GetField().GetOccupiedCount()==1 && G->GetField().GetOccupiedCount()==1) ++Added;
		});
		const auto Capacity=ChangeJournal::Get().GetCapacity();
		ChangeJournal::Get().SetCapacity(1);
		const auto Denied=Gateway.Apply(Command(Extract),Auth);
		ChangeJournal::Get().SetCapacity(Capacity);
		Check(!Denied.Succeeded() && World->GetOwnedInstanceCount()==Owned && Parent->Children.size()==Children && !Added &&
			World->GetAuthoritativeRevision()==Start && Source->GetBlockRevision()==*Extract->ExpectedRevision &&
			!World->Transactions.GetStatus().CanUndo,"journal rejection leaked hierarchy, history, revisions, notification or coverage");
		const auto Applied=Gateway.Apply(Command(Extract),Auth);
		Check(Applied.Succeeded() && Applied.Object && Added==1 && World->GetAuthoritativeRevision()==Start+1,"prepared Studio extraction");
		auto D=std::dynamic_pointer_cast<BlockGrid>(ObjectRegistry::Get().Lookup(*Applied.Object));
		const auto Stamp=Source->GetLastTransfer();
		Check(Stamp && D->GetLastTransfer()==Stamp && Stamp->Grids[0]==Source->GetObjectId() && Stamp->Grids[1]==D->GetObjectId() &&
			Stamp->PublicRevisions[0]==Source->GetPublicBlockRevision() && Stamp->PublicRevisions[1]==D->GetPublicBlockRevision(),
			"both public views must identify their shared committed transfer");
		const auto After=Source->GetBlockRevision(),Other=D->GetBlockRevision();
		auto Dependent=std::make_shared<BlockGrid>(); Dependent->SetParent(D);
		Check(!Gateway.Undo(*World,Studio).Succeeded() && Source->GetBlockRevision()==After && D->GetBlockRevision()==Other,
			"dependent extraction inverse published a partial edit");
		Dependent->Destroy();
		Check(Gateway.Undo(*World,Studio).Succeeded() && Source->GetField().GetOccupiedCount()==2 && !D->GetParent() &&
			D->IsTransferParked() && !D->GetField().GetOccupiedCount(),"Undo extraction must return both cells and park its empty shell");
		Check(!Gateway.Apply(SubmitBlockGridEditCommand{D->GetObjectId(),World->GetObjectId(),Fill},Server).Succeeded(),
			"parked shell received gameplay writes");
		Check(Gateway.Redo(*World,Studio).Succeeded() && D->GetParent().value_or(nullptr)==Parent && !D->IsTransferParked() &&
			Added==2 && Source->GetBlockRevision()!=After && D->GetBlockRevision()!=Other,"Redo preserves identity and advances both revisions");
		auto Merge=std::make_shared<blockgrid::EditRequest>();
		Merge->ExpectedRevision=Source->GetBlockRevision(); Merge->Bilateral=blockgrid::GridTransferEdit{{},D->GetObjectId(),D->GetBlockRevision()};
		Check(Gateway.Apply(Command(Merge),Auth).Succeeded() && !D->GetField().GetOccupiedCount(),"Studio merge");
		Check(Gateway.Undo(*World,Studio).Succeeded() && D->GetField().GetOccupiedCount()==1 && Source->GetField().GetOccupiedCount()==1,"undo merge");
		Check(Gateway.Redo(*World,Studio).Succeeded() && D->GetField().GetOccupiedCount()==0 && Source->GetField().GetOccupiedCount()==2,"redo merge");
	}
	void TestMovingGrid() {
		auto World=std::make_shared<DataModel>();
		const auto Server=ScriptSecurityContext::ServerRuntime();
		Run(World,RuntimeMode::Offline,Server,R"(
			local Room=Instance.new("SpatialRegion")
			Room.Name="Hangar"
			Room.Parent=game.Workspace
			local Floor=Instance.new("Part")
			Floor.Anchored=true
			Floor.Size=Vector3.new(64,1,64)
			Floor.CFrame=CFrame.new(0,-.5,0)
			Floor.Parent=Room
			local G=Instance.new("BlockGrid")
			G.Name="Carriage"
			G.Parent=Room
			G.CFrame=CFrame.new(0,4,0)
			local First=G:GetRevision()
			assert(not pcall(function() G:SetMotionMode("Dynamic",First) end) and G:GetRevision()==First)
			G:SetBlock(Vector3.zero,"Stone")
			G:SetBlock(Vector3.new(2,0,0),"Brick")
			local Revision=G:GetRevision()
			assert(not pcall(function() G:SetMotionMode("Dynamic",First) end))
			local Moving=G:SetMotionMode("Dynamic",Revision)
			local S=G:GetAssemblyState()
			assert(Moving~=Revision and G.MotionMode=="Dynamic" and math.abs(S.Mass-2)<.001)
			assert((S.LocalCenterOfMass-Vector3.new(1.5,.5,.5)).Magnitude<.001)
			assert(S.LocalInertia[1].X>0 and S.LocalInertia[2].Y>S.LocalInertia[1].X)
			local Declined=G:GetRevision()
			assert(not pcall(function() G:Fill(Vector3.zero,Vector3.new(129,1,1),"Stone") end))
			assert(not pcall(function() G.CanCollide=false end))
			assert(not pcall(function() G.Parent=game.Workspace end))
			assert(not pcall(function() G:SetAssemblyVelocity(Vector3.new(1000,0,0),Vector3.zero,Declined) end))
			assert(G:GetRevision()==Declined and G.Parent==Room and G:GetAssemblyState().Mass==2)
			G:SetAssemblyVelocity(Vector3.new(1,0,0),Vector3.zero,Declined)
			assert(G:GetAssemblyState().LinearVelocity.X>.99)
		)");
		auto Room=std::dynamic_pointer_cast<WorldRoot>(World->GetService("Workspace")->FindFirstChild("Hangar",false));
		auto Grid=std::dynamic_pointer_cast<BlockGrid>(Room->FindFirstChild("Carriage",false));
		auto Prior=Grid->GetAssemblyProjection();
		Check(Prior && blockgrid::ValidAssemblyProjection(*Prior),"invalid initial motion receipt");
		const std::array Required{Grid->GetObjectId()};
		const auto Receipt=Room->PrepareSpatialDestination({{-1,3,-1},{4,6,2}},Required);
		Check(Receipt.has_value() && !Room->CanHoldSpatialCollision({}),"dynamic collision provided a static reservation");
		const auto Body=std::ranges::find(Receipt->Providers,Grid->GetObjectId(),&SpatialProviderReceipt::Owner);
		Check(Body!=Receipt->Providers.end() && !Body->AuthoritativeEmpty && Body->Shape==PhysicsShapeKind::ConvexAssembly &&
			Body->Installed.size()==2 && Body->Installed[0].Body==Body->Installed[1].Body &&
			Body->Installed[0].Piece!=Body->Installed[1].Piece,"native two-hull receipt lost a child or its owning body");
		const auto Data=blockgrid::Encode(Grid->GetField());
		const auto Cells=Grid->GetField().GetRevision(),Geometry=Grid->GetField().GetGeometryRevision();
		const auto Token=Grid->GetBlockRevision();
		for (int I=0;I<180;++I) Room->StepPhysics(1./60,std::nullopt);
		const auto Rest=Grid->GetAssemblyProjection();
		Check(Rest && Rest->Revision>Prior->Revision && Rest->Pose.Position.x>.03f && std::abs(Rest->Pose.Position.y)<.08f &&
			blockgrid::ValidAssemblyProjection(*Rest),"native motion or floor contact did not reach the model");
		Check(Grid->GetField().GetRevision()==Cells && Grid->GetField().GetGeometryRevision()==Geometry && Grid->GetBlockRevision()==Token &&
			blockgrid::Encode(Grid->GetField())==Data,"movement changed immutable local cells");
		auto Invalid=*Rest; Invalid.Mass=-1; Check(!blockgrid::ValidAssemblyProjection(Invalid),"invalid finite mass accepted");
		bool Denied=false;
		try { (void)Grid->ApplyReplicaAssemblyProjection(*Rest); } catch (const std::exception&) { Denied=true; }
		Check(Denied,"replica motion reached an authoritative grid");
		Run(World,RuntimeMode::Offline,Server,R"(
			local Room=game.Workspace:FindFirstChild("Hangar")
			local G=Room:FindFirstChild("Carriage")
			local Params=RaycastParams.new()
			Params.IncludeBlockGrids=true
			local Center=G:CellToWorld(Vector3.zero)
			local W=Room:Raycast(Center+Vector3.new(0,2,0),Vector3.new(0,-4,0),Params)
			assert(W and W.Instance==G and W.GridHit.Cell==Vector3.zero)
			local Overlap=Room:GetPartsInSphere(Center,.15,Params)
			assert(#Overlap.GridCells==1 and Overlap.GridCells[1].Cell==Vector3.zero)
			G:RemoveBlock(Vector3.new(2,0,0))
			assert(math.abs(G:GetAssemblyState().Mass-1)<.001)
			local Last=G:GetRevision()
			assert(not pcall(function() G:RemoveBlock(Vector3.zero) end) and G:GetRevision()==Last)
			G.CFrame=CFrame.new(1,6,3)
			assert((G:GetAssemblyState().CFrame.Position-G.CFrame.Position).Magnitude<.001)
			G:SetMotionMode("Static",G:GetRevision())
			assert(G.MotionMode=="Static" and G:GetAssemblyState().Mass==0)
			G.Parent=game.Workspace
			G:SetMotionMode("Dynamic",G:GetRevision())
			G.Parent=nil
			assert(G.MotionMode=="Static")
			G:Destroy()
		)");
	}
}
int main() try {
	using namespace gargantuan;
	BootstrapNativeRuntimeSchema();
	TestAssetDefinitions();
	TestMovingGrid();
	TestGridTransfers();
	auto World=std::make_shared<DataModel>();
	std::ifstream Example(GARGANTUAN_BLOCKGRID_EXAMPLE,std::ios::binary);
	Check(Example.good(),"could not load PoweredGate creator example");
	const std::string Source{std::istreambuf_iterator<char>(Example),std::istreambuf_iterator<char>()};
	Run(World,RuntimeMode::Offline,ScriptSecurityContext::ServerRuntime(),Source);
	Run(World,RuntimeMode::Offline,ScriptSecurityContext::ServerRuntime(),R"(
		local G=Instance.new("BlockGrid")
		assert(G.CellSize==1 and G.Visible and G.CanQuery and G.CanCollide)
		G.Name="Construction"
		G.Parent=game.Workspace
		local P=Vector3.new(-1,0,-1)
		local Rev=G:GetRevision()
		local Next=G:Fill(Vector3.new(-3,0,-3),Vector3.new(2,1,2),"Stone")
		assert(Rev~=Next and G.BlockRevision==Next and G:GetBlock(P).Definition=="Stone")
		assert(G:SetBlock(P,"Stone")==Next)
		local Rot={Up="+Z",Front="+Y"}
		local R=G:SetBlock(P,"Brick",Rot)
		assert(R~=Next and G:GetBlock(P).Orientation.Up=="+Z")
		local Read=G:ReadRegion(Vector3.new(-3,0,-3),Vector3.new(2,1,2))
		assert(Read.Revision==R and #Read.Cells==25 and Read.Cells[1].Position==Vector3.new(-3,0,-3))
		assert(not pcall(function() G.CellSize=2 end))
		assert(not pcall(function() G:SetBlock(P,"Missing") end) and G:GetRevision()==R)
		assert(not pcall(function() G:SetBlock(P,"Stone",{Up="+Y",Front="-Y"}) end))
		assert(not pcall(function() G:Fill(Vector3.zero,Vector3.new(40,40,40),"Stone") end))
		assert(G:GetRevision()==R and G:WorldToCell(G:CellToWorld(P))==P)
		local H=G:Raycast(Vector3.new(-0.5,2,-0.5),Vector3.new(0,-3,0))
		assert(H.Cell==P and H.Block.Definition=="Brick" and H.Normal==Vector3.new(0,1,0) and H.Revision==R)
		assert(not pcall(function() H.Revision="" end))
		G:RemoveBlock(P)
		assert(G:GetBlock(P)==nil and G:Raycast(Vector3.new(-0.5,2,-0.5),Vector3.new(0,-3,0))==nil)
		G:Clear(Vector3.new(-3,0,-3),Vector3.new(2,1,2))
		G.CellSize=2
		G.CFrame=CFrame.new(8,2,-4)*CFrame.Angles(0,math.pi*0.5,0)
		assert(G:WorldToCell(G:CellToWorld(P))==P and G:WorldToCell(G:CellToWorld(Vector3.zero))==Vector3.zero)
		local Other=Instance.new("BlockGrid")
		Other.Parent=game.Workspace
		assert(Other:GetRevision()~=G:GetRevision())
		local Spec={Shape="Wedge",Color=Color3.new(0.25,0.5,1),Ports={"+X","-X"}}
		local Registered=Other:DefineBlock("project:ramp",Spec)
		assert(Other:DefineBlock("project:ramp",Spec)==Registered)
		assert(not pcall(function() Other:DefineBlock("project:ramp",{Shape="Slab"}) end))
		assert(not pcall(function() Other:DefineBlock("project:bad",{Ports={"+Z","+Z"}}) end))
		assert(not pcall(function() Other:DefineBlock("project:bad",{Shape="Unresolved"}) end))
		local Digest=Other:GetDefinitionDigest("project:ramp")
		assert(#Digest==64 and Other:GetRevision()==Registered)
		local Placed=Other:SetBlock(Vector3.zero,"project:ramp")
		local Hit=Other:Raycast(Vector3.new(0.5,2,0.25),Vector3.new(0,-2,0))
		assert(Hit.Block.Definition=="project:ramp" and math.abs(Hit.Position.Y-0.25)<0.001)
		assert(not pcall(function() Other:ReplaceDefinition("project:ramp",{Shape="Slab"},Registered) end))
		assert(Other:ReplaceDefinition("project:ramp",{Shape="Slab",Collision="None",Queryable=false},Placed)~=Placed)
		assert(Other:GetDefinitionDigest("project:ramp")~=Digest and Other:GetBlock(Vector3.zero).Definition=="project:ramp")
		assert(Other:Raycast(Vector3.new(0.5,2,0.25),Vector3.new(0,-2,0))==nil)
		local Room=Instance.new("SpatialRegion")
		Room.Parent=game.Workspace
		local BeforeDomain=G:GetRevision()
		G.SpatialRegion=Room
		assert(G.SpatialRegion==Room and G.Parent==game.Workspace and G:GetRevision()~=BeforeDomain)
		G.SpatialRegion=nil
		assert(G.SpatialRegion==nil and G:GetRevision()~=BeforeDomain)
		local Shapes=Instance.new("BlockGrid")
		Shapes.Parent=Room
		Shapes:DefineBlock("project:ramp",{Shape="Wedge"})
		Shapes:DefineBlock("project:stair",{Shape="Stairs"})
		Shapes:DefineBlock("project:ghost",{Shape="Slab",Collision="None",Queryable=true})
		Shapes:DefineBlock("project:solid",{Shape="Slab",Queryable=false})
		Shapes:SetBlock(Vector3.zero,"project:ramp")
		Shapes:SetBlock(Vector3.new(2,0,0),"project:stair")
		Shapes:SetBlock(Vector3.new(3,0,0),"project:ghost")
		Shapes:SetBlock(Vector3.new(4,0,0),"project:solid")
		local Params=RaycastParams.new()
		Params.IncludeBlockGrids=true
		local function Ray(X,Z) return Room:Raycast(Vector3.new(X,2,Z),Vector3.new(0,-3,0),Params) end
		for _, Z in {0.25,0.75} do
			local H=Ray(0.5,Z)
			assert(H and H.GridHit.Cell==Vector3.zero and math.abs(H.Position.Y-Z)<0.03)
			assert(H.Normal.Y>0.65 and H.Normal.Z < -0.65)
		end
		assert(math.abs(Ray(2.5,0.25).Position.Y-0.5)<0.03 and math.abs(Ray(2.5,0.75).Position.Y-1)<0.03)
		assert(Ray(3.5,0.5).GridHit.Block.Definition=="project:ghost" and Ray(4.5,0.5)==nil)
		Shapes:SetBlock(Vector3.zero,"project:ramp",{Up="+Y",Front="+X"})
		local Rotated=Ray(0.25,0.5)
		assert(Rotated and math.abs(Rotated.Position.Y-0.75)<0.03)
		local Stair=Room:GetPartsInSphere(Vector3.new(2.5,0.5,0.75),0.2,Params,1)
		assert(#Stair.GridCells==1 and not Stair.Truncated and Stair.GridCells[1].Cell==Vector3.new(2,0,0))
		local Air=Room:GetPartsInSphere(Vector3.new(2.5,0.8,0.15),0.05,Params)
		assert(Air.GridCells and #Air.GridCells==0)
		Shapes:Fill(Vector3.new(8,0,0),Vector3.new(16,8,8),"Stone")
		local Center=Vector3.new(10,2.5,2.5)
		for _, R in {Room:GetPartsInSphere(Center,0.25,Params,2),
			Room:GetPartsInBox(CFrame.new(Center),Vector3.new(0.5,0.25,0.25),Params,2),
			Room:GetPartsInCapsule(CFrame.new(Center),0.125,0.5,Params,2)} do
			assert(#R.GridCells==2 and not R.Truncated and R.GridCells[1].Cell==Vector3.new(9,2,2))
			assert(R.GridCells[2].Cell==Vector3.new(10,2,2) and R.GridCells[2].Revision==Shapes:GetRevision())
		end
		assert(Room:GetPartsInSphere(Center,0.25,Params,1).Truncated)
		assert(Room:GetPartsInSphere(Center,0.25).GridCells==nil)
		local Legacy=Instance.new("Part")
		Legacy.Size=Vector3.new(1,1,1)
		Legacy.CFrame=CFrame.new(20.5,0.5,0.5)
		Legacy.Anchored=true
		Shapes:SetBlock(Vector3.new(20,0,0),"Stone")
		Legacy.Parent=Room
		assert(Ray(20.5,0.5).Instance==Legacy)
		local Mixed=Room:GetPartsInSphere(Vector3.new(20.5,0.5,0.5),0.2,Params,1)
		assert(Mixed.Parts[1]==Legacy and #Mixed.GridCells==0 and Mixed.Truncated)
	)");
	Run(World,RuntimeMode::Offline,ScriptSecurityContext::ServerRuntime(),R"(
		local Grid=Instance.new("BlockGrid")
		Grid.Parent=game.Workspace
		Grid:DefineChannel("damage",{Kind="UInt8",Default=0,Persistent=true})
		Grid:DefineChannel("active",{Kind="Bool",Default=false,Indexed=true,OnCopy="Value"})
		local A,B,C=Vector3.zero,Vector3.new(1,0,0),Vector3.new(2,0,0)
		local Site={Min=A,MaxExclusive=Vector3.new(6,1,1)}
		local Prior=Grid:GetRevision()
		local Edit=Grid:BeginEdit(Prior)
		Edit:Fill(A,Vector3.new(3,1,1),"Stone")
		Edit:SetValue(A,"active",true)
		Edit:SetValue(A,"damage",42)
		assert(Grid:GetBlock(A)==nil and Edit:Commit()~=Prior)
		assert(not pcall(function() Edit:Commit() end))
		local Read=Grid:ReadRegion(Site.Min,Site.MaxExclusive,{Where={Channel="active",Equals=true}})
		assert(#Read.Cells==1 and Read.Cells[1].Position==A)
		assert(#Grid:ReadRegion(Site.Min,Site.MaxExclusive,{Definitions={}}).Cells==0)
		local Copy=Grid:BeginEdit(Read.Revision)
		Copy:CopyRegion(A,Vector3.new(1,1,1),B)
		Copy:MoveRegion(B,Vector3.new(2,1,1),C)
		Copy:Commit()
		assert(not Grid:GetBlock(B) and Grid:GetValue(C,"damage")==0 and Grid:GetValue(C,"active"))
		local Active=Grid:FindActive("active",Site)
		local Search=Grid:FindConnected({A},{Bounds=Site,MaxVisited=1})
		local Seen=Grid:GetRevision()
		Grid:SetBlock(B,"Brick")
		local function Drain(Cursor)
			local Cells={}
			while true do
				local Page=Cursor:Step(1)
				assert(Page.Revision==Seen and #Page.Cells<=1)
				for _,P in Page.Cells do assert(not Cells[P]);Cells[P]=true end
				if Page.Status~="More" then return Page.Status,Cells end
			end
		end
		local Status,Members=Drain(Active)
		assert(Status=="Complete" and Members[A] and Members[C] and not Members[B])
		assert(Drain(Search)=="Complete" and Search:Step(1).Status=="Complete")
		local Limited=Grid:FindConnected({A},{Bounds=Site,MaxVisited=1})
		local Page=Limited:Step(32)
		assert(Page.Status=="Limit")
		local Image=Grid:ReadPacked(A,Vector3.new(3,1,1))
		assert(buffer.len(Image.Cells)==6 and #Image.Palette==2 and Image.Bindings.Stone==Grid:GetDefinitionDigest("Stone"))
		local Paste=Grid:BeginEdit()
		Paste:WritePacked(Vector3.new(3,0,0),Site.MaxExclusive,Image)
		buffer.writeu16(Image.Cells,0,0)
		Paste:Commit()
		assert(Grid:GetBlock(Vector3.new(3,0,0)).Definition=="Stone")
		local Bad=Grid:BeginEdit()
		Bad:SetBlock(B,"Brick")
		assert(not pcall(function() Bad:SetValue(B,"damage",256) end))
		assert(not pcall(function() Bad:Commit() end))
		local Stale=Grid:BeginEdit()
		Grid:RemoveBlock(B)
		assert(not pcall(function() Stale:Commit() end))
		local Pool={}
		for I=1,4 do Pool[I]=Grid:BeginEdit() end
		assert(not pcall(function() Grid:BeginEdit() end))
		for _,T in Pool do T:Cancel();T:Cancel() end
		Grid:BeginEdit():Cancel()
	)");
	Run(World,RuntimeMode::NetworkClient,ScriptSecurityContext::ClientRuntime(),R"(
		local G=game.Workspace:FindFirstChild("Construction")
		assert(not pcall(function() G:SetBlock(Vector3.zero,"Stone") end))
		assert(not pcall(function() G:GetBlock(Vector3.zero) end))
		assert(not pcall(function() G.CellSize=1 end))
	)");
	auto Grid=std::dynamic_pointer_cast<BlockGrid>(World->GetService("Workspace")->FindFirstChild("Construction",false));
	Check(Grid!=nullptr,"script-created grid unavailable");
	MutationGateway Gateway;
	auto Edit=std::make_shared<blockgrid::EditRequest>();
	Edit->Writes={{{{0,0,0},{1,1,1}},{blockgrid::Stone,0}}};
	Edit->ExpectedRevision=Grid->GetBlockRevision();
	const SubmitBlockGridEditCommand Command{Grid->GetObjectId(),World->GetObjectId(),Edit};
	Grid->SetAdmission([](const blockgrid::Field::Candidate&,std::span<const blockgrid::Write>){return false;});
	Check(Gateway.Apply(Command,ScriptSecurityContext::ServerRuntime()).Status==MutationStatus::ResourceLimit,"provider denial");
	Check(Grid->GetField().GetOccupiedCount()==0 && Grid->GetBlockRevision()==*Edit->ExpectedRevision,"prior state");
	Grid->SetAdmission({});
	Check(Gateway.Apply(Command,ScriptSecurityContext::ServerRuntime()).Succeeded(),"admit edited source");
	Check(Gateway.Apply(Command,ScriptSecurityContext::ServerRuntime()).Status==MutationStatus::Conflict,"stale token");
	Check(!Gateway.Apply(Command,MutationAuthorityContext::AuthenticatedPeer(ScriptSecurityContext::ClientRuntime(),
		World->GetObjectId())).Succeeded(),"peer cannot inject");
	const auto Studio=ScriptSecurityContext::StudioCoreUi();
	constexpr std::uint64_t Owner=7;
	const auto SubmitStudio=[&](blockgrid::Write W,std::optional<TransactionId> Tx={}) {
		auto Q=std::make_shared<blockgrid::EditRequest>();
		Q->Writes={W}; Q->ExpectedRevision=Grid->GetBlockRevision();
		return Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(),World->GetObjectId(),Q},
			MutationAuthorityContext::Studio(Studio,World->GetObjectId(),Tx,Owner));
	};
	const blockgrid::Region Large{{-16,0,-16},{16,2,16}};
	const auto Pre=World->GetAuthoritativeRevision();
	const auto Initial=blockgrid::Encode(Grid->GetField());
	Check(SubmitStudio({Large,{blockgrid::Brick,2}}).Succeeded(),"authoring fill");
	Check(World->GetAuthoritativeRevision()==Pre+1 && World->Transactions.GetStatus().RetainedCount==1,"one history item");
	const auto Filled=blockgrid::Encode(Grid->GetField());
	Check(SubmitStudio({Large,{blockgrid::Brick,2}}).Succeeded() &&
		World->Transactions.GetStatus().RetainedCount==1 && World->GetAuthoritativeRevision()==Pre+1,"no-op history");
	Check(Gateway.Undo(*World,Studio).Succeeded() && blockgrid::Encode(Grid->GetField())==Initial,"undo 2048 individual cells");
	Check(Gateway.Redo(*World,Studio).Succeeded() && blockgrid::Encode(Grid->GetField())==Filled,"redo fill");
	auto Tx=World->Transactions.Begin(*World,Owner,"Two edits",TransactionOrigin::Studio);
	Check(Tx.Succeeded(),"begin");
	Check(SubmitStudio({blockgrid::SingleCell({1,0,1}),{}},Tx.Id).Succeeded(),"first grouped edit");
	Check(SubmitStudio({blockgrid::SingleCell({2,0,2}),{}},Tx.Id).Succeeded(),"second grouped edit");
	Check(World->Transactions.Commit(*World,Tx.Id,Owner).Succeeded() && World->Transactions.GetStatus().RetainedCount==2,
		"one grouped undo");
	Check(Gateway.Undo(*World,Studio).Succeeded() && blockgrid::Encode(Grid->GetField())==Filled,"group undo");
	Grid->SetAdmission([](const blockgrid::Field::Candidate&,std::span<const blockgrid::Write>){return false;});
	const auto Prior=World->GetAuthoritativeRevision();
	Check(!SubmitStudio({blockgrid::SingleCell({3,0,3}),{}}).Succeeded() &&
		World->GetAuthoritativeRevision()==Prior && World->Transactions.GetStatus().RetainedCount==2,"rejected authoring admission");
	Grid->SetAdmission({});
	auto Diverge=std::make_shared<blockgrid::EditRequest>(); Diverge->Writes={{{{3,0,3},{4,1,4}},{blockgrid::Stone,0}}};
	Check(Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(),World->GetObjectId(),Diverge},
		ScriptSecurityContext::ServerRuntime()).Succeeded(),"independent runtime mutation");
	const auto Diverged=blockgrid::Encode(Grid->GetField());
	Check(!Gateway.Undo(*World,Studio).Succeeded() && blockgrid::Encode(Grid->GetField())==Diverged,"divergence holds current state");
	auto Def=std::make_shared<blockgrid::EditRequest>();
	Def->Binding=blockgrid::BindingEdit{{"project:authored",blockgrid::Shape::Slab}};
	Check(Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(),World->GetObjectId(),Def},
		MutationAuthorityContext::Studio(Studio,World->GetObjectId(),{},Owner)).Succeeded(),"author definition");
	Check(Gateway.Undo(*World,Studio).Succeeded() && !Grid->GetField().FindDefinitionId("project:authored"),"undo definition");
	Check(Gateway.Redo(*World,Studio).Succeeded() && Grid->GetField().FindDefinitionId("project:authored"),"redo definition");
	std::cout<<"[BlockGrid:BindingTest] Class, actual VM edits, authority, grid picking and conflict checks passed\n";
} catch (const std::exception& Error) { std::cerr<<"[BlockGrid:BindingTest] "<<Error.what()<<'\n'; return 1; }
