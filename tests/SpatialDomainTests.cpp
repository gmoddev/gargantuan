#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <iostream>
#include <stdexcept>

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

	void Run() {
		auto Game = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Common = std::make_shared<Folder>();
		Common->SetParent(Space);
		auto A = std::make_shared<SpatialRegion>(), B = std::make_shared<SpatialRegion>();
		A->SetParent(Space);
		B->SetParent(Common);
		Check(Game->GetSpatialRegions().size() == 2, "both domains registered in one DataModel");
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
		NativePart->SetParent(B);
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
		Check(!Wide->GetSpatialRegion() && !Ray(*Space, 6.3f).HasHit(), "retired explicit domain stays unavailable");
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
	Run();
	std::cout << "[Spatial:Domains] Native physics, render, membership and teardown passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Domains] " << Error.what() << '\n';
	return 1;
}
