#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/render/SpatialViewportCapture.hpp"
#include "../src/terrain/ScenePersistence.hpp"
#include "../src/terrain/TerrainMutation.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <iostream>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace gargantuan {
	struct WorldRootTestAccess {
		static void StartTerrain(WorldRoot &Owner) { Owner.InitializeTerrainRuntime(RuntimeMode::Offline); }
		static std::uint64_t Submit(WorldRoot &World) {
			return World.SubmitTerrainEdit(*World.GetOwnedTerrain(), {terrain::SphereBrush{
				{8, 9, 8}, 2., terrain::BrushMode::Add, 4}}).value();
		}
		static bool HasTicket(WorldRoot &World, std::uint64_t Ticket) { return World.GetTerrainTicket(Ticket).has_value(); }
	};
}

namespace {
	using namespace gargantuan;
	void Check(bool Success, const char *Reason) {
		if (!Success) throw std::runtime_error(Reason);
	}

	auto MakePart(const std::shared_ptr<Instance> &Parent, glm::vec3 Size, glm::vec3 Position) {
		auto PartValue = std::make_shared<Part>();
		PartValue->SetAnchored(true);
		PartValue->SetSize(Size);
		PartValue->SetCFrame(CFrame(Position));
		PartValue->SetParent(Parent);
		return PartValue;
	}

	auto Ray(WorldRoot &World, float X = 5.f) {
		return World.ResolveRaycast({X, 10, 0}, {0, -16, 0});
	}

	void PublicationIncarnation() {
		auto Game = std::make_shared<DataModel>();
		auto Space = Game->GetService("Workspace");
		alignas(SpatialRegion) std::array<std::byte, sizeof(SpatialRegion)> Storage{};
		auto MakeRegion = [&] {
			auto *Region = std::construct_at(reinterpret_cast<SpatialRegion *>(Storage.data()));
			return std::shared_ptr<SpatialRegion>(Region, [](SpatialRegion *Object) { std::destroy_at(Object); });
		};
		RenderPublisher Publisher;
		RenderProjection Draw;
		const auto Camera = MakeLookAtRenderCameraInput({0, 4, 12}, {0, 2, 0}, {0, 1, 0}, 50);
		auto Region = MakeRegion();
		Region->SetParent(Space);
		auto OldPart = MakePart(Region, {1, 1, 1}, {1, 2, 0});
		const auto OldId = OldPart->GetObjectId();
		(void)Draw.Apply(*Publisher.Publish(*Region, Camera, 128, 96));
		const auto Steady = Publisher.Publish(*Region, Camera, 128, 96);
		Check(Draw.GetItem(OldId) && !Steady->FullResync,
			"stable rendered realm and activation reuse the committed source");
		(void)Draw.Apply(*Steady);
		Region->SetParent(std::nullopt);
		OldPart->Destroy();
		auto NewPart = MakePart(Region, {1, 1, 1}, {2, 2, 0});
		Region->SetParent(Space);
		auto Restart = Publisher.Publish(*Region, Camera, 128, 96);
		Check(Restart->FullResync, "reattaching a named world replaces its prior published activation");
		(void)Draw.Apply(*Restart);
		Check(!Draw.GetItem(OldId) && Draw.GetItem(NewPart->GetObjectId()), "reactivation retired prior geometry");

		// Both incarnations use the exact same address in the same live DataModel.
		const auto RetiredId = NewPart->GetObjectId();
		const auto PriorActivation = Region->GetSpatialActivation();
		std::weak_ptr<SpatialRegion> Retired = Region;
		bool RejectedDuringDestruction = false;
		auto OnDestroy = Region->Destroying->Once([&](std::monostate) {
			try { (void)Publisher.Publish(*Region, Camera, 128, 96); }
			catch (const std::invalid_argument &) { RejectedDuringDestruction = true; }
		});
		Region->Destroy();
		Check(RejectedDuringDestruction, "the incremental renderer rejects a retiring chart inside its first observer");
		Region.reset();
		Check(Retired.expired(), "the renderer cannot keep a retired world alive");
		Region = MakeRegion();
		Region->SetParent(Space);
		Region->SetParent(std::nullopt);
		Region->SetParent(Space);
		Check(Region->GetSpatialActivation() == PriorActivation, "replacement uses the same numeric activation");
		auto Replacement = MakePart(Region, {1, 1, 1}, {3, 2, 0});
		const auto Fresh = Publisher.Publish(*Region, Camera, 128, 96);
		Check(Fresh->FullResync, "same-address replacement starts a fresh publication");
		(void)Draw.Apply(*Fresh);
		Check(!Draw.GetItem(RetiredId) && Draw.GetItem(Replacement->GetObjectId()), "replacement owns only its new geometry");
		Game->Destroy();
	}

