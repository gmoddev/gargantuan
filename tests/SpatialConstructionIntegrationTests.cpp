// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	constexpr blockgrid::Cell Target{-2, 0, 3};

	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	struct ScratchProject {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-spatial-construction-" + ProjectId::New().ToString());
		ScratchProject() {
			Require(std::filesystem::create_directory(Root), "could not create private test project");
		}
		~ScratchProject() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	};

	std::shared_ptr<Workspace> WorkspaceOf(const std::shared_ptr<DataModel> &Game) {
		return std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
	}

	MutationResult Edit(const std::shared_ptr<BlockGrid> &Grid, ObjectId Scope, blockgrid::Block Value) {
		auto Request = std::make_shared<blockgrid::EditRequest>();
		Request->Writes.push_back({blockgrid::SingleCell(Target), Value});
		MutationGateway Gateway;
		return Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(), Scope, Request},
			ScriptSecurityContext::CoreTrusted());
	}

	std::shared_ptr<BlockGrid> MakeGrid(const std::shared_ptr<Instance> &Parent, const char *Name,
		blockgrid::Block Value, const CFrame &Pose) {
		auto Grid = std::make_shared<BlockGrid>();
		Grid->SetName(Name);
		Grid->SetCFrame(Pose);
		Grid->SetParent(Parent);
		const auto Result = Edit(Grid, Parent->GetDataModel()->GetObjectId(), Value);
		if (!Result.Succeeded()) throw std::runtime_error("grid edit failed: " + Result.Message);
		return Grid;
	}

	RaycastParams GridQueries() {
		RaycastParams Params;
		Params.IncludeBlockGrids = true;
		return Params;
	}

	glm::vec3 TargetCenter(const BlockGrid &Grid) {
		return Grid.GetCFrame().PointToWorldSpace(glm::vec3(-1.5f, 0.5f, 3.5f) * Grid.GetCellSize());
	}

	void CheckQueries(WorldRoot &World, const std::shared_ptr<BlockGrid> &Grid,
		const std::shared_ptr<Instance> &FilterRoot) {
		const auto Center = TargetCenter(*Grid);
		const auto Origin = Center + glm::vec3(0, 3, 0);
		const glm::vec3 Down(0, -6, 0);
		Require(!World.ResolveRaycast(Origin, Down).HasHit(), "ordinary ray unexpectedly included grid");
		const auto Hit = World.ResolveRaycast(Origin, Down, GridQueries());
		Require(Hit.Succeeded() && Hit.GridInstance == Grid && Hit.GetHitInstance() == Grid &&
			Hit.Cell && *Hit.Cell == glm::ivec3{-2,0,3}, "rotated ray returned the wrong owner or cell");
		Require(std::abs(Hit.Distance - 2.5f) < 0.04f && Hit.Normal.y > 0.9f, "ray distance/normal is invalid");

		PhysicsQueryShape Sphere;
		Sphere.Transform = CFrame(Origin);
		Sphere.Radius = 0.15f;
		auto Params = GridQueries();
		Params.FilterType = Enums::RaycastFilterType::Include;
		Params.FilterDescendantsInstances.Values = {FilterRoot};
		const auto Cast = World.ResolveShapeCast(Sphere, Down, Params);
		Require(Cast.Succeeded() && Cast.GridInstance == Grid && Cast.Cell == Hit.Cell,
			"included filtered sweep lost semantic cell");
		PhysicsQueryShape Box;
		Box.Kind = PhysicsQueryShapeKind::Box;
		Box.Transform = CFrame(Center);
		Box.Size = {0.25f, 0.25f, 0.25f};
		const auto Overlap = World.ResolveOverlap(Box, Params);
		Require(Overlap.Succeeded() && Overlap.Parts.empty() && !Overlap.TerrainInstance &&
			Overlap.BlockGrids == std::vector<std::shared_ptr<BlockGrid>>{Grid}, "overlap did not return this grid once");
		Params.FilterType = Enums::RaycastFilterType::Exclude;
		const auto ExcludedHit = World.ResolveShapeCast(Sphere, Down, Params);
		const auto ExcludedOverlap = World.ResolveOverlap(Box, Params);
		Require(ExcludedHit.Succeeded() && !ExcludedHit.HasHit() && ExcludedOverlap.Succeeded() &&
			ExcludedOverlap.BlockGrids.empty(), "excluded hierarchy leaked into sweep/overlap");
	}

	void CheckCollisionFlags(WorldRoot &World, const std::shared_ptr<BlockGrid> &Grid) {
		const auto Start = TargetCenter(*Grid) + glm::vec3(0, 2, 0);
		const glm::vec3 Sweep(0, -4, 0);
		const PhysicsKinematicMotionRequest Request{.Position=Start, .Radius=0.25f, .Height=1.0f, .Translation=Sweep};
		Grid->SetCanQuery(false);
		Require(!World.ResolveRaycast(Start, Sweep, GridQueries()).HasHit(), "collision-only grid was queryable");
		const auto Blocked = World.ResolveKinematicMotion(Request);
		Require(Blocked.Succeeded() && Blocked.AppliedTranslation.y > -2.0f, "collision-only grid failed to stop motion");
		Grid->SetCanCollide(false);
		Grid->SetCanQuery(true);
		Require(World.ResolveRaycast(Start, Sweep, GridQueries()).GridInstance == Grid, "query-only grid was absent");
		const auto Passed = World.ResolveKinematicMotion(Request);
		Require(Passed.Succeeded() && std::abs(Passed.AppliedTranslation.y - Sweep.y) < 0.02f,
			"query-only grid stopped authoritative motion");
		Grid->SetCanCollide(true);
	}

	void CheckPublication(const std::shared_ptr<Workspace> &Space, const std::shared_ptr<SpatialRegion> &Region,
		const std::shared_ptr<BlockGrid> &Grid, const std::shared_ptr<BlockGrid> &Other) {
		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput({0,4,16},{0,0,0},{0,1,0},60);
		const auto Publish = [&](WorldRoot &World) {
			const auto Packet = Publisher.Publish(World, Camera, 640, 360);
			Require(Packet != nullptr, "publisher returned no projection");
			return Projection.Apply(*Packet);
		};
		const auto First = Publish(*Space);
		Require(First.MeshesCreated && First.VertexUploadBytes && Projection.GetItem(Grid->GetObjectId()) &&
			!Projection.GetItem(Other->GetObjectId()), "default publication was empty or leaked the other chart");
		const auto Prior = Grid->GetCFrame();
		auto Shift = Prior;
		Shift.Position += glm::vec3(13,2,0);
		Grid->SetCFrame(Shift);
		const auto Moved = Publish(*Space);
		const auto Item = Projection.GetItem(Grid->GetObjectId());
		Require(Item && glm::distance(glm::vec3(Item->ModelMatrix[3]), Shift.Position) < 0.001f &&
			Moved.VertexUploadBytes == 0, "transform-only grid movement rebuilt mesh or missed movement");
		Grid->SetVisible(false);
		Publish(*Space);
		Require(!Projection.GetItem(Grid->GetObjectId()), "hidden grid remained in render projection");
		Grid->SetVisible(true);
		Grid->SetCFrame(Prior);
		Publish(*Space);
		Require(Projection.GetItem(Grid->GetObjectId()), "visible grid was not rematerialized");
		Space->SetCurrentSpatialRegion(Region);
		Publish(**Space->GetCurrentSpatialRegion());
		Require(!Projection.GetItem(Grid->GetObjectId()) && Projection.GetItem(Other->GetObjectId()),
			"view switch retained grid from another chart");
		Space->SetCurrentSpatialRegion(std::nullopt);
		Publish(*Space);
		Require(Projection.GetItem(Grid->GetObjectId()) && !Projection.GetItem(Other->GetObjectId()),
			"return to default chart failed");
		auto Temporary = MakeGrid(Space, "Transient", {blockgrid::Stone,0}, CFrame(200,0,0));
		Publish(*Space);
		const auto Id = Temporary->GetObjectId();
		Require(Projection.GetItem(Id), "temporary grid did not publish");
		Temporary->Destroy();
		Publish(*Space);
		Require(!Projection.GetItem(Id) && !ObjectRegistry::Get().Lookup(Id), "destroyed grid retained a live projection");
	}

	void CheckPersistence(Project &ProjectValue, const std::shared_ptr<DataModel> &Game,
		const std::shared_ptr<BlockGrid> &Grid, const std::shared_ptr<BlockGrid> &Other) {
		Game->MarkPersistenceSubtreeArchivable();
		const auto Clone = std::dynamic_pointer_cast<BlockGrid>(Grid->Clone());
		Require(Clone && Clone->GetObjectId() != Grid->GetObjectId() &&
			Clone->GetField().Get(Target) == Grid->GetField().Get(Target), "clone lost grid bytes or reused its lifetime");
		Clone->Destroy();
		ProjectValue.PersistGameAtomically(ProjectValue.CaptureGame(Game,Game->GetAuthoritativeRevision()));
		auto Reopened = ProjectValue.DeserializeGame();
		Reopened->InitializeLoadedProjectRevision();
		auto NewGrid = std::dynamic_pointer_cast<BlockGrid>(Reopened->FindFirstDescendant("WestGrid"));
		auto NewOther = std::dynamic_pointer_cast<BlockGrid>(Reopened->FindFirstDescendant("EastGrid"));
		auto NewRegion = std::dynamic_pointer_cast<SpatialRegion>(Reopened->FindFirstDescendant("NestedRegion"));
		Require(NewGrid && NewOther && NewRegion && NewGrid != Grid && NewOther != Other &&
			NewGrid->GetField().Get(Target) == Grid->GetField().Get(Target) &&
			NewOther->GetField().Get(Target) == Other->GetField().Get(Target), "project reopen lost independent native fields");
		CheckQueries(*WorkspaceOf(Reopened),NewGrid,NewGrid);
		CheckQueries(*NewRegion,NewOther,NewOther);
		const std::array<std::shared_ptr<Instance>,1> Roots{Grid};
		const auto Captured = InstanceSerialization::CaptureGasDocument(Roots,Gas::DocumentKind::Model);
		Require(Captured.has_value(), "GASM capture failed");
		const auto Packed = Gas::Encode(*Captured);
		Require(Packed.has_value(), "GASM encode failed");
		const auto Parsed = Gas::Decode(*Packed);
		Require(Parsed.has_value(), "GASM binary round trip failed");
		auto Restored = InstanceSerialization::MaterializeGasDocument(*Parsed,false);
		Require(Restored.Succeeded() && Restored.Roots.size()==1, "valid GASM grid hydration failed");
		auto ModelGrid = std::dynamic_pointer_cast<BlockGrid>(Restored.Roots.front());
		Require(ModelGrid && ModelGrid->GetField().Get(Target)==Grid->GetField().Get(Target), "GASM lost the native field");
		ModelGrid->Destroy();
		auto Corrupt = *Parsed;
		auto &Properties = Corrupt.Nodes.at(0).SavedProperties;
		bool Changed = false;
		for (auto &[Name, Value] : Properties) {
			if (auto *Bytes = std::get_if<Gas::Binary>(&Value)) {
				Require(!Bytes->empty(), "GASM captured empty native payload");
				Bytes->pop_back();
				Changed = true;
			}
		}
		Require(Changed, "GASM never contained the native grid payload");
		auto Rejected = InstanceSerialization::MaterializeGasDocument(Corrupt,false);
		Require(!Rejected.Succeeded() && Rejected.Roots.empty() &&
			Grid->GetField().Get(Target) == blockgrid::Block{blockgrid::Stone,0}, "corrupt native data escaped admission");
		Reopened->Destroy();
	}
}

