#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
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
	void CreatorBinding() {
		const auto Game = std::make_shared<DataModel>();
		const auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		const auto Domain = std::make_shared<SpatialRegion>();
		Domain->SetName("Room");
		Domain->SetParent(World);
		const auto Shared = std::make_shared<ModuleScript>();
		Shared->SetName("Shared");
		Shared->SetSource("return {}");
		Shared->SetParent(World);
		const auto First = std::make_shared<Script>(), Other = std::make_shared<Script>();
		First->SetName("First");
		Other->SetName("Other");
		First->SetParent(World);
		Other->SetParent(World);
		First->SetRunContext(Enums::RunContext::Server);
		Other->SetRunContext(Enums::RunContext::Server);
		First->SetSource(R"lua(
local State = require("@game/Workspace/Shared")
local Room = game:GetService("Workspace"):FindFirstChild("Room")
local Lo, Hi = Vector3.new(-2,-2,-2), Vector3.new(2,2,2)
if State.First then
    assert(not pcall(function() State.First:Renew(2) end))
    local Fresh = assert(Room:RequestCollisionArea(Lo,Hi))
    Fresh:Release()
    assert(Fresh:GetStatus().Life == "Released")
    script:SetAttribute("Restarted",true)
    return
end
local First, Reason = Room:RequestCollisionArea(Lo,Hi,5,"Interactive")
assert(First and not Reason and typeof(First)=="SpatialCollisionLease")
assert(First:GetStatus().Collision=="Pending")
assert(not pcall(function() First:GetStatus().Collision="Ready" end))
local None, Invalid=Room:RequestCollisionArea(Hi,Lo,-1)
assert(None==nil and Invalid=="Invalid")
State.First=First
task.defer(function()
    local Snapshot=First:GetStatus()
    assert(Snapshot.Life=="Active" and Snapshot.Collision=="Ready" and type(Snapshot.Revision)=="string")
    assert(First:SetArea(Vector3.new(30,0,0),Vector3.new(33,3,3)) and First:SetPriority("Background") and First:Renew(4))
    assert(First:GetStatus().Collision=="Pending")
    script:SetAttribute("Deferred",true)
end)
)lua");
		Other->SetSource(R"lua(
local State = require("@game/Workspace/Shared")
assert(State.First and not pcall(function() State.First:Release() end))
local Room = game:GetService("Workspace"):FindFirstChild("Room")
local Mine = assert(Room:RequestCollisionArea(Vector3.new(-2,-2,-2),Vector3.new(2,2,2)))
State.Other=Mine
task.defer(function()
    assert(Mine:GetStatus().Collision=="Ready")
    script:SetAttribute("Independent",true)
end)
)lua");
		{
			ScriptEngine Runtime(Game);
			Runtime.RunBootstrapScript(First);
			Runtime.RunBootstrapScript(Other);
			if (First->Status == ScriptStatus::Error) throw std::runtime_error(First->ErrorMessage);
			if (Other->Status == ScriptStatus::Error) throw std::runtime_error(Other->ErrorMessage);
			const auto Both = Domain->PumpSpatialCollision(SpatialResidencyClock::now(), 1);
			Check(
				Both.Holders == 2 && Both.DistinctRequests == 1 && Both.Captures == 1,
				"managed server scripts must contribute independently to one native capture"
			);
			Runtime.Step();
			Check(
				First->GetAttributeValue("Deferred").has_value() && Other->GetAttributeValue("Independent").has_value(),
				"deferred callbacks lost their issuing script capability"
			);
			First->SetEnabled(false);
			First->SetEnabled(true);
			Runtime.RunBootstrapScript(First);
			if (First->Status == ScriptStatus::Error) throw std::runtime_error(First->ErrorMessage);
			Check(
				First->GetAttributeValue("Restarted").has_value(),
				"restarted script reused an old native collision grant"
			);
			const auto Left = Domain->PumpSpatialCollision(SpatialResidencyClock::now(), 2);
			Check(
				Left.Holders == 1 && Left.DistinctRequests == 1,
				"one script's retirement released another script's holder"
			);
			Runtime.ShutdownRuntime();
			Check(
				Domain->PumpSpatialCollision(SpatialResidencyClock::now(), 2).Holders == 0,
				"stopped runtime retained collision demand"
			);
		}
		const auto Client = std::make_shared<Script>();
		Client->SetName("Client");
		Client->SetRunContext(Enums::RunContext::Client);
		Client->SetParent(World);
		Client->SetSource(R"lua(
local Room=game:GetService("Workspace"):FindFirstChild("Room")
local Request,Reason=Room:RequestCollisionArea(Vector3.new(0,0,0),Vector3.new(2,2,2))
assert(Request==nil and Reason=="Unavailable")
)lua");
		{
			ScriptEngine Runtime(Game, {}, RuntimeMode::NetworkClient);
			Runtime.RunBootstrapScript(Client);
			if (Client->Status == ScriptStatus::Error) throw std::runtime_error(Client->ErrorMessage);
			Check(Client->Status == ScriptStatus::Finished, "client scoped denial did not run");
		}
		Game->Destroy();
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
	CreatorBinding();
	std::cout << "[Spatial:Residency] Shared native demand, independent audiences and lifecycle passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Spatial:Residency] " << Error.what() << '\n';
	return 1;
}