	std::unique_ptr<terrain::Field> MakeField(double Height, terrain::MaterialId Material) {
		auto Field = std::make_unique<terrain::Field>();
		const terrain::SphereBrush Brush{{8, Height, 8}, 2.7, terrain::BrushMode::Add, Material};
		const auto Bounds = terrain::GetBrushBounds(Field->GetGrid(), Brush).value();
		const auto Keys = terrain::GetRegionChunks(Bounds).value();
		auto Candidate = terrain::Field::Prepare(Field->Capture(Keys).value(), Brush).value();
		(void)Field->Commit(std::move(Candidate)).value();
		return Field;
	}

	void TerrainDomains() {
		const auto Game = std::make_shared<DataModel>();
		const auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		const auto A = std::make_shared<SpatialRegion>(), B = std::make_shared<SpatialRegion>();
		A->SetParent(Space); B->SetParent(Space);
		Check(A->GetTerrain() && B->GetTerrain() && A->GetTerrain() != B->GetTerrain() &&
			A->GetTerrain() != Space->GetTerrain(), "each physical domain owns its terrain shell");
		A->RestoreTerrainField(MakeField(6., 3)); B->RestoreTerrainField(MakeField(12., 5));
		const auto Project = ProjectId::New();
		const auto SavedA = terrain::CaptureDomainTerrain(*A, Project, 7).value();
		const auto SavedB = terrain::CaptureDomainTerrain(*B, Project, 9).value();
		Check(!terrain::CaptureSceneTerrain(*Game, Project) && SavedA.Index.Dataset != SavedB.Index.Dataset,
			"two non-default terrain closures must not populate Workspace");
		const auto Read = [&](const terrain::TerrainArtifactId &Id, std::size_t Maximum) {
			if (terrain::FindTerrainArtifact(SavedA.Artifacts, Id))
				return terrain::ReadCapturedTerrainArtifact(SavedA.Artifacts, Id, Maximum);
			return terrain::ReadCapturedTerrainArtifact(SavedB.Artifacts, Id, Maximum);
		};
		bool Denied = false;
		try { (void)terrain::LoadDomainTerrain(Project, 9, SavedA.ManifestDigest, Read); }
		catch (const std::runtime_error &) { Denied = true; }
		Check(Denied, "domain terrain must reject a foreign addressed manifest");
		const auto Reopened = std::make_shared<DataModel>();
		const auto Default = std::dynamic_pointer_cast<Workspace>(Reopened->GetService("Workspace"));
		const auto Low = std::make_shared<SpatialRegion>(), High = std::make_shared<SpatialRegion>();
		Low->SetParent(Default); High->SetParent(Default);
		Low->RestoreTerrainField(terrain::LoadDomainTerrain(Project, 7, SavedA.ManifestDigest, Read));
		High->RestoreTerrainField(terrain::LoadDomainTerrain(Project, 9, SavedB.ManifestDigest, Read));
		Check(Low->GetTerrain()->GetField().GetLifetime() != A->GetTerrain()->GetField().GetLifetime(),
			"opening a domain terrain cannot inherit an old edit ticket lifetime");
		WorldRootTestAccess::StartTerrain(*Default);
		WorldRootTestAccess::StartTerrain(*Low); WorldRootTestAccess::StartTerrain(*High);
		RaycastParams Params;
		Params.IncludeTerrain = true;
		const auto Cast = [&](WorldRoot &World) { return World.ResolveRaycast({8, 25, 8}, {0, -28, 0}, Params); };
		const auto Below = Cast(*Low), Above = Cast(*High);
		Check(Below.TerrainInstance == Low->GetTerrain() && Above.TerrainInstance == High->GetTerrain() &&
			Above.Position.y > Below.Position.y + 4.f && !Cast(*Default).HasHit(), "native terrain collision leaks across domains");
		RenderPublisher Pub;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput({8, 17, 25}, {8, 8, 8}, {0, 1, 0}, 55);
		for (const auto &Scope : {Low, High}) {
			Pub.SetTerrainRenderState(Scope->GetTerrainRenderState());
			(void)Projection.Apply(*Pub.Publish(*Scope, Camera, 120, 90));
			const auto &Draws = Projection.GetTerrainDraws();
			Check(!Draws.empty() && std::ranges::all_of(Draws, [&](const auto &Draw) {
				return Draw.Item.Object == Scope->GetTerrain()->GetObjectId();
			}), "camera must publish only its selected terrain surface");
		}
		const auto View = std::make_shared<ViewportFrame>();
		View->SetParent(Default);
		const auto CameraObject = std::make_shared<gargantuan::Camera>();
		CameraObject->SetCFrame(CFrame(8, 12, 25));
		CameraObject->SetParent(View); View->SetCurrentCamera(CameraObject);
		SpatialViewportCapture Capture;
		for (const auto &Scope : {Low, High}) {
			View->SetSpatialRegion(Scope);
			const auto First = Capture.Capture(*View, *Scope, nullptr, 128, 96).value();
			const auto Again = Capture.Capture(*View, *Scope, nullptr, 128, 96).value();
			Check(First.World == Scope->GetObjectId() && First.Geometry && First.Geometry == Again.Geometry,
				"unchanged live domain GUI view must reuse its immutable geometry");
			RenderProjection Secondary, Empty;
			Secondary.ApplyViewportScene(First, Empty);
			Check(!Secondary.GetTerrainDraws().empty() && std::ranges::all_of(Secondary.GetTerrainDraws(),
				[&](const auto &Draw) { return Draw.Item.Object == Scope->GetTerrain()->GetObjectId(); }),
				"a secondary domain camera needs its own terrain resources even when the main world is empty");
		}
		const auto OldTicket = WorldRootTestAccess::Submit(*High);
		High->SetParent(nullptr);
		Check(!High->IsTerrainCollisionReady() && !Cast(*High).Succeeded(), "detached domain rejects physical queries and retires terrain");
		High->SetParent(Default); WorldRootTestAccess::StartTerrain(*High);
		const auto NewTicket = WorldRootTestAccess::Submit(*High);
		Check(NewTicket > OldTicket && !WorldRootTestAccess::HasTicket(*High, OldTicket) &&
			WorldRootTestAccess::HasTicket(*High, NewTicket), "reattachment cannot reuse an expired terrain edit ticket");
		Game->Destroy(); Reopened->Destroy();
	}

