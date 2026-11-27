// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/blockgrid/Edit.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/terrain/Field.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

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

	MutationResult Edit(const std::shared_ptr<BlockGrid> &Grid, ObjectId Scope, blockgrid::Block Value,
		blockgrid::Cell Position = Target) {
		auto Request = std::make_shared<blockgrid::EditRequest>();
		Request->Writes.push_back({blockgrid::SingleCell(Position), Value});
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

	void AddTerrainFixture(const std::shared_ptr<Workspace> &Space, const std::shared_ptr<SpatialRegion> &First) {
		auto Second = std::make_shared<SpatialRegion>();
		Second->SetName("TerrainSibling");
		Second->SetParent(Space);
		const auto SetSample = [](SpatialRegion &Region, terrain::MaterialId Material) {
			auto Field = std::make_unique<terrain::Field>();
			const std::array<terrain::SampleWrite,1> Writes{{{{2,2,2},{-240,Material}}}};
			const std::array Keys{terrain::GetChunkKey(Writes.front().Point)};
			auto Prepared = terrain::Field::Prepare(Field->Capture(Keys).value(),Writes).value();
			(void)Field->Commit(std::move(Prepared)).value();
			Region.RestoreTerrainField(std::move(Field));
		};
		SetSample(*First,3);
		SetSample(*Second,5);
		auto Proof = std::make_shared<Script>();
		Proof->SetName("VerifyIndependentTerrain");
		Proof->SetRunContext(Enums::RunContext::Server);
		Proof->SetSource(R"(
			local World = game.Workspace
			local Lo, Hi = Vector3.new(2,2,2), Vector3.new(3,3,3)
			local A = World.NestedRegion.Terrain:ReadRegion(Lo,Hi)
			local B = World.TerrainSibling.Terrain:ReadRegion(Lo,Hi)
			local Empty = World.Terrain:ReadRegion(Lo,Hi)
			assert(buffer.readi16(A,0)==-240 and buffer.readu32(A,2)==3)
			assert(buffer.readi16(B,0)==-240 and buffer.readu32(B,2)==5 and buffer.readu32(Empty,2)==0)
			local Cell = Vector3.new(7,8,9)
			local Grid = World.WestGrid
			assert(Grid:GetBlock(Cell)==nil)
			Grid:SetBlock(Cell,"Brick")
			assert(Grid:GetBlock(Cell).Definition=="Brick")
			-- The packaged Player exposes Warning diagnostics on its bounded headless console.
			warn("[Spatial:PlayTerrain] Independent fields and runtime-only cell verified")
		)");
		Proof->SetParent(Space);
	}

	void CheckTerrainOwners(const std::shared_ptr<DataModel> &Game) {
		auto First = std::dynamic_pointer_cast<SpatialRegion>(Game->FindFirstDescendant("NestedRegion"));
		auto Second = std::dynamic_pointer_cast<SpatialRegion>(Game->FindFirstDescendant("TerrainSibling"));
		Require(First && Second && First->GetTerrain() != Second->GetTerrain(), "reopen did not preserve terrain owners");
		const auto &A = First->GetTerrain()->GetField();
		const auto &B = Second->GetTerrain()->GetField();
		Require(A.GetLifetime() != B.GetLifetime() && A.Read({2,2,2}).value() == terrain::Sample{-240,3} &&
			B.Read({2,2,2}).value() == terrain::Sample{-240,5} &&
			WorkspaceOf(Game)->GetTerrain()->GetField().Read({2,2,2}).value() == terrain::AirSample,
			"wrong terrain field published at coincident sample coordinates");
	}

	void CheckDefaultNetworkProfile(const std::shared_ptr<DataModel> &Populated,
		const std::shared_ptr<DataModel> &Ordinary) {
		bool Denied = false;
		try { (void)Populated->AcquireDefaultNetworkSpatialProfile(); }
		catch (const std::invalid_argument &) { Denied = true; }
		Require(Denied && !Populated->HasDefaultNetworkSpatialProfile(),
			"default-only network admission accepted unsupported native content");
		auto Profile = Ordinary->AcquireDefaultNetworkSpatialProfile();
		Require(Profile && Ordinary->HasDefaultNetworkSpatialProfile(), "ordinary world failed default network admission");
		auto FolderValue = std::make_shared<Folder>();
		auto DetachedGrid = std::make_shared<BlockGrid>();
		DetachedGrid->SetParent(FolderValue);
		const auto Revision = Ordinary->GetAuthoritativeRevision();
		Denied = false;
		try { FolderValue->SetParent(WorkspaceOf(Ordinary)); }
		catch (const std::invalid_argument &) { Denied = true; }
		Require(Denied && !FolderValue->GetParent() && !DetachedGrid->GetDataModel() &&
			Ordinary->GetAuthoritativeRevision() == Revision, "network admission partially attached a grid subtree");
		Profile.reset();
		FolderValue->SetParent(WorkspaceOf(Ordinary));
		Require(DetachedGrid->GetDataModel() == Ordinary, "retired network lease blocked offline editing");
		FolderValue->Destroy();
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
		CheckTerrainOwners(Reopened);
		std::optional<terrain::TerrainCapture> DefaultCapture;
		terrain::DomainTerrainCaptures Captures;
		auto Place = ProjectValue.CaptureGasPlace(Game,&DefaultCapture,&Captures);
		Require(Place && Place->Place && !DefaultCapture && Captures.size() == 2 && Place->Place->Domains.size() == 2 &&
			Gas::GetFormatVersion(*Place) == Gas::DomainTerrainPlaceFormatVersion, "GASP omitted bound terrain artifacts");
		auto Bytes = Gas::Encode(*Place).value();
		auto Document = Gas::Decode(Bytes).value();
		auto RestoredPlace = ProjectValue.LoadGasPlace(Document);
		CheckTerrainOwners(RestoredPlace);
		RestoredPlace->Destroy();
		auto FirstBinding = Document.Place->Domains.begin();
		auto SecondBinding = std::next(FirstBinding);
		std::swap(FirstBinding->second,SecondBinding->second);
		bool SwappedRejected = false;
		try { auto Wrong = ProjectValue.LoadGasPlace(Document); Wrong->Destroy(); }
		catch (const std::exception &) { SwappedRejected = true; }
		Require(SwappedRejected, "GASP accepted another terrain owner's manifest digest");
		CheckTerrainOwners(Game);
		auto NewGrid = std::dynamic_pointer_cast<BlockGrid>(Reopened->FindFirstDescendant("WestGrid"));
		auto NewOther = std::dynamic_pointer_cast<BlockGrid>(Reopened->FindFirstDescendant("EastGrid"));
		auto NewRegion = std::dynamic_pointer_cast<SpatialRegion>(Reopened->FindFirstDescendant("NestedRegion"));
		Require(NewGrid && NewOther && NewRegion && NewGrid != Grid && NewOther != Other &&
			NewGrid->GetField().Get(Target) == Grid->GetField().Get(Target) &&
			NewOther->GetField().Get(Target) == Other->GetField().Get(Target), "project reopen lost independent native fields");
		CheckQueries(*WorkspaceOf(Reopened),NewGrid,NewGrid);
		CheckQueries(*NewRegion,NewOther,NewOther);

		auto RegionClone = std::dynamic_pointer_cast<SpatialRegion>(NewRegion->Clone());
		Require(RegionClone && !RegionClone->GetParent() && RegionClone->GetTerrain() != NewRegion->GetTerrain() &&
			RegionClone->GetTerrain()->GetField().GetLifetime() != NewRegion->GetTerrain()->GetField().GetLifetime() &&
			RegionClone->GetTerrain()->GetField().Read({2,2,2}).value() == terrain::Sample{-240,3},
			"cloning an edited project region shared or lost its native field");
		RegionClone->SetName("ClonedRegion");
		RegionClone->SetParent(WorkspaceOf(Reopened));
		terrain::DomainTerrainCaptures ClonedDomains;
		auto ClonedPlace = ProjectValue.CaptureGasPlace(Reopened,&DefaultCapture,&ClonedDomains);
		Require(ClonedPlace && ClonedDomains.size() == 3 && ClonedPlace->Place->Domains.size() == 3,
			"project did not address the cloned field separately");
		for (const auto &[Node,Capture] : ClonedDomains) ProjectValue.PersistTerrainArtifacts(Capture,Node);
		auto ReopenedClone = ProjectValue.LoadGasPlace(*ClonedPlace);
		const auto SavedClone = std::dynamic_pointer_cast<SpatialRegion>(ReopenedClone->FindFirstDescendant("ClonedRegion"));
		Require(SavedClone && SavedClone->GetTerrain()->GetField().Read({2,2,2}).value() == terrain::Sample{-240,3} &&
			SavedClone->GetTerrain()->GetField().GetLifetime() != RegionClone->GetTerrain()->GetField().GetLifetime(),
			"reopening the cloned terrain did not reconstruct its own bound field");
		ReopenedClone->Destroy();
		RegionClone->Destroy();
		CheckTerrainOwners(Reopened);
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

	void CheckOffscreenPixels() {
		constexpr std::uint32_t Width = 192, Height = 128;
		SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, "vulkan", SDL_HINT_OVERRIDE);
		Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		struct VideoLifetime { ~VideoLifetime() { SDL_Quit(); } } Video;
		auto Game = std::make_shared<DataModel>();
		auto World = WorkspaceOf(Game);
		auto Grid = MakeGrid(World, "PixelGrid", {blockgrid::Brick,0}, CFrame{});
		const auto Center = TargetCenter(*Grid);
		const auto Camera = MakeLookAtRenderCameraInput(Center + glm::vec3(3,2,4),Center,{0,1,0},55);
		RenderPublisher Publisher;
		EditorViewportRenderer View(Width,Height);
		const auto Capture = [&] {
			auto Frame = View.Capture(Publisher.Publish(*World, Camera, Width, Height));
			Require(Frame.Width == Width && Frame.Height == Height && Frame.RgbPixels.size() == Width*Height*3,
				"incomplete native GPU readback");
			return Frame.RgbPixels;
		};
		const auto Changed = [](const auto &A, const auto &B) {
			std::size_t Count = 0;
			for (std::size_t I = 0; I < A.size(); I += 3)
				if (A[I] != B[I] || A[I+1] != B[I+1] || A[I+2] != B[I+2]) ++Count;
			return Count;
		};
		Grid->SetVisible(false);
		const auto Blank = Capture();
		Grid->SetVisible(true);
		const auto Visible = Capture();
		Grid->SetCFrame(CFrame(1.25f,0,0));
		const auto Moved = Capture();
		Grid->SetVisible(false);
		const auto Hidden = Capture();
		Grid->SetVisible(true);
		const auto Reappeared = Capture();
		Grid->Destroy();
		const auto Deleted = Capture();
		const auto VisiblePixels = Changed(Blank,Visible);
		const auto MovedPixels = Changed(Visible,Moved);
		Require(VisiblePixels >= 32 && MovedPixels >= 32 && Hidden == Blank && Reappeared == Moved && Deleted == Blank,
			"actual visible/moved/hidden/retired pixels disagree");
		std::cout << "[Spatial:ConstructionGpu] VisiblePixels=" << VisiblePixels
			<< " MovedPixels=" << MovedPixels << " HiddenResidual=" << Changed(Blank,Hidden)
			<< " DeletedResidual=" << Changed(Blank,Deleted) << '\n';

		auto Sparse = std::make_shared<BlockGrid>();
		blockgrid::Field Field;
		std::vector<blockgrid::Write> Writes;
		for (int Z = 0; Z < 32; ++Z)
			for (int X = 0; X < 16; ++X)
				if ((X + Z) % 2 == 0) Writes.push_back({blockgrid::SingleCell({X,0,Z}),{blockgrid::Stone,0}});
		Require(Field.Apply(Writes), "could not seed sparse GPU fixture");
		Sparse->RestoreField(std::move(Field));
		Sparse->SetParent(World);
		constexpr blockgrid::Cell Chosen{4,0,4};
		const auto Detail = MakeLookAtRenderCameraInput({4.5f,6.0f,5.5f},{4.5f,0.5f,4.5f},{0,1,0},58);
		RenderPublisher FullPublisher;
		EditorViewportRenderer FullView(Width,Height);
		const auto Compare = [&](bool ExpectPatch) {
			const auto Partial = Publisher.Publish(*World,Detail,Width,Height);
			if (ExpectPatch)
				Require(Partial->MeshCreates.empty() && Partial->MeshVertexUpdates.size() == 1 &&
					Partial->MeshVertexUpdates.front().Vertices->size() == 24, "sparse edit missed partial GPU path");
			const auto Actual = View.Capture(Partial);
			FullPublisher.RequestFullResync();
			const auto Rebuilt = FullView.Capture(FullPublisher.Publish(*World,Detail,Width,Height));
			Require(Actual.RgbPixels.size() == Width*Height*3 && Actual.RgbPixels == Rebuilt.RgbPixels,
				"partial GPU output differs from independently rebuilt topology");
			return Actual.RgbPixels;
		};
		const auto Filled = Compare(false);
		Require(Edit(Sparse,Game->GetObjectId(),{},Chosen).Succeeded(), "sparse GPU removal failed");
		const auto Cut = Compare(true);
		Require(Edit(Sparse,Game->GetObjectId(),{blockgrid::Brick,0},Chosen).Succeeded(), "sparse GPU restore failed");
		const auto Brick = Compare(true);
		const auto CutPixels = Changed(Filled,Cut), RestoredPixels = Changed(Cut,Brick);
		Require(CutPixels >= 32 && RestoredPixels >= 32 && Changed(Filled,Brick) >= 32,
			"native readback missed topology removal or restored tint");
		std::cout << "[Spatial:ConstructionGpu] SparseRemovedPixels=" << CutPixels
			<< " SparseRestoredPixels=" << RestoredPixels << " RebuildResidual=0\n";
		Game->Destroy();
	}

	void CheckAuthoring(const std::filesystem::path &Root) {
		using Json = nlohmann::ordered_json;
		Require(SDL_Init(0), SDL_GetError());
		struct SdlLifetime { ~SdlLifetime() { SDL_Quit(); } } Sdl;
		EditorHost Host("grid-history-test");
		std::size_t Sequence = 0;
		const auto Call = [&](const char *Method, const Json &Parameters) {
			return Json::parse(Host.HandleRequest(Json{{"Version", EditorHostProtocolVersion},
				{"RequestId", std::to_string(++Sequence)}, {"SessionToken", "grid-history-test"},
				{"Method", Method}, {"Params", Parameters}}.dump()));
		};
		const auto Ok = [&](const char *Method, const Json &Parameters) {
			auto Reply = Call(Method, Parameters);
			if (!Reply.value("Ok", false)) throw std::runtime_error(std::string(Method) + " rejected: " + Reply.dump());
			return Reply.at("Result");
		};
		const Json Open{{"Root",Root.generic_string()}};
		const auto Empty = Json::object();
		const auto Address = [&]() -> Json {
			auto Snapshot = Ok("GetSnapshot",Empty).at("Snapshot");
			for (const auto &Object : Snapshot.at("Objects"))
				if (Object.value("Name","") == "WestGrid")
					return {{"Scope",Snapshot.at("Cursor").at("Scope")}, {"Object",Object.at("Id")},
						{"Cell",{Target.X,Target.Y,Target.Z}}};
			throw std::runtime_error("authoring grid absent from snapshot");
		};
		Ok("Handshake",Empty);
		Ok("OpenProject",Open);
		const auto InitialAddress = Address();
		auto Read = Ok("ReadBlockGridCell",InitialAddress);
		Require(Read.at("Definition") == "Stone", "EditorHost opened the wrong field");
		auto Edit = InitialAddress;
		Edit["MaxExclusive"] = {Target.X+1,Target.Y+1,Target.Z+1};
		Edit["Revision"] = Read.at("Revision");
		Edit["Definition"] = "Brick";
		Edit["Orientation"] = 7;
		auto Receipt = Ok("EditBlockGridRegion",Edit);
		Require(Receipt.at("Revision") != Read.at("Revision") && Receipt.at("Orientation") == 7,
			"EditorHost omitted the edit revision or orientation");
		Require(!Call("EditBlockGridRegion",Edit).at("Ok").get<bool>(), "EditorHost admitted a stale edit");
		Ok("Undo",Empty);
		Require(Ok("ReadBlockGridCell",InitialAddress).at("Definition") == "Stone", "EditorHost undo lost the prior cell");
		Ok("Redo",Empty);
		Read = Ok("ReadBlockGridCell",InitialAddress);
		Require(Read.at("Definition") == "Brick" && Read.at("Orientation") == 7, "EditorHost redo lost the oriented cell");
		Ok("SaveProject",Empty);
		const auto Play = Ok("StartPlaySession",Empty);
		Host.PumpPlaySessionForTesting();
		const Json PlayId{{"PlaySessionId",Play.at("PlaySessionId")}};
		const auto Polled = Ok("PollPlayDiagnostics",PlayId);
		Edit["Revision"] = Read.at("Revision");
		Edit["Definition"] = "Air";
		auto Denied = Call("EditBlockGridRegion",Edit);
		Require(!Denied.at("Ok").get<bool>() && Denied.at("Error").at("Code") == "PlaySessionActive",
			"EditorHost allowed an edit during Play");
		const auto Stopped = Ok("StopPlaySession",PlayId);
		bool TerrainProof = false;
		for (const auto *Packet : {&Play,&Polled,&Stopped}) {
			for (const auto &Entry : Packet->at("Diagnostics")) {
				if (Entry.value("Severity","") == "Error") throw std::runtime_error("Play failed: " + Entry.dump());
				if (Entry.value("Message","").find("[Spatial:PlayTerrain] Independent") != std::string::npos)
					TerrainProof = true;
			}
		}
		Require(TerrainProof, "Play did not verify its private terrain fields and runtime mutation");
		auto RuntimeAddress = InitialAddress;
		RuntimeAddress["Cell"] = {7,8,9};
		Require(Ok("ReadBlockGridCell",RuntimeAddress).at("Definition") == "Air", "Play cell leaked into authoring state");
		Require(Ok("ReadBlockGridCell",InitialAddress).at("Definition") == "Brick", "Play changed authoring state");
		const auto OriginalPreRun = Ok("GetPreRunSource",Empty);
		Ok("OpenProject",Open);
		const auto NewAddress = Address();
		Require(NewAddress.at("Scope") != InitialAddress.at("Scope") &&
			!Call("ReadBlockGridCell",InitialAddress).at("Ok").get<bool>(), "EditorHost reused a replaced world scope");
		Read = Ok("ReadBlockGridCell",NewAddress);
		Require(Read.at("Definition") == "Brick" && Read.at("Orientation") == 7, "EditorHost reopen lost the authored edit");
		const auto EqualSource = Ok("GetPreRunSource",Empty);
		Require(EqualSource.at("ProjectRoot") != OriginalPreRun.at("ProjectRoot") &&
			EqualSource.at("SchemaGeneration") == OriginalPreRun.at("SchemaGeneration") &&
			EqualSource.at("SourceRevision") == OriginalPreRun.at("SourceRevision"),
			"equal-source replacement failed to retire the world or reuse its frozen schema");
		const auto StaleValidation = [&](const Json &State) {
			const auto Reply = Call("ValidatePreRun",{{"ProjectRoot",State.at("ProjectRoot")},
				{"ExpectedSourceRevision",State.at("SourceRevision")},{"SourceBase64",State.at("SourceBase64")}});
			Require(!Reply.at("Ok").get<bool>() && Reply.at("Error").at("Code") == "StaleProject",
				"PreRun accepted a buffer bound to the previous world");
		};
		StaleValidation(OriginalPreRun);
		DiskFilesystem Files(Root);
		auto DiskProject = Project::fromExisting(&Files);
		constexpr auto ChangedSource = R"(Schema:RegisterClass({Namespace="Game", Name="SpatialTestMarker",
			Version=1, Base="Engine.Folder", Properties={MarkerEnabled={Type="Boolean",Default=true}}}))";
		Require(DiskProject.ReplacePreRunSource(std::nullopt,ChangedSource), "PreRun project source change was not admitted");
		Ok("OpenProject",Open);
		const auto ChangedSourceState = Ok("GetPreRunSource",Empty);
		Require(ChangedSourceState.at("ProjectRoot") != EqualSource.at("ProjectRoot") &&
			ChangedSourceState.at("SchemaGeneration").get<std::uint64_t>() > EqualSource.at("SchemaGeneration").get<std::uint64_t>() &&
			ChangedSourceState.at("SourceRevision") != EqualSource.at("SourceRevision"),
			"changed-source replacement failed to publish a new schema generation");
		StaleValidation(EqualSource);
		const auto Schema = Ok("GetSchema",Empty);
		bool Activated = false;
		for (const auto &Row : Schema.at("Definitions"))
			if (Row.value("Namespace","") == "Game" && Row.value("Name","") == "SpatialTestMarker") Activated = true;
		Require(Activated && Ok("ReadBlockGridCell",Address()).at("Definition") == "Brick",
			"changed PreRun registration or saved grid was absent after reopen");
	}

	void AddPackageSessionProof(const std::shared_ptr<DataModel> &Game, const std::shared_ptr<BlockGrid> &Inside) {
		constexpr auto PrivateDefinition = "project:private_sealed_package_tile";
		MutationGateway Gateway;
		auto Request = std::make_shared<blockgrid::EditRequest>();
		Request->Binding = blockgrid::BindingEdit{{PrivateDefinition,blockgrid::Shape::Slab}};
		Require(Gateway.Apply(SubmitBlockGridEditCommand{Inside->GetObjectId(),Game->GetObjectId(),Request},
			ScriptSecurityContext::CoreTrusted()).Succeeded(), "private authored tile definition");
		Require(Edit(Inside,Game->GetObjectId(),{Inside->GetField().GetDefinitionId(PrivateDefinition),0},
			{1,0,1}).Succeeded(), "private authored tile placement");

		auto Remote = std::make_shared<RemoteEvent>();
		Remote->SetName("SpatialPackageProof");
		Remote->SetParent(Game);
		auto Scripts = std::make_shared<Folder>();
		Scripts->SetName("GameScripts");
		Scripts->SetParent(Game);
		auto Server = std::make_shared<Script>();
		Server->SetName("SpatialAuthority");
		Server->SetRunContext(Enums::RunContext::Server);
		Server->SetSource(R"(
			local World = game.Workspace
			local Inside = World.NestedRegion.EastGrid
			assert(Inside:GetBlock(Vector3.new(1, 0, 1)).Definition == "project:private_sealed_package_tile")
			local Grid = World.WestGrid
			local Cell = Vector3.new(-2, 0, 3)
			Grid:DefineChannel("private_damage", {Kind="UInt8", Default=0, Persistent=true})
			Grid:DefineChannel("signal", {Kind="Bool", Default=false, Audience="RelevantPeers"})
			local State = Grid:BeginEdit()
			State:SetValue(Cell, "private_damage", 91)
			State:SetValue(Cell, "signal", true)
			State:Commit()
			local Changed = false
			game.SpatialPackageProof.OnServerEvent:Connect(function(_, Stage)
				if Stage == "baseline" and not Changed then
					assert(Grid:GetValue(Cell, "private_damage") == 91)
					Grid:SetBlock(Cell, "Brick")
					Changed = true
					warn("[Spatial:PackageAuthority] Authenticated client baseline acknowledged and grid edited")
				elseif Stage == "observed" and Changed then
					warn("[Spatial:PackageAuthority] Client acknowledged the edited grid")
				end
			end)
		)");
		Server->SetParent(Scripts);
		auto Client = std::make_shared<Script>();
		Client->SetName("SpatialObserver");
		Client->SetRunContext(Enums::RunContext::Client);
		Client->SetSource(R"(
			local World = game.Workspace
			local Cell = Vector3.new(-2, 0, 3)
			local Sent, Done = false, false
			game:GetService("RunService").PostSimulation:Connect(function()
				if Done then return end
				local Grid = World:FindFirstChild("WestGrid")
				local Event = game:FindFirstChild("SpatialPackageProof")
				if not Grid or not Event then return end
				local Ready, Block = pcall(function() return Grid:GetBlock(Cell) end)
				if not Ready or not Block then return end
				assert(World:FindFirstChild("NestedRegion") == nil)
				if Block.Definition == "Stone" and not Sent then
					if not Grid:GetValue(Cell, "signal") then return end
					assert(not pcall(function() Grid:GetValue(Cell, "private_damage") end))
					assert(not pcall(function() Grid:SetBlock(Cell, "Brick") end))
					Sent = Event:TryFireServer("baseline")
				elseif Sent and Block.Definition == "Brick" then
					assert(Grid:Raycast(Grid:CellToWorld(Cell) + Vector3.new(0, 3, 0), Vector3.new(0, -6, 0)).Cell == Cell)
					Done = Event:TryFireServer("observed")
					if Done then warn("[Spatial:PackageObserver] Public grid, channel, ray and authoritative edit verified") end
				end
			end)
		)");
		Client->SetParent(Scripts);
	}

	void WritePackageProject(const std::filesystem::path &Root, bool Session) {
		Require(std::filesystem::create_directory(Root), "package fixture destination must be new");
		DiskFilesystem Files(Root);
		auto ProjectValue = Project::fromInit(&Files,"SpatialConstructionPackage");
		auto Game = ProjectValue.DeserializeGame();
		Game->InitializeLoadedProjectRevision();
		auto Space = WorkspaceOf(Game);
		auto Region = std::make_shared<SpatialRegion>();
		Region->SetName("NestedRegion");
		Region->SetParent(Space);
		auto Pose = CFrame::Angles(0,1.57079632679f,0);
		Pose.Position = {8,4,-6};
		MakeGrid(Space,"WestGrid",{blockgrid::Stone,0},Pose);
		auto Inside = MakeGrid(Region,"EastGrid",{blockgrid::Brick,1},Pose);
		if (Session) AddPackageSessionProof(Game,Inside);
		else AddTerrainFixture(Space,Region);
		auto Proof = std::make_shared<Script>();
		Proof->SetName("VerifyPackagedGrids");
		Proof->SetRunContext(Enums::RunContext::Server);
		Proof->SetSource(R"(
			local World = game.Workspace
			local Region = World.NestedRegion
			local West = World.WestGrid
			local East = Region.EastGrid
			local Cell = Vector3.new(-2, 0, 3)
			assert(West:GetBlock(Cell).Definition == "Stone" and East:GetBlock(Cell).Definition == "Brick")
			local Params = RaycastParams.new()
			Params.IncludeBlockGrids = true
			local Origin = West:CellToWorld(Cell) + Vector3.new(0, 3, 0)
			local Direction = Vector3.new(0, -6, 0)
			local A = World:Raycast(Origin, Direction, Params)
			local B = Region:Raycast(Origin, Direction, Params)
			assert(A.Instance == West and B.Instance == East and A.GridHit.Cell == Cell and B.GridHit.Cell == Cell)
			warn("[Spatial:PackagedGrid] Two coincident charts restored and queried")
		)");
		Proof->SetParent(Space);
		Game->MarkPersistenceSubtreeArchivable();
		ProjectValue.PersistGameAtomically(ProjectValue.CaptureGame(Game,Game->GetAuthoritativeRevision()));
		Game->Destroy();
	}
}

