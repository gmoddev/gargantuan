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

	ReplicationRelevanceConfiguration Limited;
	Limited.MaximumQueryCells = 1;
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
		DestroyOperations = std::ranges::count_if(
			DenseLeaveSecond.Frame->Operations,
			[](const auto &Operation) { return std::holds_alternative<DestroyReplication>(Operation.Intent); }
		);
	Check(
		DenseLeaveSecond.Succeeded() && DenseLeaveSecond.Frame->Operations.size() == 5 &&
			DestroyOperations == 1 && BudgetReplica.ApplyFrame(*DenseLeaveSecond.Frame).Succeeded() &&
			DenseObjects.back()->GetDestroyed() &&
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
	auto EnterRaceBaseline =
		EnterRaceCoordinator.AddPeer(EnterRaceConnection, ReplicationEpoch(1), EnterRaceSelection);
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
		EnterRaceFirst.Succeeded() &&
			EnterRaceFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame,
		"pending-enter destruction fixture leaves exactly one target behind its budget"
	);
	Check(
		!EnterRaceSecond.Succeeded() &&
			EnterRaceSecond.Error == "No replication relevance changes are available",
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
	Check(
		DependencyEnterFirst.Succeeded() &&
			DependencyEnterFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			BudgetReplica.ApplyFrame(*DependencyEnterFirst.Frame).Succeeded() && DependencyEnterSecond.Succeeded() &&
			DependencyEnterSecond.Frame->Operations.size() == 1 &&
			BudgetReplica.ApplyFrame(*DependencyEnterSecond.Frame).Succeeded() &&
			BudgetReplica.Resolve(DependencyPlayer->GetObjectId()) &&
			BudgetReplica.Resolve(DependencyCharacter->GetObjectId()),
		"owner-required hard-reference group is prioritized and never publishes a referrer before its target"
	);
	auto DependencyLeaveFirst = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	auto DependencyLeaveSecond = BudgetCoordinator.UpdateRelevance(BudgetConnection, RootOnly);
	Check(
		DependencyLeaveFirst.Succeeded() &&
			DependencyLeaveFirst.Frame->Operations.size() == MaximumRelevanceTransitionsPerFrame &&
			BudgetReplica.ApplyFrame(*DependencyLeaveFirst.Frame).Succeeded() && DependencyLeaveSecond.Succeeded() &&
			DependencyLeaveSecond.Frame->Operations.size() == 1 &&
			BudgetReplica.ApplyFrame(*DependencyLeaveSecond.Frame).Succeeded(),
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
	const auto ReplacementFirstApplied =
		ReplacementFirst.Frame ? ReplacementReplica.ApplyFrame(*ReplacementFirst.Frame) : ReplicaApplyResult{};
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
		CycleDestroy.Succeeded() && CycleReplica.ApplyFrame(*CycleDestroy.Frame).Succeeded() &&
			ReplicaCyclePlayer && !ReplicaCyclePlayer->GetCharacter() &&
			std::ranges::any_of(CycleDestroy.Frame->Operations, [](const auto &Operation) {
				return std::holds_alternative<DestroyReplication>(Operation.Intent);
			}),
		"authoritative Character destroy clears the hard Player reference before retiring the replica"
	);

	Runtime.Destroy();
	if (Failures == 0) std::cout << "Replication relevance tests passed\n";
	return Failures == 0 ? 0 : 1;
}