	void DestinationReadiness() {
		const auto Game = std::make_shared<DataModel>();
		const auto Default = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		const auto A = std::make_shared<SpatialRegion>(), B = std::make_shared<SpatialRegion>();
		A->SetParent(Default); B->SetParent(Default);
		const SpatialDestinationBounds Bounds{{-3,-3,-3}, {3,3,3}};
		const auto Empty = A->PrepareSpatialDestination(Bounds).value();
		Check(Empty.Providers.size() == 1 && Empty.Providers.front().AuthoritativeEmpty &&
			A->ValidateSpatialDestination(Empty) && !B->ValidateSpatialDestination(Empty),
			"a held empty destination belongs only to its attached world");
		const auto Near = MakePart(A, {1,1,1}, {0,0,0});
		const auto Far = MakePart(A, {1,1,1}, {80,50,60});
		const auto Foreign = MakePart(B, {1,1,1}, {0,0,0});
		const std::array Missing{Foreign->GetObjectId()};
		Check(!A->ValidateSpatialDestination(Empty) && !A->PrepareSpatialDestination(Bounds, Missing),
			"a newly participating body or a missing required provider prevents handoff");
		const auto Held = A->PrepareSpatialDestination(Bounds).value();
		Far->SetCFrame(CFrame(100,50,60));
		Check(A->ValidateSpatialDestination(Held), "an unrelated ordinary body outside the footprint invalidated the transfer");
		Near->SetCFrame(CFrame(0,1,0));
		Check(!A->ValidateSpatialDestination(Held), "pose change inside the footprint must invalidate the transfer");
		const auto Grid = std::make_shared<BlockGrid>();
		blockgrid::Field Cells;
		const std::array<blockgrid::Write,1> Writes{{{blockgrid::SingleCell({0,0,0}), {blockgrid::Stone,0}}}};
		Check(Cells.Apply(Writes), "grid fixture did not change");
		Grid->RestoreField(std::move(Cells)); Grid->SetParent(A);
		const std::array Required{Grid->GetObjectId(), A->GetTerrain()->GetObjectId()};
		const auto WithGrid = A->PrepareSpatialDestination(Bounds, Required).value();
		const auto Provider = std::ranges::find(WithGrid.Providers, Grid->GetObjectId(), &SpatialProviderReceipt::Owner);
		Check(Provider != WithGrid.Providers.end() && Provider->Provider == SpatialCollisionProvider::BlockGrid &&
			!Provider->Installed.empty() && !Provider->AuthoritativeEmpty, "grid receipt must bind installed native bodies");
		Grid->SetCFrame(CFrame(0,2,0));
		Check(!A->ValidateSpatialDestination(WithGrid), "moving grid owner must invalidate a held destination");
		const auto PartsGrid = std::make_shared<BlockGrid>();
		blockgrid::Field Catalog;
		Check(Catalog.Define({.Key="project:low",.Geometry=blockgrid::Shape::Slab}) &&
			Catalog.Define({.Key="project:sensor",.CanCollide=false,.Queryable=true}), "shape catalog declarations");
		const std::array<blockgrid::Write,2> Pieces{{
			{blockgrid::SingleCell({0,0,0}), {Catalog.GetDefinitionId("project:low"),0}},
			{blockgrid::SingleCell({3,0,0}), {Catalog.GetDefinitionId("project:sensor"),0}}}};
		Check(Catalog.Apply(Pieces), "shape catalog membership");
		PartsGrid->RestoreField(std::move(Catalog)); PartsGrid->SetCFrame(CFrame(31,0,0)); PartsGrid->SetParent(B);
		const std::array CatalogOwner{PartsGrid->GetObjectId()};
		const auto ProbePiece = [&](float X, float Y) {
			const SpatialDestinationBounds At{{X,Y,.40f},{X+.05f,Y+.04f,.45f}};
			const auto Receipt = B->PrepareSpatialDestination(At, CatalogOwner).value();
			return std::ranges::find(Receipt.Providers, PartsGrid->GetObjectId(), &SpatialProviderReceipt::Owner)->Installed;
		};
		const auto LowPiece = ProbePiece(31.4f,.20f);
		Check(LowPiece.size() == 1 && LowPiece.front().CatalogGeometry == std::uint8_t(blockgrid::Shape::Slab) &&
			ProbePiece(31.4f,.80f).empty() && ProbePiece(34.4f,.20f).empty(),
			"movement receipts require the real slab half and discard query-only catalog pieces");
		const auto Actor = MakePart(Default, {1,1,1}, {20,0,0});
		Actor->SetAnchored(false); Actor->SetSpatialRegion(A);
		Actor->SetLinearVelocity({3,0,0}); Actor->ApplyImpulse({2,0,0});
		const auto Identity = Actor->GetObjectId();
		const SpatialMovementRequest Movement{Identity, A->GetObjectId(), A->GetSpatialActivation(),
			Actor->GetCFrame(), CFrame(0,0,0)};
		const auto Blocked = B->PrepareSpatialDestination(Bounds).value();
		Check(!B->CommitSpatialTransfer(Actor, Movement, Blocked).Succeeded() &&
			WorldRoot::GetEffectiveWorld(*Actor) == A && Actor->GetLinearVelocity().x == 3.f,
			"occupied handoff cannot move or consume the source body");
		Foreign->SetCFrame(CFrame(90,0,0));
		Check(!B->CommitSpatialTransfer(Actor, Movement, Blocked).Succeeded(), "handoff accepted an expired destination receipt");
		const auto Solid = std::make_shared<BlockGrid>();
		blockgrid::Field CollisionOnly;
		Check(CollisionOnly.Apply(Writes), "collision-only grid fixture");
		Solid->RestoreField(std::move(CollisionOnly)); Solid->SetCanQuery(false); Solid->SetParent(B);
		const auto CollisionReceipt = B->PrepareSpatialDestination(Bounds).value();
		Check(!B->CommitSpatialTransfer(Actor, Movement, CollisionReceipt).Succeeded(), "non-queryable solid grid allowed a body overlap");
		Solid->SetCFrame(CFrame(65,0,0));
		const auto Clear = B->PrepareSpatialDestination(Bounds).value();
		Check(B->CommitSpatialTransfer(Actor, Movement, Clear).Succeeded() && Actor->GetObjectId() == Identity &&
			WorldRoot::GetEffectiveWorld(*Actor) == B && Actor->GetParent().value_or(nullptr) == Default &&
			Actor->GetCFrame().Position == glm::vec3(0) && Actor->GetLinearVelocity().x == 3.f &&
			!Ray(*A,20).HasHit() && Ray(*B,0).Instance == Actor,
			"successful checked handoff preserves identity, parent and queued motion while changing coordinates");
		B->StepPhysics(.02, std::nullopt);
		Check(Actor->GetLinearVelocity().x > 3.f, "a pending impulse must reach the destination safe point exactly once");
		const auto Previous = A->PrepareSpatialDestination(Bounds).value();
		A->SetParent(nullptr); A->SetParent(Default);
		Check(!A->ValidateSpatialDestination(Previous), "reactivation changed the semantic domain epoch");
		Game->Destroy();
	}

