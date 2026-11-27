// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include <luacode.h>
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
		auto Status=luau_load(Engine.L,"BlockGridNative",Code,Size,0);
		std::free(Code);
		if (!Status) Status=lua_pcall(Engine.L,0,0,0);
		if (Status) throw std::runtime_error(lua_tostring(Engine.L,-1));
	}
}
int main() try {
	using namespace gargantuan;
	BootstrapNativeRuntimeSchema();
	auto World=std::make_shared<DataModel>();
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
		local Room=Instance.new("SpatialRegion")
		Room.Parent=game.Workspace
		local BeforeDomain=G:GetRevision()
		G.SpatialRegion=Room
		assert(G.SpatialRegion==Room and G.Parent==game.Workspace and G:GetRevision()~=BeforeDomain)
		G.SpatialRegion=nil
		assert(G.SpatialRegion==nil and G:GetRevision()~=BeforeDomain)
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
	Check(!Gateway.Apply(Command,MutationAuthorityContext::AuthenticatedPeer(ScriptSecurityContext::ServerRuntime(),
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
	std::cout<<"[BlockGrid:BindingTest] Class, actual VM edits, authority, grid picking and conflict checks passed\n";
} catch (const std::exception& Error) { std::cerr<<"[BlockGrid:BindingTest] "<<Error.what()<<'\n'; return 1; }
