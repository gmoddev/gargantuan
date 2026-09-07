#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/Players.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Contains(const std::vector<ObjectId> &Objects, ObjectId Object) {
		return std::ranges::binary_search(Objects, Object);
	}
}

int main() {
	BootstrapNativeRuntimeSchema();
	auto World = std::make_shared<DataModel>();
	HeadlessRenderer Renderer(Vector2(64, 64));
	Engine Runtime(
		World,
		&Renderer,
		nullptr,
		EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
	);
	Runtime.ProcessService->Alive = true;
	auto LocalPlayer = Runtime.Players->CreateSessionPlayer({"relevance-test", "local"});
	auto RemotePlayer = Runtime.Players->CreateSessionPlayer({"relevance-test", "remote"});
	auto LocalCharacter = LocalPlayer->GetCharacter()
							  ? std::dynamic_pointer_cast<Character>(*LocalPlayer->GetCharacter())
							  : nullptr;
	auto RemoteCharacter = RemotePlayer->GetCharacter()
							   ? std::dynamic_pointer_cast<Character>(*RemotePlayer->GetCharacter())
							   : nullptr;
	Check(LocalCharacter && RemoteCharacter, "server fixture creates two authoritative Characters");
	if (!LocalCharacter || !RemoteCharacter) return 1;
	Check(
		LocalPlayer->FindProperty("Character")->MaterializationDependencyPolicy ==
				InstanceProperty::MaterializationDependency::Hard &&
			LocalCharacter->FindProperty("RootPart")->MaterializationDependencyPolicy ==
				InstanceProperty::MaterializationDependency::Soft,
		"schema distinguishes hard Player.Character from soft Character.RootPart materialization edges"
	);
	LocalCharacter->SetPosition({0.0f, 6.0f, 0.0f});
	RemoteCharacter->SetPosition({1000.0f, 6.0f, 0.0f});
	auto NpcCharacter = std::make_shared<KinematicCharacter>();
	NpcCharacter->SetPosition({1200.0f, 6.0f, 0.0f});
	NpcCharacter->SetParent(World);

	ReplicationRelevance Relevance(World);
	const ConnectionId Connection{1, 1};
	Check(
		Relevance.AddPeer(Connection, LocalPlayer->GetObjectId(), LocalCharacter->GetObjectId()),
		"server relevance accepts an authoritative peer identity"
	);
	const auto *Initial = Relevance.GetSelection(Connection);
	Check(
		Initial && Contains(Initial->RequiredObjects, LocalCharacter->GetObjectId()),
		"controlled Character is an owner-required relevance reason"
	);
	Check(
		Initial && !Contains(Initial->DesiredObjects, RemoteCharacter->GetRootPart().value()->GetObjectId()),
		"distant remote Character descendants are outside initial interest"
	);
	Check(
		Initial && !Contains(Initial->DesiredObjects, NpcCharacter->GetObjectId()),
		"a distant NPC is irrelevant without requiring a Player identity"
	);

	ReplicationCoordinator Coordinator(World);
	auto Baseline = Initial ? Coordinator.AddPeer(Connection, ReplicationEpoch(1), *Initial)
							: ReplicationProduceResult{};
	ReplicaApplier Replica;
	Check(
		Baseline.Succeeded() && Replica.ApplyFrame(*Baseline.Frame).Succeeded(),
		"peer-specific dependency closure produces a valid structural baseline"
	);
	const auto InitialKnownObjects = Coordinator.GetView(Connection)
										 ? Coordinator.GetView(Connection)->KnownObjects.size()
										 : 0;
	auto ReplicaRemotePlayer = std::dynamic_pointer_cast<Player>(Replica.Resolve(RemotePlayer->GetObjectId()));
	auto ReplicaRemoteCharacter = ReplicaRemotePlayer && ReplicaRemotePlayer->GetCharacter()
									  ? std::dynamic_pointer_cast<Character>(*ReplicaRemotePlayer->GetCharacter())
									  : nullptr;
	Check(
		ReplicaRemoteCharacter && !ReplicaRemoteCharacter->GetRootPart(),
		"global remote Player keeps a Character shell with an unresolved soft RootPart"
	);
	Check(
		Replica.Resolve(LocalCharacter->GetRootPart().value()->GetObjectId()) != nullptr,
		"owner-required Character materializes its structural descendants"
	);
	const auto *OwnerView = Coordinator.GetView(Connection);
	Check(
		OwnerView && OwnerView->RelevantObjects.contains(LocalCharacter->GetObjectId()),
		"dependency-closed owner Character remains marked relevant after baseline publication"
	);
	Check(
		LocalCharacter->ApplyAttributeMutation(
			"OwnerServerProof", WireValue(true), ScriptSecurityContext::CoreTrusted()
		) == MutationStatus::Success,
		"server fixture mutates the owner-required Character after materialization"
	);
	auto OwnerAttribute = Coordinator.ProduceIncremental(Connection);
	if (!OwnerAttribute.Succeeded())
		std::cerr << "[Replication:RelevanceTest] owner Attribute production failed: " << OwnerAttribute.Error << '\n';
	Check(
		OwnerAttribute.Succeeded(), "owner-required Character remains eligible for incremental structural publication"
	);
	bool OwnerAttributeApplied = false;
	if (OwnerAttribute.Frame) {
		auto Applied = Replica.ApplyFrame(*OwnerAttribute.Frame);
		OwnerAttributeApplied = Applied.Succeeded();
		if (!OwnerAttributeApplied)
			std::cerr << "[Replication:RelevanceTest] owner Attribute application failed: " << Applied.Message << '\n';
	}
	Check(OwnerAttributeApplied, "owner-required Character Attribute applies to the replica");
	auto ReplicaLocalCharacter = std::dynamic_pointer_cast<Character>(Replica.Resolve(LocalCharacter->GetObjectId()));
	Check(
		ReplicaLocalCharacter && ReplicaLocalCharacter->GetAttributeValue("OwnerServerProof") == WireValue(true),
		"a server-authored Attribute reaches the materialized controlled Character"
	);

	auto OffInterestPart = std::make_shared<Part>();
	OffInterestPart->SetCFrame(CFrame(5'000.0f, 0.0f, 0.0f));
	OffInterestPart->SetParent(World);
	OffInterestPart->SetCFrame(CFrame(5'100.0f, 0.0f, 0.0f));
	const auto OperationsBeforeSkippedState = Coordinator.GetMetrics().OperationsGenerated;
	auto SkippedState = Coordinator.ProduceIncremental(Connection);
	Check(
		!SkippedState.Succeeded() && SkippedState.Error == "No relevant replication changes are available" &&
			Coordinator.GetMetrics().OperationsGenerated == OperationsBeforeSkippedState &&
			Coordinator.GetMetrics().ReplicationBacklog == 0,
		"off-interest property journals advance without network publication or retained delta backlog"
	);
	RemoteCharacter->SetPosition({100.0f, 6.0f, 0.0f});
	NpcCharacter->SetPosition({120.0f, 6.0f, 0.0f});
	Check(Relevance.Update(1), "spatial relevance updates after a remote Character moves");
	auto Enter = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
	Check(
		Enter.Succeeded() && Replica.ApplyFrame(*Enter.Frame).Succeeded(),
		"remote Character enter materializes a bounded structural subtree"
	);
	Check(
		Relevance.WasSelectionEvaluated(Connection) && Relevance.Update(2) &&
			!Relevance.WasSelectionEvaluated(Connection),
		"unchanged relevance does not request dependency-closure work between cadence ticks"
	);
	ReplicaRemoteCharacter = std::dynamic_pointer_cast<Character>(Replica.Resolve(RemoteCharacter->GetObjectId()));
	Check(
		ReplicaRemoteCharacter && ReplicaRemoteCharacter->GetRootPart(),
		"soft Character.RootPart resolves when its target enters relevance"
	);
	Check(
		Replica.Resolve(NpcCharacter->GetObjectId()) != nullptr,
		"a Player-independent NPC enters through the shared Character relevance boundary"
	);

	RemoteCharacter->SetPosition({280.0f, 6.0f, 0.0f});
	Check(Relevance.Update(7), "hysteresis fixture updates at the bounded cadence");
	auto Hysteresis = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
	Check(
		!Hysteresis.Succeeded() && Hysteresis.Error == "No replication relevance changes are available",
		"an object between enter and leave radii remains resident"
	);

	RemoteCharacter->SetPosition({400.0f, 6.0f, 0.0f});
	NpcCharacter->SetPosition({1200.0f, 6.0f, 0.0f});
	Check(Relevance.Update(13), "leave-radius movement is observed");
	auto Leave = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
	Check(
		Leave.Succeeded() && Replica.ApplyFrame(*Leave.Frame).Succeeded(),
		"remote Character leave safely unpublishes its structural subtree"
	);
	ReplicaRemotePlayer = std::dynamic_pointer_cast<Player>(Replica.Resolve(RemotePlayer->GetObjectId()));
	ReplicaRemoteCharacter = ReplicaRemotePlayer && ReplicaRemotePlayer->GetCharacter()
								 ? std::dynamic_pointer_cast<Character>(*ReplicaRemotePlayer->GetCharacter())
								 : nullptr;
	Check(
		ReplicaRemoteCharacter && !ReplicaRemoteCharacter->GetRootPart(),
		"remote Player.Character remains stable while expensive descendants leave"
	);
	Check(
		Replica.Resolve(NpcCharacter->GetObjectId()) == nullptr,
		"a Player-independent NPC unpublishes without an authoritative destroy"
	);

	RemoteCharacter->SetPosition({100.0f, 6.0f, 0.0f});
	NpcCharacter->SetPosition({120.0f, 6.0f, 0.0f});
	Check(Relevance.Update(19), "reentry movement is observed");
	auto Reenter = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
	Check(
		Reenter.Succeeded() && Replica.ApplyFrame(*Reenter.Frame).Succeeded(),
		"same authoritative ObjectIds can reenter after peer-specific unpublish"
	);
	Check(
		Replica.Resolve(RemoteCharacter->GetRootPart().value()->GetObjectId()) != nullptr,
		"reentry reconstructs current authoritative structure exactly once"
	);
	Check(
		Replica.Resolve(NpcCharacter->GetObjectId()) != nullptr,
		"a Player-independent NPC reenters through the same shared relevance result"
	);

	const auto Metrics = Relevance.GetMetrics();
	Check(
		Metrics.RelevanceEnters >= 1 && Metrics.RelevanceLeaves >= 1 && Metrics.SpatialQueries >= 3,
		"interest enter, leave, and spatial-query work remain observable"
	);
	Check(
		InitialKnownObjects < World->GetDescendants().size() + 1,
		"peer materialization remains smaller than the authoritative world when interest excludes objects"
	);

	const std::array FarFocus{glm::vec3(50'000.0f, 6.0f, 0.0f)};
	Check(
		Relevance.SetTrustedFocus(Connection, FarFocus) && Relevance.Update(25) &&
			Contains(Relevance.GetSelection(Connection)->DesiredObjects, LocalCharacter->GetObjectId()),
		"removing an owner's spatial reason retains the Character through its mandatory control reason"
	);
	Check(
		Relevance.SetOwnerCharacter(Connection, {}) && Relevance.Update(31) &&
			!Contains(Relevance.GetSelection(Connection)->DesiredObjects, LocalCharacter->GetObjectId()) &&
			Contains(Relevance.GetSelection(Connection)->DesiredObjects, LocalPlayer->GetObjectId()),
		"removing the final owner reason releases the Character while the session Player remains mandatory"
	);

	auto OnlyFirst = std::make_shared<Part>();
	OnlyFirst->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
	OnlyFirst->SetParent(World);
	auto OnlySecond = std::make_shared<Part>();
	OnlySecond->SetCFrame(CFrame(1'000.0f, 0.0f, 0.0f));
	OnlySecond->SetParent(World);
	auto Shared = std::make_shared<Part>();
	Shared->SetCFrame(CFrame(500.0f, 0.0f, 0.0f));
	Shared->SetParent(World);
	auto Neither = std::make_shared<Part>();
	Neither->SetCFrame(CFrame(5'000.0f, 0.0f, 0.0f));
	Neither->SetParent(World);
	ReplicationRelevance Differential(World);
	const ConnectionId FirstDifferential{8, 1};
	const ConnectionId SecondDifferential{9, 1};
	const std::array FirstFocus{glm::vec3(0.0f), glm::vec3(500.0f, 0.0f, 0.0f)};
	const std::array SecondFocus{glm::vec3(1'000.0f, 0.0f, 0.0f), glm::vec3(500.0f, 0.0f, 0.0f)};
	Check(
		Differential.AddPeer(FirstDifferential, LocalPlayer->GetObjectId(), LocalCharacter->GetObjectId()) &&
			Differential.AddPeer(SecondDifferential, RemotePlayer->GetObjectId(), RemoteCharacter->GetObjectId()) &&
			Differential.SetTrustedFocus(FirstDifferential, FirstFocus) &&
			Differential.SetTrustedFocus(SecondDifferential, SecondFocus) && Differential.Update(1),
		"two authoritative peers accept independent bounded focus sets"
	);
	const auto *FirstSelection = Differential.GetSelection(FirstDifferential);
	const auto *SecondSelection = Differential.GetSelection(SecondDifferential);
	Check(
		FirstSelection && SecondSelection && Contains(FirstSelection->DesiredObjects, OnlyFirst->GetObjectId()) &&
			!Contains(FirstSelection->DesiredObjects, OnlySecond->GetObjectId()) &&
			Contains(FirstSelection->DesiredObjects, Shared->GetObjectId()) &&
			!Contains(FirstSelection->DesiredObjects, Neither->GetObjectId()) &&
			!Contains(SecondSelection->DesiredObjects, OnlyFirst->GetObjectId()) &&
			Contains(SecondSelection->DesiredObjects, OnlySecond->GetObjectId()) &&
			Contains(SecondSelection->DesiredObjects, Shared->GetObjectId()) &&
			!Contains(SecondSelection->DesiredObjects, Neither->GetObjectId()),
		"two peers select first-only, second-only, shared, and neither spatial objects deterministically"
	);

	auto RegionWorld = std::make_shared<DataModel>();
	auto RegionIdentity = std::make_shared<Folder>();
	RegionIdentity->SetParent(RegionWorld);
	auto BoundaryPart = std::make_shared<Part>();
	BoundaryPart->SetCFrame(CFrame(10.0f, 0.0f, 0.0f));
	BoundaryPart->SetParent(RegionWorld);
	auto LargePart = std::make_shared<Part>();
	LargePart->SetSize({20'000.0f, 20'000.0f, 20'000.0f});
	LargePart->SetCFrame(CFrame(200.0f, 0.0f, 0.0f));
	LargePart->SetParent(RegionWorld);
	auto RuntimeCharacter = std::make_shared<KinematicCharacter>();
	RuntimeCharacter->SetPosition({20.0f, 0.0f, 0.0f});
	RuntimeCharacter->SetParent(RegionWorld);
	ReplicationRelevance RegionRelevance(RegionWorld);
	const ConnectionId RegionConnection{12, 1};
	const std::array RegionFocus{glm::vec3(100.0f, 0.0f, 0.0f)};
	Check(
		RegionRelevance.AddPeer(RegionConnection, RegionIdentity->GetObjectId()) &&
			RegionRelevance.SetTrustedFocus(RegionConnection, RegionFocus) && RegionRelevance.Update(1) &&
			RegionRelevance.IsLargeSpatialObject(LargePart->GetObjectId()) &&
			Contains(RegionRelevance.GetSelection(RegionConnection)->DesiredObjects, LargePart->GetObjectId()),
		"bounded large-object fallback remains a conservative 3E candidate"
	);
	const auto BeforeSameRegion = RegionRelevance.GetMetrics();
	BoundaryPart->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
	Check(
		RegionRelevance.Update(7) && RegionRelevance.GetMetrics().SpatialMoves == BeforeSameRegion.SpatialMoves &&
			RegionRelevance.GetMetrics().SameRegionUpdates > BeforeSameRegion.SameRegionUpdates,
		"committed same-region BasePart movement does not mutate region buckets"
	);
	const auto EntersBeforeBoundary = RegionRelevance.GetMetrics().RelevanceEnters;
	const auto LeavesBeforeBoundary = RegionRelevance.GetMetrics().RelevanceLeaves;
	BoundaryPart->SetCFrame(CFrame(129.0f, 0.0f, 0.0f));
	Check(
		!RegionRelevance.VerifySpatialIndex(),
		"semantic movement is observably dirty until the established projection synchronization safe point"
	);
	Check(
		RegionRelevance.Update(13) &&
			RegionRelevance.GetSpatialCellAddress(BoundaryPart->GetObjectId()) ==
				SpatialCellAddress{DefaultSpatialSpace, {1, 0, 0}} &&
			Contains(RegionRelevance.GetSelection(RegionConnection)->DesiredObjects, BoundaryPart->GetObjectId()) &&
			RegionRelevance.GetMetrics().RelevanceEnters == EntersBeforeBoundary &&
			RegionRelevance.GetMetrics().RelevanceLeaves == LeavesBeforeBoundary,
		"region crossing while inside 3E relevance causes no semantic enter/leave churn"
	);
	RuntimeCharacter->ApplyRuntimeTransform(CFrame(257.0f, 0.0f, 0.0f));
	Check(
		RegionRelevance.Update(19) && RegionRelevance.GetSpatialCellAddress(RuntimeCharacter->GetObjectId()) ==
										  SpatialCellAddress{DefaultSpatialSpace, {2, 0, 0}},
		"runtime Character movement uses the same post-authority region membership path"
	);
	auto NewParent = std::make_shared<Folder>();
	NewParent->SetParent(RegionWorld);
	BoundaryPart->SetParent(NewParent);
	Check(
		RegionRelevance.Update(25) &&
			RegionRelevance.GetSpatialCellAddress(BoundaryPart->GetObjectId()) ==
				SpatialCellAddress{DefaultSpatialSpace, {1, 0, 0}} &&
			RegionRelevance.VerifySpatialIndex(),
		"reparenting reconstructs canonical spatial-root membership without stale entries"
	);
	const auto LargeId = LargePart->GetObjectId();
	LargePart->Destroy();
	Check(
		RegionRelevance.Update(31) && !RegionRelevance.IsLargeSpatialObject(LargeId) &&
			!Contains(RegionRelevance.GetSelection(RegionConnection)->DesiredObjects, LargeId) &&
			RegionRelevance.VerifySpatialIndex(),
		"destroy removes large fallback and generation-bearing membership before later selection"
	);
	ReplicationRelevanceConfiguration InvalidRegionConfiguration;
	InvalidRegionConfiguration.RegionSize = 0.0;
	Check(!InvalidRegionConfiguration.IsValid(), "production relevance rejects a zero region size");
	InvalidRegionConfiguration = {};
	InvalidRegionConfiguration.MaximumSpatialRegions = MaximumReplicationSpatialRegions + 1;
	Check(!InvalidRegionConfiguration.IsValid(), "production relevance rejects regions beyond its hard ceiling");
	InvalidRegionConfiguration = {};
	InvalidRegionConfiguration.MaximumQueryMembershipVisits = 0;
	Check(!InvalidRegionConfiguration.IsValid(), "production relevance rejects a zero candidate-visit budget");

	ReplicationRelevanceConfiguration Limited;
	Limited.MaximumQueryRegions = 1;
	ReplicationRelevance Bounded(World, {}, Limited);
	Check(
		!Bounded.AddPeer({2, 1}, LocalPlayer->GetObjectId(), LocalCharacter->GetObjectId()) && !Bounded.IsHealthy() &&
			Bounded.GetMetrics().LimitFailures == 1,
		"pathological spatial queries fail coherently at a hard work bound"
	);

	auto BudgetWorld = std::make_shared<DataModel>();
	ReplicationCoordinator BudgetCoordinator(BudgetWorld);
	const ConnectionId BudgetConnection{3, 1};
	PeerRelevanceSelection RootOnly{
		.RequiredObjects = {BudgetWorld->GetObjectId()},
		.DesiredObjects = {BudgetWorld->GetObjectId()},
	};
	auto BudgetBaseline = BudgetCoordinator.AddPeer(BudgetConnection, ReplicationEpoch(1), RootOnly);
	ReplicaApplier BudgetReplica;
	Check(
		BudgetBaseline.Succeeded() && BudgetReplica.ApplyFrame(*BudgetBaseline.Frame).Succeeded(),
		"budget fixture begins with a minimal materialized root"
	);
	std::vector<std::shared_ptr<Folder>> DenseObjects;
	DenseObjects.reserve(MaximumRelevanceTransitionsPerFrame + 5);
	PeerRelevanceSelection DenseSelection = RootOnly;
	for (std::size_t Index = 0; Index < MaximumRelevanceTransitionsPerFrame + 5; ++Index) {
		auto Object = std::make_shared<Folder>();
		Object->SetParent(BudgetWorld);
		DenseSelection.DesiredObjects.push_back(Object->GetObjectId());
		DenseObjects.push_back(std::move(Object));
	}
	std::ranges::sort(DenseSelection.DesiredObjects);
	auto DenseEnterFirst = BudgetCoordinator.UpdateRelevance(BudgetConnection, DenseSelection);
	Check(
		DenseEnterFirst.Succeeded() &&
			DenseEnterFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			BudgetReplica.ApplyFrame(*DenseEnterFirst.Frame).Succeeded() &&
			BudgetCoordinator.GetMetrics().MaterializationBacklog == 5 &&
			BudgetCoordinator.HasPendingRelevance(BudgetConnection),
		"dense enter is deterministically budgeted with an observable bounded backlog"
	);
	auto DenseEnterSecond = BudgetCoordinator.UpdateRelevance(BudgetConnection, DenseSelection);
	Check(
		DenseEnterSecond.Succeeded() && DenseEnterSecond.Frame->Operations.size() == 5 &&
			BudgetReplica.ApplyFrame(*DenseEnterSecond.Frame).Succeeded() &&
			BudgetCoordinator.GetMetrics().MaterializationBacklog == 0 &&
			!BudgetCoordinator.HasPendingRelevance(BudgetConnection),
		"dense enter completes over subsequent bounded transition frames"
	);
	auto DenseLeaveFirst = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	Check(
		DenseLeaveFirst.Succeeded() &&
			DenseLeaveFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			BudgetReplica.ApplyFrame(*DenseLeaveFirst.Frame).Succeeded(),
		"peer unpublish uses the same bounded work budget without destroying server objects"
	);
	DenseObjects.back()->Destroy();
	auto DenseLeaveSecond = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	std::size_t DestroyOperations = 0;
	if (DenseLeaveSecond.Frame)
		DestroyOperations = std::ranges::count_if(DenseLeaveSecond.Frame->Operations, [](const auto &Operation) {
			return std::holds_alternative<DestroyReplication>(Operation.Intent);
		});
	Check(
		DenseLeaveSecond.Succeeded() && DenseLeaveSecond.Frame->Operations.size() == 5 && DestroyOperations == 1 &&
			BudgetReplica.ApplyFrame(*DenseLeaveSecond.Frame).Succeeded() && DenseObjects.back()->GetDestroyed() &&
			std::ranges::all_of(
				std::span(DenseObjects).first(DenseObjects.size() - 1),
				[](const auto &Object) { return !Object->GetDestroyed(); }
			),
		"destroy during a budgeted leave is distinguished from peer unpublish without affecting live server objects"
	);

	auto SharedWorld = std::make_shared<DataModel>();
	auto SharedAncestor = std::make_shared<Folder>();
	SharedAncestor->SetParent(SharedWorld);
	auto SharedFirst = std::make_shared<Folder>();
	SharedFirst->SetParent(SharedAncestor);
	auto SharedSecond = std::make_shared<Folder>();
	SharedSecond->SetParent(SharedAncestor);
	ReplicationCoordinator SharedCoordinator(SharedWorld);
	const ConnectionId SharedConnection{6, 1};
	PeerRelevanceSelection SharedSelection{
		.RequiredObjects = {SharedWorld->GetObjectId()},
		.DesiredObjects = {SharedFirst->GetObjectId(), SharedSecond->GetObjectId()},
	};
	auto SharedBaseline = SharedCoordinator.AddPeer(SharedConnection, ReplicationEpoch(1), SharedSelection);
	ReplicaApplier SharedReplica;
	Check(
		SharedBaseline.Succeeded() && SharedReplica.ApplyFrame(*SharedBaseline.Frame).Succeeded(),
		"two relevant objects materialize one shared ancestor closure"
	);
	SharedSelection.DesiredObjects = {SharedSecond->GetObjectId()};
	auto SharedLeave = SharedCoordinator.UpdateRelevance(SharedConnection, SharedSelection);
	const auto *SharedView = SharedCoordinator.GetView(SharedConnection);
	Check(
		SharedLeave.Succeeded() && SharedLeave.Frame->Operations.size() == 1 &&
			SharedReplica.ApplyFrame(*SharedLeave.Frame).Succeeded() && SharedView &&
			!SharedView->Knows(SharedFirst->GetObjectId()) && SharedView->Knows(SharedSecond->GetObjectId()) &&
			SharedView->Knows(SharedAncestor->GetObjectId()),
		"one dependent can leave without unpublishing an ancestor still required by another object"
	);

	auto EnterRaceWorld = std::make_shared<DataModel>();
	ReplicationCoordinator EnterRaceCoordinator(EnterRaceWorld);
	const ConnectionId EnterRaceConnection{7, 1};
	PeerRelevanceSelection EnterRaceSelection{
		.RequiredObjects = {EnterRaceWorld->GetObjectId()},
		.DesiredObjects = {EnterRaceWorld->GetObjectId()},
	};
	auto EnterRaceBaseline = EnterRaceCoordinator.AddPeer(EnterRaceConnection, ReplicationEpoch(1), EnterRaceSelection);
	std::vector<std::shared_ptr<Folder>> EnterRaceObjects;
	EnterRaceObjects.reserve(MaximumRelevanceTransitionsPerFrame + 1);
	for (std::size_t Index = 0; Index < MaximumRelevanceTransitionsPerFrame + 1; ++Index) {
		auto Object = std::make_shared<Folder>();
		Object->SetParent(EnterRaceWorld);
		EnterRaceSelection.DesiredObjects.push_back(Object->GetObjectId());
		EnterRaceObjects.push_back(std::move(Object));
	}
	std::ranges::sort(EnterRaceSelection.DesiredObjects);
	auto EnterRaceFirst = EnterRaceCoordinator.UpdateRelevance(EnterRaceConnection, EnterRaceSelection);
	const auto *EnterRaceFirstView = EnterRaceCoordinator.GetView(EnterRaceConnection);
	std::shared_ptr<Folder> PendingEnterObject;
	if (EnterRaceFirstView)
		for (const auto &Object : EnterRaceObjects)
			if (!EnterRaceFirstView->Knows(Object->GetObjectId())) {
				Check(!PendingEnterObject, "pending-enter fixture has only one budgeted target");
				PendingEnterObject = Object;
			}
	Check(PendingEnterObject != nullptr, "pending-enter fixture discovers its ObjectId-ordered backlog target");
	if (PendingEnterObject) PendingEnterObject->Destroy();
	auto EnterRaceSecond = EnterRaceCoordinator.UpdateRelevance(EnterRaceConnection, EnterRaceSelection);
	const auto *EnterRaceView = EnterRaceCoordinator.GetView(EnterRaceConnection);
	Check(EnterRaceBaseline.Succeeded(), "pending-enter destruction fixture produces its baseline");
	Check(
		EnterRaceFirst.Succeeded() && EnterRaceFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame,
		"pending-enter destruction fixture leaves exactly one target behind its budget"
	);
	Check(
		!EnterRaceSecond.Succeeded() && EnterRaceSecond.Error == "No replication relevance changes are available",
		"destroying the pending target produces no stale lifecycle work"
	);
	Check(
		EnterRaceView && PendingEnterObject && !EnterRaceView->Knows(PendingEnterObject->GetObjectId()),
		"destroyed pending target never enters the peer's materialized view"
	);

	PeerRelevanceSelection DependencySelection = RootOnly;
	std::vector<std::shared_ptr<Folder>> DependencyFillers;
	DependencyFillers.reserve(MaximumRelevanceTransitionsPerFrame - 1);
	for (std::size_t Index = 0; Index < MaximumRelevanceTransitionsPerFrame - 1; ++Index) {
		auto Object = std::make_shared<Folder>();
		Object->SetParent(BudgetWorld);
		DependencySelection.DesiredObjects.push_back(Object->GetObjectId());
		DependencyFillers.push_back(std::move(Object));
	}
	auto DependencyCharacter = std::make_shared<KinematicCharacter>();
	DependencyCharacter->SetParent(BudgetWorld);
	auto DependencyPlayer = std::make_shared<Player>();
	DependencyPlayer->SetParent(BudgetWorld);
	DependencyPlayer->SetCharacter(DependencyCharacter);
	DependencySelection.RequiredObjects.push_back(DependencyPlayer->GetObjectId());
	DependencySelection.DesiredObjects.push_back(DependencyPlayer->GetObjectId());
	std::ranges::sort(DependencySelection.DesiredObjects);
	auto DependencyEnterFirst = BudgetCoordinator.UpdateRelevance(BudgetConnection, DependencySelection);
	auto DependencyEnterSecond = BudgetCoordinator.UpdateRelevance(BudgetConnection, DependencySelection);
	const auto DependencyEnterFirstApplied = DependencyEnterFirst.Frame
												 ? BudgetReplica.ApplyFrame(*DependencyEnterFirst.Frame)
												 : ReplicaApplyResult{};
	const auto DependencyEnterSecondApplied = DependencyEnterSecond.Frame
												  ? BudgetReplica.ApplyFrame(*DependencyEnterSecond.Frame)
												  : ReplicaApplyResult{};
	Check(
		DependencyEnterFirst.Succeeded() &&
			DependencyEnterFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			DependencyEnterFirstApplied.Succeeded() && DependencyEnterSecond.Succeeded() &&
			DependencyEnterSecond.Frame->Operations.size() == 1 && DependencyEnterSecondApplied.Succeeded() &&
			BudgetReplica.Resolve(DependencyPlayer->GetObjectId()) &&
			BudgetReplica.Resolve(DependencyCharacter->GetObjectId()),
		"owner-required hard-reference group is prioritized and never publishes a referrer before its target"
	);
	auto DependencyLeaveFirst = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	auto DependencyLeaveSecond = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	const auto DependencyLeaveFirstApplied = DependencyLeaveFirst.Frame
												 ? BudgetReplica.ApplyFrame(*DependencyLeaveFirst.Frame)
												 : ReplicaApplyResult{};
	const auto DependencyLeaveSecondApplied = DependencyLeaveSecond.Frame
												  ? BudgetReplica.ApplyFrame(*DependencyLeaveSecond.Frame)
												  : ReplicaApplyResult{};
	Check(
		DependencyLeaveFirst.Succeeded() &&
			DependencyLeaveFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			DependencyLeaveFirstApplied.Succeeded() && DependencyLeaveSecond.Succeeded() &&
			DependencyLeaveSecond.Frame->Operations.size() == 1 && DependencyLeaveSecondApplied.Succeeded(),
		"hard-reference leave orders dependents ahead of targets across a transition budget boundary"
	);

	auto ReplacementWorld = std::make_shared<DataModel>();
	auto ReplacementPlayer = std::make_shared<Player>();
	ReplacementPlayer->SetParent(ReplacementWorld);
	auto OldReplacementCharacter = std::make_shared<KinematicCharacter>();
	OldReplacementCharacter->SetParent(ReplacementWorld);
	ReplacementPlayer->SetCharacter(OldReplacementCharacter);
	PeerRelevanceSelection ReplacementBaselineSelection{
		.RequiredObjects = {ReplacementWorld->GetObjectId(), ReplacementPlayer->GetObjectId()},
		.DesiredObjects = {ReplacementWorld->GetObjectId(), ReplacementPlayer->GetObjectId()},
	};
	std::vector<std::shared_ptr<Folder>> ReplacementFillers;
	ReplacementFillers.reserve(MaximumRelevanceTransitionsPerFrame);
	for (std::size_t Index = 0; Index < MaximumRelevanceTransitionsPerFrame; ++Index) {
		auto Object = std::make_shared<Folder>();
		Object->SetParent(ReplacementWorld);
		ReplacementBaselineSelection.DesiredObjects.push_back(Object->GetObjectId());
		ReplacementFillers.push_back(std::move(Object));
	}
	std::ranges::sort(ReplacementBaselineSelection.DesiredObjects);
	ReplicationCoordinator ReplacementCoordinator(ReplacementWorld);
	const ConnectionId ReplacementConnection{10, 1};
	auto ReplacementBaseline = ReplacementCoordinator.AddPeer(
		ReplacementConnection, ReplicationEpoch(1), ReplacementBaselineSelection
	);
	ReplicaApplier ReplacementReplica;
	Check(
		ReplacementBaseline.Succeeded() && ReplacementReplica.ApplyFrame(*ReplacementBaseline.Frame).Succeeded(),
		"Character replacement fixture begins with a hard-referenced Character and dense known set"
	);
	auto NewReplacementCharacter = std::make_shared<KinematicCharacter>();
	NewReplacementCharacter->SetParent(ReplacementWorld);
	ReplacementPlayer->SetCharacter(NewReplacementCharacter);
	PeerRelevanceSelection ReplacementSelection{
		.RequiredObjects = {ReplacementWorld->GetObjectId(), ReplacementPlayer->GetObjectId()},
		.DesiredObjects = {ReplacementWorld->GetObjectId(), ReplacementPlayer->GetObjectId()},
	};
	auto ReplacementFirst = ReplacementCoordinator.UpdateRelevance(ReplacementConnection, ReplacementSelection);
	const auto ReplacementFirstApplied = ReplacementFirst.Frame ? ReplacementReplica.ApplyFrame(*ReplacementFirst.Frame)
																: ReplicaApplyResult{};
	auto ReplacementPlayerReplica = std::dynamic_pointer_cast<Player>(
		ReplacementReplica.Resolve(ReplacementPlayer->GetObjectId())
	);
	Check(
		ReplacementFirst.Succeeded() && ReplacementFirstApplied.Succeeded() && ReplacementPlayerReplica &&
			ReplacementPlayerReplica->GetCharacter() &&
			*ReplacementPlayerReplica->GetCharacter() ==
				ReplacementReplica.Resolve(NewReplacementCharacter->GetObjectId()) &&
			!ReplacementReplica.Resolve(OldReplacementCharacter->GetObjectId()) &&
			ReplacementCoordinator.HasPendingRelevance(ReplacementConnection),
		"a new hard Character target consumes transition budget before the old target leaves"
	);
	auto ReplacementSecond = ReplacementCoordinator.UpdateRelevance(ReplacementConnection, ReplacementSelection);
	Check(
		ReplacementSecond.Succeeded() && ReplacementReplica.ApplyFrame(*ReplacementSecond.Frame).Succeeded() &&
			!ReplacementCoordinator.HasPendingRelevance(ReplacementConnection),
		"deferred unrelated leaves drain after the replacement reference is safe"
	);

	auto ScheduledWorld = std::make_shared<DataModel>();
	StructuralReplicationConfiguration ScheduledConfiguration{
		.MaximumTransitionsPerPeerTick = 4,
		.MaximumTransitionsPerTick = 8,
		.PeerQuantum = 2,
		.MaximumPendingTransitionsPerPeer = 64,
		.TransitionDeadlineTicks = 3,
	};
	ReplicationCoordinator ScheduledCoordinator(ScheduledWorld, {}, true, ScheduledConfiguration);
	const ConnectionId ScheduledConnection{11, 1};
	PeerRelevanceSelection ScheduledSelection{
		.RequiredObjects = {ScheduledWorld->GetObjectId()},
		.DesiredObjects = {ScheduledWorld->GetObjectId()},
	};
	std::vector<std::shared_ptr<Folder>> ScheduledObjects;
	std::vector<ObjectId> ExpectedScheduledOrder;
	for (std::size_t Index = 0; Index < 12; ++Index) {
		auto Object = std::make_shared<Folder>();
		Object->SetParent(ScheduledWorld);
		ScheduledSelection.DesiredObjects.push_back(Object->GetObjectId());
		ExpectedScheduledOrder.push_back(Object->GetObjectId());
		ScheduledObjects.push_back(std::move(Object));
	}
	std::ranges::sort(ScheduledSelection.DesiredObjects);
	std::ranges::sort(ExpectedScheduledOrder);
	auto ScheduledBaseline = ScheduledCoordinator.AddPeerBounded(
		ScheduledConnection, ReplicationEpoch(1), ScheduledSelection
	);
	ReplicaApplier ScheduledReplica;
	Check(
		ScheduledBaseline.Succeeded() && ScheduledBaseline.Frame->Operations.size() == 1 &&
			ScheduledReplica.ApplyFrame(*ScheduledBaseline.Frame).Succeeded() &&
			ScheduledCoordinator.GetMetrics().MaterializationBacklog == 13 &&
			ScheduledCoordinator.GetMetrics().ObjectsPublished == 0 &&
			!ScheduledCoordinator.GetView(ScheduledConnection)->Knows(ScheduledWorld->GetObjectId()),
		"bounded bootstrap prepares only its required dependency closure and retains compact ordinary work"
	);
	Check(
		!ScheduledCoordinator.CommitSchedulerAcceptance(ScheduledConnection, ReliableReplicationSequence(99))
				.Succeeded() &&
			!ScheduledCoordinator.GetView(ScheduledConnection)->Knows(ScheduledWorld->GetObjectId()),
		"mismatched scheduler acceptance cannot commit a prepared structural frame"
	);
	Check(
		ScheduledCoordinator.CommitSchedulerAcceptance(ScheduledConnection, ScheduledBaseline.Frame->Sequence)
				.Succeeded() &&
			ScheduledCoordinator.GetMetrics().MaterializationBacklog == 12 &&
			ScheduledCoordinator.GetMetrics().ObjectsPublished == 1 &&
			ScheduledCoordinator.GetView(ScheduledConnection)->Knows(ScheduledWorld->GetObjectId()),
		"bounded bootstrap becomes committed only after scheduler acceptance"
	);

	const auto CancelledObject = ExpectedScheduledOrder.back();
	ScheduledSelection.DesiredObjects.erase(std::ranges::find(ScheduledSelection.DesiredObjects, CancelledObject));
	ExpectedScheduledOrder.pop_back();
	auto RecordedScheduled = ScheduledCoordinator.RecordDesiredState(ScheduledConnection, ScheduledSelection, 1);
	Check(
		RecordedScheduled.Succeeded() && ScheduledCoordinator.GetMetrics().MaterializationBacklog == 11 &&
			ScheduledCoordinator.GetMetrics().StructuralTransitionsCancelled == 1,
		"an object that becomes undesired before materialization cancels without a create/destroy pair"
	);

	std::vector<ObjectId> ActualScheduledOrder;
	for (std::uint64_t Tick = 10; Tick < 32 && ScheduledCoordinator.HasPendingRelevance(ScheduledConnection); ++Tick) {
		auto Produced = ScheduledCoordinator.ProducePendingRelevance(ScheduledConnection, 2, Tick);
		Check(
			Produced.Succeeded() && Produced.SelectedTransitions <= 2 &&
				ScheduledReplica.ApplyFrame(*Produced.Frame).Succeeded(),
			"constrained structural scheduling emits a dependency-safe bounded frame"
		);
		if (!Produced.Succeeded()) break;
		std::vector<ObjectId> SelectedObjects;
		for (const auto &Operation : Produced.Frame->Operations)
			if (IsPublishReplication(Operation.Intent))
				SelectedObjects.push_back(GetReplicationObject(Operation.Intent));
		const auto BeforeAcceptance = ScheduledCoordinator.GetMetrics();
		Check(
			BeforeAcceptance.StructuralTransitionsSelected ==
					BeforeAcceptance.StructuralTransitionsAccepted + Produced.SelectedTransitions &&
				std::ranges::none_of(
					SelectedObjects,
					[&](ObjectId Object) { return ScheduledCoordinator.GetView(ScheduledConnection)->Knows(Object); }
				),
			"selection and preparation do not claim scheduler acceptance or structural commit"
		);
		Check(
			ScheduledCoordinator.CommitSchedulerAcceptance(ScheduledConnection, Produced.Frame->Sequence).Succeeded(),
			"accepted bounded structural frame commits its exact prepared transition set"
		);
		ActualScheduledOrder.insert(ActualScheduledOrder.end(), SelectedObjects.begin(), SelectedObjects.end());
	}
	const auto ScheduledMetrics = ScheduledCoordinator.GetMetrics();
	Check(
		ActualScheduledOrder == ExpectedScheduledOrder &&
			!ScheduledCoordinator.HasPendingRelevance(ScheduledConnection) &&
			ScheduledMetrics.StructuralTransitionsSelected == ScheduledMetrics.StructuralTransitionsAccepted &&
			ScheduledMetrics.StructuralTransitionsAccepted == ScheduledMetrics.StructuralTransitionsCommitted &&
			ScheduledMetrics.StructuralTransitionsDeferredByBudget != 0 &&
			ScheduledMetrics.StructuralDeadlineMisses != 0,
		"persistent age-ordered scheduling drains every eligible ObjectId and reports overload honestly"
	);
	Check(
		ScheduledReplica.Resolve(CancelledObject) == nullptr,
		"cancelled pending materialization never becomes visible to the client replica"
	);

	auto CoalescedWorld = std::make_shared<DataModel>();
	auto CoalescedFirst = std::make_shared<Folder>();
	CoalescedFirst->SetParent(CoalescedWorld);
	auto CoalescedSecond = std::make_shared<Folder>();
	CoalescedSecond->SetParent(CoalescedWorld);
	ReplicationCoordinator CoalescedCoordinator(CoalescedWorld, {}, true, ScheduledConfiguration);
	const ConnectionId CoalescedConnection{13, 1};
	PeerRelevanceSelection CoalescedSelection{
		.RequiredObjects = {CoalescedWorld->GetObjectId()},
		.DesiredObjects = {
			CoalescedWorld->GetObjectId(), CoalescedFirst->GetObjectId(), CoalescedSecond->GetObjectId()
		},
	};
	std::ranges::sort(CoalescedSelection.DesiredObjects);
	auto CoalescedRegistration = CoalescedCoordinator.RegisterPeerBounded(
		CoalescedConnection, ReplicationEpoch(1), CoalescedSelection
	);
	auto CoalescedBaseline = CoalescedCoordinator.ProducePendingBaseline(CoalescedConnection, 1, 1);
	ReplicaApplier CoalescedReplica;
	Check(
		CoalescedRegistration.Succeeded() && CoalescedBaseline.Succeeded() &&
			CoalescedReplica.ApplyFrame(*CoalescedBaseline.Frame).Succeeded() &&
			CoalescedCoordinator.CommitSchedulerAcceptance(CoalescedConnection, CoalescedBaseline.Frame->Sequence)
				.Succeeded(),
		"coalescing fixture commits only its required root before ordinary structural work"
	);
	CoalescedSecond->SetName("CurrentWhilePending");
	auto SkippedPendingMutation = CoalescedCoordinator.ProduceIncremental(CoalescedConnection, 2);
	Check(
		!SkippedPendingMutation.Succeeded() &&
			SkippedPendingMutation.Error == "No relevant replication changes are available",
		"journal progress skips history for a peer that has not materialized the pending object"
	);
	auto CoalescedEnters = CoalescedCoordinator.ProducePendingRelevance(CoalescedConnection, 2, 2);
	Check(
		CoalescedEnters.Succeeded() && CoalescedReplica.ApplyFrame(*CoalescedEnters.Frame).Succeeded() &&
			CoalescedCoordinator.CommitSchedulerAcceptance(CoalescedConnection, CoalescedEnters.Frame->Sequence)
				.Succeeded(),
		"pending objects materialize from their current template after skipped unknown-object history"
	);
	auto CoalescedReplicaObject = CoalescedReplica.Resolve(CoalescedSecond->GetObjectId());
	Check(
		CoalescedReplicaObject && CoalescedReplicaObject->GetName() == "CurrentWhilePending",
		"eventual materialization contains the newest authoritative state without historical replay"
	);
	auto NoHistoricalReplay = CoalescedCoordinator.ProduceIncremental(CoalescedConnection, 2);
	Check(
		!NoHistoricalReplay.Succeeded() && NoHistoricalReplay.Error == "No replication changes are available",
		"coalesced pending-object mutation does not remain as a later journal update"
	);

	PeerRelevanceSelection ScheduledLeaveSelection{
		.RequiredObjects = {ScheduledWorld->GetObjectId()},
		.DesiredObjects = {ScheduledWorld->GetObjectId()},
	};
	auto RecordedLeaves = ScheduledCoordinator.RecordDesiredState(ScheduledConnection, ScheduledLeaveSelection, 40);
	const auto RetainedObject = ExpectedScheduledOrder.front();
	ScheduledLeaveSelection.DesiredObjects.push_back(RetainedObject);
	std::ranges::sort(ScheduledLeaveSelection.DesiredObjects);
	auto CancelledLeave = ScheduledCoordinator.RecordDesiredState(ScheduledConnection, ScheduledLeaveSelection, 41);
	Check(
		RecordedLeaves.Succeeded() && CancelledLeave.Succeeded() &&
			ScheduledCoordinator.GetView(ScheduledConnection)->Knows(RetainedObject) &&
			ScheduledCoordinator.GetMetrics().MaterializationBacklog == ExpectedScheduledOrder.size() - 1,
		"ordinary leave cancelled by renewed desire preserves the committed materialization lifetime"
	);

	StructuralReplicationConfiguration InvalidStructuralConfiguration;
	InvalidStructuralConfiguration.MaximumTransitionsPerPeerTick = 0;
	Check(!InvalidStructuralConfiguration.IsValid(), "production structural scheduling rejects a zero peer budget");
	InvalidStructuralConfiguration = {};
	InvalidStructuralConfiguration.PeerQuantum = InvalidStructuralConfiguration.MaximumTransitionsPerPeerTick + 1;
	Check(
		!InvalidStructuralConfiguration.IsValid(),
		"production structural scheduling rejects a peer quantum larger than its peer budget"
	);
	InvalidStructuralConfiguration = {};
	InvalidStructuralConfiguration.MaximumPendingTransitions =
		InvalidStructuralConfiguration.MaximumPendingTransitionsPerPeer - 1;
	Check(
		!InvalidStructuralConfiguration.IsValid(),
		"production structural scheduling rejects a global pending limit below its per-peer limit"
	);

	StructuralReplicationConfiguration GlobalPendingConfiguration;
	GlobalPendingConfiguration.MaximumTransitionsPerPeerTick = 2;
	GlobalPendingConfiguration.MaximumTransitionsPerTick = 2;
	GlobalPendingConfiguration.PeerQuantum = 2;
	GlobalPendingConfiguration.MaximumPendingTransitionsPerPeer = 2;
	GlobalPendingConfiguration.MaximumPendingTransitions = 2;
	auto GlobalPendingWorld = std::make_shared<DataModel>();
	auto GlobalPendingFirst = std::make_shared<Folder>();
	GlobalPendingFirst->SetParent(GlobalPendingWorld);
	auto GlobalPendingSecond = std::make_shared<Folder>();
	GlobalPendingSecond->SetParent(GlobalPendingWorld);
	ReplicationCoordinator GlobalPendingCoordinator(GlobalPendingWorld, {}, true, GlobalPendingConfiguration);
	PeerRelevanceSelection GlobalPendingSelection{
		.RequiredObjects = {GlobalPendingWorld->GetObjectId()},
		.DesiredObjects = {
			GlobalPendingWorld->GetObjectId(), GlobalPendingFirst->GetObjectId(), GlobalPendingSecond->GetObjectId()
		},
	};
	std::ranges::sort(GlobalPendingSelection.DesiredObjects);
	auto GlobalPendingRegistration = GlobalPendingCoordinator.RegisterPeerBounded(
		{14, 1}, ReplicationEpoch(1), GlobalPendingSelection
	);
	Check(
		!GlobalPendingRegistration.Succeeded() &&
			GlobalPendingCoordinator.GetMetrics().StructuralBacklogLimitFailures == 1 &&
			GlobalPendingCoordinator.GetMetrics().MaterializationBacklog == 0,
		"the global pending ceiling fails only the registering peer and releases its partial queue"
	);

	PeerRelevanceSelection OversizedSelection = RootOnly;
	OversizedSelection.DesiredObjects.resize(MaximumPeerDesiredObjects + 1, BudgetWorld->GetObjectId());
	auto Oversized = BudgetCoordinator.UpdateRelevance(BudgetConnection, OversizedSelection);
	Check(
		!Oversized.Succeeded() && Oversized.Error == "Replication relevance selection exceeds its object limit",
		"oversized native relevance selections fail before dependency traversal or temporary growth"
	);

	RemoteCharacter->SetParent(RemotePlayer);
	ReplicationCoordinator CycleCoordinator(World);
	PeerRelevanceSelection CycleSelection{
		.RequiredObjects = {World->GetObjectId()},
		.DesiredObjects = {RemotePlayer->GetObjectId()},
	};
	auto CycleBaseline = CycleCoordinator.AddPeer({4, 1}, ReplicationEpoch(1), CycleSelection);
	ReplicaApplier CycleReplica;
	Check(
		CycleBaseline.Succeeded() && CycleBaseline.Frame->Operations.size() == 4 &&
			CycleReplica.ApplyFrame(*CycleBaseline.Frame).Succeeded(),
		"dependency closure terminates deterministically across an ancestry and hard-reference cycle"
	);
	RemoteCharacter->Destroy();
	auto CycleDestroy = CycleCoordinator.UpdateRelevance({4, 1}, CycleSelection);
	const auto ReplicaCyclePlayer = std::dynamic_pointer_cast<Player>(
		CycleReplica.Resolve(RemotePlayer->GetObjectId())
	);
	Check(
		CycleDestroy.Succeeded() && CycleReplica.ApplyFrame(*CycleDestroy.Frame).Succeeded() && ReplicaCyclePlayer &&
			!ReplicaCyclePlayer->GetCharacter() &&
			std::ranges::any_of(
				CycleDestroy.Frame->Operations,
				[](const auto &Operation) { return std::holds_alternative<DestroyReplication>(Operation.Intent); }
			),
		"authoritative Character destroy clears the hard Player reference before retiring the replica"
	);

	Runtime.Destroy();
	if (Failures == 0) std::cout << "Replication relevance tests passed\n";
	return Failures == 0 ? 0 : 1;
}