int main() {
	using namespace gargantuan;
	try {
		BootstrapNativeRuntimeSchema();
		ScratchProject Scratch;
		DiskFilesystem Files(Scratch.Root);
		auto ProjectValue = Project::fromInit(&Files,"SpatialConstruction");
		auto Game = ProjectValue.DeserializeGame();
		Game->InitializeLoadedProjectRevision();
		auto Space = WorkspaceOf(Game);
		auto Region = std::make_shared<SpatialRegion>();
		Region->SetName("NestedRegion");
		Region->SetParent(Space);
		auto Inside = std::make_shared<Folder>();
		Inside->SetParent(Region);
		auto Pose = CFrame::Angles(0,1.57079632679f,0);
		Pose.Position = {8,4,-6};
		auto West = MakeGrid(Space,"WestGrid",{blockgrid::Stone,0},Pose);
		auto East = MakeGrid(Inside,"EastGrid",{blockgrid::Brick,1},Pose);
		CheckQueries(*Space,West,West);
		CheckQueries(*Region,East,Inside);
		CheckCollisionFlags(*Region,East);
		CheckPublication(Space,Region,West,East);
		const auto Independent = std::make_shared<DataModel>();
		const auto OtherSpace = WorkspaceOf(Independent);
		const auto Miss = OtherSpace->ResolveRaycast(TargetCenter(*West)+glm::vec3(0,3,0), {0,-6,0}, GridQueries());
		Require(Miss.Succeeded() && !Miss.HasHit(), "independent world received another world's collision");
		bool ForeignDenied = false;
		try { OtherSpace->SetCurrentSpatialRegion(Region); } catch (const std::exception &) { ForeignDenied = true; }
		Require(ForeignDenied && !OtherSpace->GetCurrentSpatialRegion(), "foreign domain became the current view");
		Require(!Edit(West,Independent->GetObjectId(),{blockgrid::Air,0}).Succeeded() &&
			West->GetField().Get(Target).Definition == blockgrid::Stone, "foreign mutation scope removed an occupied cell");
		Independent->Destroy();
		CheckPersistence(ProjectValue,Game,West,East);
		const auto Center = TargetCenter(*East);
		East->Destroy();
		const auto Removed = Region->ResolveRaycast(Center+glm::vec3(0,3,0), {0,-6,0}, GridQueries());
		Require(Removed.Succeeded() && !Removed.HasHit(), "destroyed grid still had queryable geometry");
		Game->Destroy();
		std::cout << "[Spatial:ConstructionTest] Passed domain, query, motion, render and persistence checks\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Spatial:ConstructionTest] " << Error.what() << '\n';
		return 1;
	}
}