	void Run() {
		auto Game = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Common = std::make_shared<Folder>();
		Common->SetParent(Space);
		auto A = std::make_shared<SpatialRegion>(), B = std::make_shared<SpatialRegion>();
		A->SetParent(Space);
		B->SetParent(Common);
		Check(Game->GetSpatialRegions().size() == 2, "both domains registered in one DataModel");
		auto PublishedPart = std::make_shared<Part>();
		PublishedPart->SetPosition({90, 2, 0});
		PublishedPart->SetParent(Space);
		PublishedPart->SetLinearVelocity({3, 0, 0});
		Space->StepPhysics(.02, std::nullopt);
		Check(PublishedPart->GetLinearVelocity().x > 2.f, "source provider accepted the requested dynamic velocity");
		PublishedPart->SetAnchored(true);
		PublishedPart->SetSpatialRegion(A);
		Check(WorldRoot::GetEffectiveWorld(*PublishedPart) == A && PublishedPart->GetLinearVelocity() == glm::vec3(0),
			"a structural publish can anchor and route a previously dynamic body before the next physics step");
		PublishedPart->Destroy();
		const auto Wide = MakePart(Common, {4, 2, 4}, {5, 2, 0});
		const auto Tall = MakePart(Common, {1, 6, 1}, {5, 2, 0});
		const auto WideId = Wide->GetObjectId(), TallId = Tall->GetObjectId();
		Wide->SetSpatialRegion(A);
		Tall->SetSpatialRegion(B);
		Check(
			Wide->GetParent() == Tall->GetParent() && Ray(*A).Instance == Wide && Ray(*B).Instance == Tall,
			"ordinary sibling identity and scoped native hits"
		);
		Check(
			Ray(*A).Position.y == 3.f && Ray(*B).Position.y == 5.f && !Ray(*Space).HasHit() && !Ray(*B, 6.3f).HasHit(),
			"default world or foreign region leaked a body"
		);
		PhysicsQueryShape Query{
			.Kind = PhysicsQueryShapeKind::Box, .Transform = CFrame(5, 2, 0), .Size = glm::vec3(.1f)
		};
		Check(
			A->ResolveOverlap(Query).Parts == std::vector<std::shared_ptr<BasePart>>{Wide} &&
				B->ResolveOverlap(Query).Parts == std::vector<std::shared_ptr<BasePart>>{Tall} &&
				Space->ResolveOverlap(Query).Parts.empty(),
			"scoped shape overlap"
		);
		auto NativePart = MakePart(A, {2, 2, 2}, {20, 2, 0});
		Check(
			Ray(*A, 20).Instance == NativePart && !Ray(*Space, 20).HasHit() && !Ray(*B, 20).HasHit(),
			"nearest authored region is the only physical owner"
		);
		std::shared_ptr<WorldRoot> DuringRemoval;
		auto RemovalObserver = A->ChildRemoved->Connect([&](const std::shared_ptr<Instance> &Removed) {
			if (Removed == NativePart) DuringRemoval = WorldRoot::GetEffectiveWorld(*Removed);
		});
		NativePart->SetParent(B);
		RemovalObserver->Disconnect();
		Check(DuringRemoval == B, "the first reparent observer resolves the new route before destination admission callbacks");
		auto Preview = std::make_shared<ViewportFrame>();
		Preview->SetParent(A);
		const auto PreviewPart = MakePart(Preview, {1, 1, 1}, {30, 2, 0});
		Check(!WorldRoot::GetEffectiveWorld(*PreviewPart) && !Ray(*A, 30).HasHit(),
			"a structural viewport stops inherited domain routing");
		auto PreviewActor = std::make_shared<KinematicCharacter>();
		PreviewActor->SetParent(Preview);
		Check(PreviewActor->AdmitMotion(*Space, {}).Status == PhysicsOperationStatus::ResourceLimit,
			"a rootless character under a viewport cannot request movement from the default world");
		PreviewPart->SetParent(PreviewActor);
		PreviewActor->SetRootPart(PreviewPart);
		Check(PreviewActor->AdmitMotion(*Space, {}).Status == PhysicsOperationStatus::ResourceLimit,
			"a character with a viewport-only root cannot resolve contacts in the default world");
		PreviewActor->SetParent(A);
		PreviewPart->SetSpatialRegion(B);
		Check(PreviewActor->AdmitMotion(*Space, {}).Succeeded(),
			"a live explicitly assigned root retains its owning character above the native domain boundary");
		PreviewPart->SetParent(B);
		Check(PreviewActor->AdmitMotion(*Space, {}).Status == PhysicsOperationStatus::ResourceLimit,
			"a former root reparented outside its character cannot authorize motion");
		PreviewPart->Destroy();
		Check(PreviewActor->AdmitMotion(*Space, {}).Status == PhysicsOperationStatus::ResourceLimit,
			"a destroyed controller root cannot authorize a default-world fallback");
		PreviewActor->Destroy();
		Preview->Destroy();
		Check(
			Ray(*B, 20).Instance == NativePart && !Ray(*A, 20).HasHit() && !Ray(*Space, 20).HasHit(),
			"hierarchy transfer retires only the old owner"
		);
		const auto Camera = MakeLookAtRenderCameraInput({5, 4, 12}, {5, 2, 0}, {0, 1, 0}, 50);
		RenderPublisher PubA, PubB, PubDefault;
		RenderProjection DrawA, DrawB, DrawDefault;
		(void)DrawA.Apply(*PubA.Publish(*A, Camera, 128, 96));
		(void)DrawB.Apply(*PubB.Publish(*B, Camera, 128, 96));
		(void)DrawDefault.Apply(*PubDefault.Publish(*Space, Camera, 128, 96));
		Check(
			DrawA.GetItem(WideId) && !DrawA.GetItem(TallId) && DrawB.GetItem(TallId) && !DrawB.GetItem(WideId) &&
				!DrawDefault.GetItem(WideId) && !DrawDefault.GetItem(TallId),
			"chart render publications leaked"
		);
		Space->SetCurrentSpatialRegion(A);
		Check(Space->GetCurrentSpatialRegion() == A, "main view selected domain");
		const auto OtherGame = std::make_shared<DataModel>();
		const auto OtherSpace = std::dynamic_pointer_cast<Workspace>(OtherGame->GetService("Workspace"));
		const auto Other = std::make_shared<SpatialRegion>();
		Other->SetParent(OtherSpace);
		bool Denied = false;
		try {
			Tall->SetSpatialRegion(Other);
		} catch (const std::invalid_argument &) {
			Denied = true;
		}
		Check(Denied && Ray(*B).Instance == Tall && !Ray(*Other).HasHit(), "foreign DataModel assignment refused");
		const auto Nested = MakePart(Wide, {1, 1, 1}, {40, 2, 0});
		Check(
			Ray(*A, 40).Instance == Nested && !Ray(*Space, 40).HasHit(), "new submember inherits explicit affiliation"
		);
		const auto Independent = MakePart(Wide, {1, 1, 1}, {44, 2, 0});
		Independent->SetSpatialRegion(A);
		Independent->SetParent(Common);
		Check(Ray(*A, 44).Instance == Independent && !Ray(*Space, 44).HasHit(), "removing an explicit child from a bound ancestor keeps its owner");
		Independent->SetSpatialRegion(std::nullopt);
		Wide->SetSpatialRegion(B);
		Check(
			!Ray(*A).HasHit() && Ray(*B, 6.3f).Instance == Wide && Wide->GetObjectId() == WideId &&
				Ray(*B, 40).Instance == Nested && !Ray(*A, 40).HasHit(),
			"same-parent transfer retains subtree and identity"
		);
		(void)DrawA.Apply(*PubA.Publish(*A, Camera, 128, 96));
		(void)DrawB.Apply(*PubB.Publish(*B, Camera, 128, 96));
		Check(!DrawA.GetItem(WideId) && DrawB.GetItem(WideId), "old and new renderer notified of affiliation change");
		Tall->SetSpatialRegion(std::nullopt);
		Check(Ray(*Space).Instance == Tall && Ray(*B).Instance == Wide, "clear assignment returns to authored world");
		A->SetParent(nullptr);
		Check(!Space->GetCurrentSpatialRegion() && Game->GetSpatialRegions().size() == 1, "detached view reset");
		B->SetParent(nullptr);
		Denied = false;
		try { Tall->SetSpatialRegion(B); } catch (const std::invalid_argument &) { Denied = true; }
		Check(Denied && !Ray(*Space, 6.3f).HasHit() && !Ray(*B, 6.3f).HasHit(),
			"a detached target cannot accept assignments or retain explicitly routed bodies");
		B->SetParent(Space);
		Check(
			Ray(*B, 6.3f).Instance == Wide && !Ray(*Space, 6.3f).HasHit(),
			"target reattachment restores same-domain binding"
		);
		B->Destroy();
		Check(!Wide->GetSpatialRegion() && !WorldRoot::GetEffectiveWorld(*Nested) && !Ray(*Space, 6.3f).HasHit(),
			"a retired explicit boundary stays unavailable even through its live descendants");
		Wide->SetSpatialRegion(std::nullopt);
		Check(
			Ray(*Space, 6.3f).Instance == Wide && Ray(*Space, 40).Instance == Nested, "explicitly clear retired binding"
		);
		Game->Destroy();
		OtherGame->Destroy();
	}
}
int main() try {
	gargantuan::BootstrapNativeRuntimeSchema();
	PublicationIncarnation();
	Run();
	TerrainDomains();
	DestinationReadiness();
	std::cout << "[Spatial:Domains] Native physics, render, membership and teardown passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Domains] " << Error.what() << '\n';
	return 1;
}