int main(int Count, char **Arguments) {
	using namespace gargantuan;
	try {
		BootstrapNativeRuntimeSchema();
		if (Count == 2 && std::string_view(Arguments[1]) == "--vulkan") {
			CheckOffscreenPixels();
			return 0;
		}
		if (Count == 3 && (std::string_view(Arguments[1]) == "--write-project" ||
			std::string_view(Arguments[1]) == "--write-session-project")) {
			WritePackageProject(Arguments[2],std::string_view(Arguments[1]) == "--write-session-project");
			return 0;
		}
		Require(Count == 1, "supported modes are --vulkan, --write-project and --write-session-project");
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
		auto SeparatePose = Pose;
		SeparatePose.Position.x += 32;
		West->SetCFrame(SeparatePose);
		West->SetSpatialRegion(Region);
		CheckQueries(*Region,West,West);
		const auto ExplicitCenter = TargetCenter(*West);
		const auto Absent = Space->ResolveRaycast(ExplicitCenter+glm::vec3(0,3,0), {0,-6,0}, GridQueries());
		Require(Absent.Succeeded() && !Absent.HasHit(), "explicitly assigned grid leaked into its parent's chart");
		West->SetSpatialRegion(std::nullopt);
		West->SetCFrame(Pose);
		AddTerrainFixture(Space,Region);
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
		CheckDefaultNetworkProfile(Game,Independent);
		Independent->Destroy();
		CheckPersistence(ProjectValue,Game,West,East);
		const auto Center = TargetCenter(*East);
		East->Destroy();
		const auto Removed = Region->ResolveRaycast(Center+glm::vec3(0,3,0), {0,-6,0}, GridQueries());
		Require(Removed.Succeeded() && !Removed.HasHit(), "destroyed grid still had queryable geometry");
		Game->Destroy();
		CheckAuthoring(Scratch.Root);
		std::cout << "[Spatial:ConstructionTest] Passed domain, query, motion, render, persistence and EditorHost checks\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Spatial:ConstructionTest] " << Error.what() << '\n';
		return 1;
	}
}
