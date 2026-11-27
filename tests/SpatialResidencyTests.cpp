#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	using namespace std::chrono_literals;
	void Check(bool Value, const char *Reason) {
		if (!Value) throw std::runtime_error(Reason);
	}
	void Run() {
		const auto Game = std::make_shared<DataModel>(), ForeignGame = std::make_shared<DataModel>();
		const auto Default = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		const auto A = std::make_shared<SpatialRegion>(), B = std::make_shared<SpatialRegion>();
		A->SetParent(Default);
		B->SetParent(Default);
		const auto View = std::make_shared<Folder>(), Peer = std::make_shared<Folder>(),
				   Tool = std::make_shared<Folder>();
		View->SetParent(Default);
		Peer->SetParent(Default);
		Tool->SetParent(Default);
		const auto Collider = std::make_shared<Part>();
		Collider->SetAnchored(true);
		Collider->SetParent(A);
		const SpatialResidencyAudience Author{View->GetObjectId(), 7}, Controller{Peer->GetObjectId(), 12},
			Editor{Tool->GetObjectId(), 1};
		const SpatialDestinationBounds Bounds{{-3, -3, -3}, {3, 3, 3}};
		const std::array Required{Collider->GetObjectId()};
		const auto Now = SpatialResidencyClock::now();
		const auto Request =
			[&](WorldRoot &Domain, SpatialResidencyAudience Audience, std::span<const ObjectId> Owners = {}) {
				return Domain.RequestSpatialCollision(Audience, Bounds, Owners, SpatialDemandPriority::Normal, 5s, Now);
			};
		const auto First = Request(*A, Author, Required).value(), Second = Request(*A, Controller, Required).value();
		Check(
			First->GetStatus(Now).Collision == SpatialCollisionCondition::Pending && !First->GetReceipt(Now),
			"request projection preceded the native admission"
		);
		const auto Work = A->PumpSpatialCollision(Now, 1);
		const auto Proof = First->GetReceipt(Now);
		Check(
			Work.Holders == 2 && Work.DistinctRequests == 1 && Work.Captures == 1 && Proof &&
				Proof == Second->GetReceipt(Now) && Proof->Domain == A->GetObjectId() && Proof->Providers.size() == 2,
			"two independent holders did not share one compatible native collision receipt"
		);
		const auto Other = Request(*B, Author, Required).value();
		B->StepPhysics(0, std::nullopt);
		Check(
			Other->GetStatus(Now).Collision == SpatialCollisionCondition::Unavailable && !Other->GetReceipt(Now),
			"foreign required owner was admitted in a coincident chart"
		);
		const auto Empty = Request(*B, Controller).value();
		B->StepPhysics(0, std::nullopt);
		Check(
			Empty->GetReceipt(Now) && Empty->GetReceipt(Now)->Providers.size() == 1 &&
				Empty->GetReceipt(Now)->Providers.front().AuthoritativeEmpty &&
				Empty->GetReceipt(Now)->Domain != Proof->Domain,
			"authoritative empty must come from the requested domain"
		);
		First->Release();
		First->Release();
		Check(
			First->GetStatus(Now).Life == SpatialLeaseLife::Released && !First->GetReceipt(Now) &&
				Second->GetReceipt(Now) == Proof && Second->Renew(Now, 8s) &&
				Second->SetPriority(Now, SpatialDemandPriority::Interactive),
			"release affected a different audience"
		);
		const auto Version = Second->GetStatus(Now).Revision;
		Collider->SetCFrame(CFrame(1, 0, 0));
		A->StepPhysics(0, std::nullopt);
		Check(
			Second->GetStatus(Now).Revision > Version && !A->ValidateSpatialDestination(*Proof) &&
				A->ValidateSpatialDestination(*Second->GetReceipt(Now)),
			"stale physics receipt persisted through a normal Main update"
		);
		const auto Stable = Second->GetReceipt(Now);
		(void)A->PumpSpatialCollision(Now, 4);
		Check(Stable == Second->GetReceipt(Now), "unchanged provider allocated a new immutable receipt");
		const auto Moving = Request(*A, Author, Required).value();
		const SpatialDestinationBounds Next{{60, 0, 0}, {63, 3, 3}};
		const auto BeforeMove = Moving->GetStatus(Now).Revision;
		Check(
			Moving->SetArea(Now, Next) && !Moving->GetReceipt(Now) && Moving->GetStatus(Now).Revision > BeforeMove &&
				Second->GetReceipt(Now) == Stable,
			"retargeting one moving view invalidated another view"
		);
		(void)A->PumpSpatialCollision(Now, 4);
		Check(
			Moving->GetReceipt(Now) && Moving->GetReceipt(Now)->Bounds == Next &&
				Moving->SetArea(Now, Bounds, Required) && Moving->GetReceipt(Now) == Stable,
			"moving holder did not rejoin an already resident compatible collision facet"
		);
		Moving->Release();
		const SpatialResidencyAudience Unknown{ForeignGame->GetObjectId(), 1};
		const auto Wrong = Request(*A, Unknown);
		Check(
			!Wrong && Wrong.error() == SpatialRequestFailure::Unavailable, "foreign audience acquired a local request"
		);
		std::vector<std::shared_ptr<SpatialCollisionLease>> Many;
		for (int I = 0; I != 8; ++I)
			Many.push_back(Request(*A, Editor).value());
		const auto Excess = Request(*A, Editor);
		Check(
			!Excess && Excess.error() == SpatialRequestFailure::Budget && Request(*A, Author).has_value(),
			"quota exhaustion escaped its audience"
		);
		for (const auto &Lease : Many)
			Lease->Release();
		A->RetireSpatialAudience(Controller);
		const auto Replay = Request(*A, Controller);
		Check(
			Second->GetStatus(Now).Life == SpatialLeaseLife::Stale && !Replay &&
				Replay.error() == SpatialRequestFailure::Stale,
			"retired audience epoch replayed"
		);
		const SpatialResidencyAudience Rejoined{Controller.Owner, Controller.Epoch + 1};
		const auto Fresh = Request(*A, Rejoined).value();
		(void)A->PumpSpatialCollision(Now, 1);
		Check(
			Fresh->GetReceipt(Now) != nullptr && Fresh->GetStatus(Now + 6s).Life == SpatialLeaseLife::Expired &&
				!Fresh->Renew(Now + 6s, 5s),
			"elapsed lease regained authority"
		);
		const auto Held = Request(*A, Author).value();
		A->SetParent(nullptr);
		A->SetParent(Default);
		Check(
			Held->GetStatus(Now).Life == SpatialLeaseLife::Stale && !Held->GetReceipt(Now) &&
				!A->ValidateSpatialDestination(*Stable),
			"reattachment revived a previous domain generation"
		);
		Game->Destroy();
		ForeignGame->Destroy();
	}
}
int main() try {
	gargantuan::BootstrapNativeRuntimeSchema();
	Run();
	std::cout << "[Spatial:Residency] Shared native demand, independent audiences and lifecycle passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Residency] " << Error.what() << '\n';
	return 1;
}
