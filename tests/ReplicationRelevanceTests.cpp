#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/Players.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include "../src/network/PlanningLookup.hpp"
#include "ReliableEnvelopeContractFixture.hpp"

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

	void TestPlanningLimits() {
		auto World = std::make_shared<DataModel>();
		const ConnectionId Connection{81, 1};
		StructuralReplicationConfiguration Configuration;
		Configuration.PlanningWorkPerTick = 1; Configuration.PlanningPeerQuantum = 1;
		Check(!Configuration.IsValid(), "planning allowance must fund a peer visit and actual work");
		Configuration.PlanningWorkPerTick = 2;
		ReplicationCoordinator Small(World, {}, true, Configuration);
		auto Input = std::make_shared<const PeerRelevanceSelection>(PeerRelevanceSelection{
			.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}});
		Check(Small.RegisterPeerPlanned(Connection, ReplicationEpoch(1), Input).Succeeded(), "minimum-budget peer registers");
		for (std::uint64_t Tick = 0; Tick != 300 && !Small.IsPlanningReady(Connection); ++Tick) {
			const auto Before = Small.GetCumulativeMetrics().PlanningWork;
			Small.ProcessPlanning(Tick);
			Check(Small.GetCumulativeMetrics().PlanningWork - Before == 2, "minimum budget makes progress, including tick zero");
			Small.ProcessPlanning(Tick);
			Check(Small.GetCumulativeMetrics().PlanningWork - Before == 2, "tick-zero and repeated calls cannot refill allowance");
		}
		Check(Small.IsPlanningReady(Connection) && Small.GetView(Connection)->KnownObjects.empty(), "minimum valid budget completes without accepting early");
		Small.RemovePeer(Connection);
		Configuration.PlanningWorkPerTick = 3;
		ReplicationCoordinator Fair(World, {}, true, Configuration);
		const ConnectionId Second{82, 1};
		Check(Fair.RegisterPeerPlanned(Connection, ReplicationEpoch(1), Input).Succeeded() &&
			Fair.RegisterPeerPlanned(Second, ReplicationEpoch(2), Input).Succeeded(), "odd-budget fairness peers register");
		for (std::uint64_t Tick = 0; Tick != 300 && (!Fair.IsPlanningReady(Connection) || !Fair.IsPlanningReady(Second)); ++Tick) {
			const auto Before = Fair.GetCumulativeMetrics().PlanningServiceOpportunities;
			Fair.ProcessPlanning(Tick);
			Check(Fair.GetCumulativeMetrics().PlanningServiceOpportunities - Before <= 1,
				"a peer visit without remaining examination budget is not service");
		}
		Check(Fair.IsPlanningReady(Connection) && Fair.IsPlanningReady(Second) &&
			Fair.GetCumulativeMetrics().PlanningMaximumServiceGapTicks <= 2, "odd remaining allowance cannot starve the next peer");
		Fair.RemovePeer(Connection); Fair.RemovePeer(Second);

		ReplicationCoordinator Limited(World);
		// Deliberately duplicate-rich native input: production 3E emits sorted
		// unique sets, but the private continuation must still enforce its ceiling.
		PeerRelevanceSelection Large;
		Large.RequiredObjects.assign(MaximumPeerDesiredObjects, World->GetObjectId());
		Large.DesiredObjects = Large.RequiredObjects;
		auto LargeInput = std::make_shared<const PeerRelevanceSelection>(std::move(Large));
		Check(Limited.RegisterPeerPlanned(Connection, ReplicationEpoch(1), LargeInput).Succeeded(), "bounded native stress input registers");
		bool Denied = false;
		std::uint64_t Tick = 0;
		for (; Tick != 300 && !Denied; ++Tick) {
			Limited.ProcessPlanning(Tick);
			Denied = !Limited.RequestPlanning(Connection, LargeInput, Tick).Succeeded();
		}
		Check(Denied && Limited.GetView(Connection)->KnownObjects.empty(), "scratch ceiling fails closed before partial acceptance");
		Check(Limited.GetCumulativeMetrics().PlanningPeerRecordsHighWater <= MaximumPlanningRecordsPerPeer &&
			Limited.GetCumulativeMetrics().PlanningRecordsHighWater <= MaximumPlanningRecords, "denial never exceeds either reservation ceiling");
		Limited.RemovePeer(Connection);
		for (int Attempt = 0; Attempt != 1'000 && Limited.HasPendingStructuralWork(); ++Attempt) Limited.ProcessPlanning(++Tick);
		Check(Limited.GetCumulativeMetrics().PlanningRecords == 0, "failed continuation releases every reservation through bounded disposal");
		World->Destroy();
	}

	void TestPlanningStaleCriticalInput() {
		auto World = std::make_shared<DataModel>();
		auto Old = std::make_shared<Folder>(); Old->SetParent(World);
		auto OldInput = std::make_shared<const PeerRelevanceSelection>(PeerRelevanceSelection{
			.RequiredObjects = {World->GetObjectId(), Old->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Old->GetObjectId()}});
		StructuralReplicationConfiguration Configuration;
		Configuration.PlanningWorkPerTick = 5; Configuration.PlanningPeerQuantum = 3;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		const ConnectionId Connection{80, 1};
		Check(Coordinator.RegisterPeerPlanned(Connection, ReplicationEpoch(1), OldInput).Succeeded(), "stale-input peer registers");
		Check(!Coordinator.RecordDesiredState(Connection, *OldInput, 0).Succeeded(), "synchronous relevance cannot mutate continuation input");
		Check(!Coordinator.SetRelevant(Connection, World->GetObjectId(), true).Succeeded(), "legacy direct publication cannot bypass continuation acceptance");
		std::uint64_t Tick = 1;
		Coordinator.ProcessPlanning(Tick);
		Old->Destroy();
		// Keep the old required input while 3E is waiting for its own service turn.
		for (; Tick != 300; ++Tick) {
			Check(Coordinator.RequestPlanning(Connection, OldInput, Tick).Succeeded(), "stale required identity defers without rejecting a healthy peer");
			Coordinator.ProcessPlanning(Tick);
			Check(!Coordinator.IsPlanningReady(Connection) && Coordinator.GetView(Connection)->KnownObjects.empty(), "obsolete partial critical group never escapes planning");
		}
		const auto Before = Coordinator.GetCumulativeMetrics().PlanningWork;
		Coordinator.ProcessPlanning(++Tick);
		Check(Coordinator.GetCumulativeMetrics().PlanningWork == Before, "obsolete input waits without repeated full rebuilds");
		auto Fresh = std::make_shared<Folder>(); Fresh->SetParent(World);
		auto FreshInput = std::make_shared<const PeerRelevanceSelection>(PeerRelevanceSelection{
			.RequiredObjects = {World->GetObjectId(), Fresh->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Fresh->GetObjectId()}});
		Check(Coordinator.RequestPlanning(Connection, FreshInput, Tick).Succeeded(), "fresh 3E input wakes parked planning");
		for (int Attempt = 0; Attempt != 300 && !Coordinator.IsPlanningReady(Connection); ++Attempt) Coordinator.ProcessPlanning(++Tick);
		auto Frame = Coordinator.ProducePendingBaseline(Connection, Configuration.PeerQuantum, Tick);
		Check(Frame.Frame.has_value(), "fresh critical group eventually becomes ready");
		if (Frame.Frame) {
			ReplicaApplier Replica;
			Check(Replica.ApplyFrame(*Frame.Frame).Succeeded(), "fresh critical group remains dependency-complete");
			Check(Coordinator.CommitSchedulerAcceptance(Connection, Frame.Frame->Sequence).Succeeded(), "fresh group remains acceptance-only");
			Check(!Replica.Resolve(Old->GetObjectId()) && Replica.Resolve(Fresh->GetObjectId()), "old critical generation cannot reappear");
		}
		Coordinator.RemovePeer(Connection);
		World->Destroy();
	}

	void TestPlanningReferenceChange() {
		auto World = std::make_shared<DataModel>();
		auto A = std::make_shared<Part>(); A->SetParent(World);
		auto B = std::make_shared<Part>(); B->SetParent(World);
		auto Referrer = std::make_shared<WeldConstraint>(); Referrer->SetParent(World);
		Referrer->SetPart0(A); Referrer->SetPart1(A);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), A->GetObjectId(), B->GetObjectId(), Referrer->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		auto Input = std::make_shared<const PeerRelevanceSelection>(Selection);
		StructuralReplicationConfiguration Configuration;
		Configuration.PlanningWorkPerTick = 5; Configuration.PlanningPeerQuantum = 3;
		Configuration.PeerQuantum = Configuration.MaximumTransitionsPerPeerTick = 8;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		const ConnectionId Connection{79, 1};
		ReplicaApplier Replica;
		std::uint64_t Tick = 0;
		Check(Coordinator.RegisterPeerPlanned(Connection, ReplicationEpoch(1), Input).Succeeded(), "reference continuation peer registers");
		auto Next = [&](bool Baseline) {
			for (int Attempt = 0; Attempt != 4'000; ++Attempt) {
				Coordinator.ProcessPlanning(++Tick);
				auto Produced = Baseline ? Coordinator.ProducePendingBaseline(Connection, 8, Tick) : Coordinator.ProducePendingRelevance(Connection, 8, Tick);
				if (!Produced.Frame) {
					if (Produced.Error == "No replication relevance changes are available") continue;
					std::cerr << "[Replication:PlanningReference] " << Produced.Error << '\n'; return false;
				}
				const auto Applied = Replica.ApplyFrame(*Produced.Frame);
				if (!Applied.Succeeded()) { std::cerr << "[Replication:PlanningReference] " << Applied.Message << '\n'; return false; }
				return Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded();
			}
			return false;
		};
		Check(Next(true), "reference continuation baseline applies");
		for (int Attempt = 0; Attempt != 4'000 && Coordinator.HasPendingStructuralWork(); ++Attempt)
			Coordinator.ProcessPlanning(++Tick);
		Check(!Coordinator.HasPendingStructuralWork(), "prior baseline disposal completes before testing a new reference plan");
		// A same-value reference journal commit replaces accepted vector storage
		// without changing its semantic revision. Resumption must not borrow an
		// iterator into that old vector, and must still make forward progress.
		auto Added = std::make_shared<Folder>(); Added->SetParent(World);
		Selection.DesiredObjects.push_back(Added->GetObjectId());
		std::ranges::sort(Selection.DesiredObjects);
		Input = std::make_shared<const PeerRelevanceSelection>(Selection);
		Check(Coordinator.RequestPlanning(Connection, Input, Tick).Succeeded(), "storage churn plan starts");
		for (int Attempt = 0; Attempt != 1'000 && !Coordinator.IsPlanningReady(Connection); ++Attempt) {
			ChangeJournal::Get().Commit(World->GetObjectId(), Referrer->GetObjectId(), PropertyUpdatedChange{
				"Part0", WireObjectReference{WireObjectId::FromObjectId(A->GetObjectId())}, true});
			auto Update = Coordinator.ProduceIncremental(Connection, 8);
			if (Update.Frame) {
				Check(Replica.ApplyFrame(*Update.Frame).Succeeded(), "same accepted reference journal applies during planning");
				Check(Coordinator.CommitSchedulerAcceptance(Connection, Update.Frame->Sequence).Succeeded(), "same-value accepted storage churn commits");
			}
			Coordinator.ProcessPlanning(++Tick);
		}
		Check(Coordinator.IsPlanningReady(Connection) && Next(false), "same-value storage churn cannot invalidate or corrupt resumed dependency planning");
		for (int Attempt = 0; Attempt != 4'000 && Coordinator.HasPendingStructuralWork(); ++Attempt)
			Coordinator.ProcessPlanning(++Tick);
		std::erase(Selection.DesiredObjects, A->GetObjectId());
		Input = std::make_shared<const PeerRelevanceSelection>(Selection);
		Check(Coordinator.RequestPlanning(Connection, Input, Tick).Succeeded(), "removal planning starts from accepted old references");
		Coordinator.ProcessPlanning(++Tick);
		const auto Before = Coordinator.GetCumulativeMetrics().PlanningInvalidations;
		Referrer->SetPart0(B); Referrer->SetPart1(B);
		Check(Coordinator.RequestPlanning(Connection, Input, Tick).Succeeded(), "reference mutation invalidates without a new semantic selection");
		Check(Next(false), "changed reference resumes as a complete clear/replace/removal group");
		auto ClientReferrer = std::dynamic_pointer_cast<WeldConstraint>(Replica.Resolve(Referrer->GetObjectId()));
		Check(ClientReferrer && ClientReferrer->GetPart0() && *ClientReferrer->GetPart0() == Replica.Resolve(B->GetObjectId()) &&
			ClientReferrer->GetPart1() && *ClientReferrer->GetPart1() == Replica.Resolve(B->GetObjectId()) && !Replica.Resolve(A->GetObjectId()),
			"KI-007 replacement uses the current target after a planning yield");
		Check(Coordinator.GetCumulativeMetrics().PlanningInvalidations > Before, "soft reference invalidation is observed");
		auto OldParent = std::make_shared<Folder>(); OldParent->SetParent(World); Referrer->SetParent(OldParent);
		Check(Coordinator.RequestPlanning(Connection, Input, Tick).Succeeded(), "hard ancestry planning starts");
		Coordinator.ProcessPlanning(++Tick);
		auto FreshParent = std::make_shared<Folder>(); FreshParent->SetParent(World); Referrer->SetParent(FreshParent);
		Check(Coordinator.RequestPlanning(Connection, Input, Tick).Succeeded(), "reparent during private planning invalidates ancestry");
		Check(Next(false), "new ancestor and dependent parent fixup are dependency-complete");
		Check(Replica.Resolve(FreshParent->GetObjectId()) && !Replica.Resolve(OldParent->GetObjectId()) &&
			Replica.Resolve(Referrer->GetObjectId())->GetParent() == Replica.Resolve(FreshParent->GetObjectId()), "stale ancestry never becomes visible");
		Coordinator.RemovePeer(Connection); // Destructor also tests nonempty detached disposal.
		World->Destroy();
	}

	void TestPlanningContinuation() {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Objects;
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}};
		for (int Index = 0; Index != 20; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetParent(Index > 0 && Index < 3 ? std::static_pointer_cast<Instance>(Objects.back()) : World);
			Selection.DesiredObjects.push_back(Object->GetObjectId()); Objects.push_back(Object);
		}
		std::ranges::sort(Selection.DesiredObjects);
		auto Snapshot = std::make_shared<const PeerRelevanceSelection>(Selection);
		StructuralReplicationConfiguration Configuration;
		Configuration.PlanningWorkPerTick = 31;
		Configuration.PlanningPeerQuantum = 7;
		Configuration.PeerQuantum = Configuration.MaximumTransitionsPerPeerTick = 4;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		std::array<ConnectionId, 5> Connections{{{71,1}, {72,1}, {73,1}, {74,1}, {75,1}}};
		std::array<ReplicaApplier, 5> Replicas;
		for (const auto Connection : Connections)
			Check(Coordinator.RegisterPeerPlanned(Connection, ReplicationEpoch(1), Snapshot).Succeeded(), "planned peer registers without synchronous discovery");
		std::uint64_t Tick = 0;
		auto Advance = [&] {
			++Tick;
			const auto Before = Coordinator.GetCumulativeMetrics().PlanningWork;
			Coordinator.ProcessPlanning(Tick);
			Coordinator.ProcessPlanning(Tick);
			Coordinator.ProcessPlanning(Tick - 1);
			Check(Coordinator.GetCumulativeMetrics().PlanningWork - Before <= Configuration.PlanningWorkPerTick,
				"repeated and older tick calls cannot refill the planning budget");
			for (std::size_t Index = 0; Index < Connections.size(); ++Index) {
				const auto Connection = Connections[Index];
				const auto *View = Coordinator.GetView(Connection);
				if (!View) continue;
				const auto BeforeKnown = View->KnownObjects.size();
				auto Produced = BeforeKnown == 0 ? Coordinator.ProducePendingBaseline(Connection, 4, Tick)
					: Coordinator.ProducePendingRelevance(Connection, 4, Tick);
				Check(Coordinator.GetView(Connection)->KnownObjects.size() == BeforeKnown, "incomplete planning and frame preparation never accept Known");
				if (!Produced.Frame) {
					Check(Produced.Error == "No replication relevance changes are available", "planned continuation defers without structural error");
					if (Produced.Error != "No replication relevance changes are available") { std::cerr << "[Replication:Planning] " << Produced.Error << '\n'; return false; }
					continue;
				}
				Check(Produced.SelectedTransitions <= 4, "complete ready batch respects unchanged 3J peer quantum");
				const auto Applied = Replicas[Index].ApplyFrame(*Produced.Frame);
				Check(Applied.Succeeded(), "only dependency-complete ready batches reach strict client validation");
				if (!Applied.Succeeded()) { std::cerr << "[Replication:Planning] " << Applied.Message << '\n'; return false; }
				Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(), "3J acceptance commits exactly the ready batch");
			}
			return true;
		};
		Check(Advance(), "first planning slice executes");
		for (const auto Connection : Connections) Check(Coordinator.GetView(Connection)->KnownObjects.empty(), "partial dependency closure is not publishable");
		for (int Attempt = 0; Attempt != 3'000 && Coordinator.HasPendingStructuralWork(); ++Attempt) if (!Advance()) break;
		for (std::size_t Index = 0; Index < Connections.size(); ++Index) {
			const auto *View = Coordinator.GetView(Connections[Index]);
			Check(View && View->KnownObjects.size() == Selection.DesiredObjects.size(), "all five peers converge under saturated planning");
			for (const auto Object : Selection.DesiredObjects) Check(Replicas[Index].Resolve(Object) != nullptr, "reference desired result has no missing object");
		}
		const auto Metrics = Coordinator.GetCumulativeMetrics();
		Check(Metrics.PlanningMaximumTickWork <= 31 && Metrics.PlanningMaximumPeerSlice <= 7, "inner planning work and rotation slice are bounded");
		Check(Metrics.PlanningMaximumServiceGapTicks <= 3, "eligible peer service is bounded under saturated rotation");
		Check(Metrics.PlanningRecordsHighWater <= MaximumPlanningRecords && Metrics.PlanningPeerRecordsHighWater <= MaximumPlanningRecordsPerPeer,
			"shared and per-peer continuation records obey hard ceilings");
		const auto BeforeIdle = Coordinator.GetCumulativeMetrics().PlanningWork;
		Coordinator.ProcessPlanning(++Tick);
		Check(Coordinator.GetCumulativeMetrics().PlanningWork == BeforeIdle, "drained planner has a zero-work path");
		// Start a removal plan, then replace its input and destroy/recreate before
		// it can become READY. Every peer must observe the fresh generation only.
		const auto OldId = Objects.front()->GetObjectId();
		PeerRelevanceSelection Empty{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}};
		auto EmptySnapshot = std::make_shared<const PeerRelevanceSelection>(Empty);
		for (const auto Connection : Connections) Check(Coordinator.RequestPlanning(Connection, EmptySnapshot, Tick).Succeeded(), "removal input queues");
		Coordinator.ProcessPlanning(++Tick);
		Objects.front()->Destroy();
		auto Fresh = std::make_shared<Folder>(); Fresh->SetParent(World);
		Check(Fresh->GetObjectId() != OldId, "recreation has a fresh identity");
		Selection.DesiredObjects = {World->GetObjectId(), Fresh->GetObjectId()};
		std::ranges::sort(Selection.DesiredObjects);
		Snapshot = std::make_shared<const PeerRelevanceSelection>(Selection);
		for (const auto Connection : Connections) Check(Coordinator.RequestPlanning(Connection, Snapshot, Tick).Succeeded(), "fresh relevance replaces incomplete input");
		Check(Coordinator.RemovePeer(Connections.back()), "disconnect cancels an in-progress frontier");
		for (int Attempt = 0; Attempt != 3'000 && Coordinator.HasPendingStructuralWork(); ++Attempt) if (!Advance()) break;
		for (std::size_t Index = 0; Index + 1 < Connections.size(); ++Index) {
			Check(Coordinator.GetView(Connections[Index])->KnownObjects.size() == 2 &&
				Replicas[Index].Resolve(Fresh->GetObjectId()) && !Replicas[Index].Resolve(OldId), "stale removal/reload plans cannot resurrect an old generation");
			Coordinator.RemovePeer(Connections[Index]);
		}
		for (int Attempt = 0; Attempt != 3'000 && Coordinator.HasPendingStructuralWork(); ++Attempt) Coordinator.ProcessPlanning(++Tick);
		Check(Coordinator.GetCumulativeMetrics().PlanningRecords == 0, "bounded detached teardown releases all continuation accounting");
		std::cout << "[Replication:Planning] ticks=" << Tick << " maxWork=" << Metrics.PlanningMaximumTickWork
			<< " maxSlice=" << Metrics.PlanningMaximumPeerSlice << " maxServiceGap=" << Metrics.PlanningMaximumServiceGapTicks
			<< " recordsHighWater=" << Metrics.PlanningRecordsHighWater << '\n';
		World->Destroy();
	}

	void TestBoundedPeerEvaluation() {
		auto World = std::make_shared<DataModel>();
		std::array<std::shared_ptr<Folder>, 20> Identities;
		std::array<ConnectionId, 20> Connections;
		std::array<glm::vec3, 20> Focus{};
		std::array<ObjectId, 20> Owners{};
		for (std::size_t Index = 0; Index < Identities.size(); ++Index) {
			Identities[Index] = std::make_shared<Folder>();
			Identities[Index]->SetParent(World);
			Connections[Index] = ConnectionId{static_cast<std::uint32_t>(Index + 1), 1};
		}
		ReplicationRelevanceConfiguration Configuration;
		Configuration.MaximumPeerEvaluationsPerTick = 4;
		Configuration.UpdateIntervalTicks = 1000;
		ReplicationRelevance Relevance(World, {}, Configuration);
		for (std::size_t Index = 0; Index < Connections.size(); ++Index) {
			Check(Relevance.AddPeer(Connections[Index], Identities[Index]->GetObjectId()), "bounded peer registers");
			Check(Relevance.SetTrustedFocus(Connections[Index], std::span(&Focus[Index], 1)), "bounded trusted focus registers");
		}
		std::array<std::shared_ptr<Part>, 8> Region;
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		auto Admit = [&](std::size_t Index) {
			Region[Index] = std::make_shared<Part>();
			Region[Index]->SetCFrame(CFrame(static_cast<float>(Index), 0.0f, 0.0f));
			Region[Index]->SetParent(World);
		};
		for (std::size_t Index = 0; Index < Region.size(); ++Index) Admit(Index);
		std::uint64_t Tick = 0;
		auto Advance = [&] {
			Check(Relevance.Update(++Tick), "bounded relevance advances safely");
			Check(Relevance.GetMetrics().PeerEvaluationsLastUpdate <= 4, "per-tick peer work obeys its count budget");
		};
		auto CompareReference = [&] {
			for (std::size_t Index = 0; Index < Connections.size(); ++Index) {
				std::vector<ObjectId> Desired{World->GetObjectId()};
				std::vector<ObjectId> Required{Identities[Index]->GetObjectId()};
				std::vector<ObjectId> CharacterCandidates;
				for (const auto &Identity : Identities) Desired.push_back(Identity->GetObjectId());
				auto Near = [&](glm::vec3 Position) {
					const auto Difference = Position - Focus[Index];
					return Difference.x * Difference.x + Difference.y * Difference.y + Difference.z * Difference.z <=
						Configuration.EnterRadius * Configuration.EnterRadius;
				};
				for (const auto &PartValue : Region)
					if (PartValue && !PartValue->GetDestroyed() && Near(PartValue->GetCFrame().Position))
						Desired.push_back(PartValue->GetObjectId());
				for (const auto &CharacterValue : Characters) {
					if (CharacterValue->GetDestroyed()) continue;
					const bool Owner = Owners[Index] == CharacterValue->GetObjectId();
					if (Owner) Required.push_back(CharacterValue->GetObjectId());
					if (Owner || Near(CharacterValue->GetPosition())) {
						Desired.push_back(CharacterValue->GetObjectId());
						CharacterCandidates.push_back(CharacterValue->GetObjectId());
					}
				}
				std::ranges::sort(Desired);
				std::ranges::sort(Required);
				std::ranges::sort(CharacterCandidates);
				Check(std::ranges::equal(Relevance.GetRuntimeCharacterCandidates(Connections[Index]), CharacterCandidates),
					"typed Character inspection candidates equal the independent semantic reference");
				const auto *Selection = Relevance.GetSelection(Connections[Index]);
				Check(Selection && Selection->DesiredObjects == Desired && Selection->RequiredObjects == Required,
					"bounded evaluation converges to independent flat-world semantic reference");
			}
		};
		auto Drain = [&] {
			for (int Attempt = 0; Attempt < 20; ++Attempt) {
				Advance();
				if (Relevance.GetMetrics().DeferredPeers == 0) { CompareReference(); return; }
			}
			Check(false, "bounded relevance backlog must drain fairly");
		};
		Advance();
		Check(Relevance.GetMetrics().DeferredPeers == 16, "one region defers remaining peers without copied transition lists");
		const auto Evaluations = Relevance.GetMetrics().PeerEvaluations;
		Check(Relevance.Update(Tick) && Relevance.GetMetrics().PeerEvaluations == Evaluations,
			"repeated calls in one tick cannot bypass the peer evaluation cap");
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		CharacterValue->SetPosition({5000.0f, 0.0f, 0.0f});
		CharacterValue->SetParent(World);
		Characters.push_back(CharacterValue);
		Owners.back() = CharacterValue->GetObjectId();
		Check(Relevance.SetOwnerCharacter(Connections.back(), Owners.back()), "critical owner arrives behind bulk work");
		Advance();
		Check(Relevance.WasSelectionEvaluated(Connections.back()) &&
			Contains(Relevance.GetSelection(Connections.back())->RequiredObjects, Owners.back()),
			"critical owner lifecycle receives a reserved evaluation while ordinary backlog advances");
		const auto Evicted = Region.front()->GetObjectId();
		Region.front()->Destroy();
		Check(!Relevance.IsRuntimeRelevant(Connections.front(), Evicted), "authoritative destroy revokes runtime relevance immediately");
		Admit(0);
		Check(Region.front()->GetObjectId() != Evicted, "reload has a fresh complete ObjectId");
		Focus[5] = {5000.0f, 0.0f, 0.0f};
		Check(Relevance.SetTrustedFocus(Connections[5], std::span(&Focus[5], 1)), "peer moves during deferred evaluation");
		const auto Disconnected = Connections[7];
		Check(Relevance.RemovePeer(Disconnected), "peer disconnect cancels deferred work");
		Connections[7] = ConnectionId{Disconnected.Slot, 2};
		Check(Relevance.AddPeer(Connections[7], Identities[7]->GetObjectId()) &&
			Relevance.SetTrustedFocus(Connections[7], std::span(&Focus[7], 1)), "reused slot gets fresh connection generation");
		Check(!Relevance.GetSelection(Disconnected), "stale connection generation is never resolved through cursor");
		Check(Relevance.GetRuntimeCharacterCandidates(Disconnected).empty(), "disconnect discards typed candidate projection");
		Drain();
		const auto BeforeIdle = Relevance.GetMetrics().PeerEvaluations;
		Advance();
		Check(Relevance.GetMetrics().PeerEvaluations == BeforeIdle && Relevance.GetMetrics().DeferredPeers == 0,
			"no-work tick performs no peer evaluation");
		// Deterministic interleaving deliberately changes authority before prior passes drain.
		std::uint32_t Seed = 0x3E3E2026;
		for (int Operation = 0; Operation < 64; ++Operation) {
			Seed = Seed * 1664525U + 1013904223U;
			const auto Index = (Seed >> 8) % Region.size();
			if ((Seed & 1) != 0) {
				if (Region[Index] && !Region[Index]->GetDestroyed()) Region[Index]->Destroy();
				else Admit(Index);
			} else {
				const auto PeerIndex = (Seed >> 16) % Connections.size();
				Focus[PeerIndex].x = Focus[PeerIndex].x == 0.0f ? 5000.0f : 0.0f;
				Check(Relevance.SetTrustedFocus(Connections[PeerIndex], std::span(&Focus[PeerIndex], 1)),
					"randomized trusted movement accepted");
			}
			Advance();
			if (Operation % 5 == 0) Drain();
		}
		Drain();
		const auto RetiredCharacter = CharacterValue->GetObjectId();
		CharacterValue->Destroy();
		Check(!Relevance.IsRuntimeRelevant(Connections.back(), RetiredCharacter),
			"old typed candidate cannot grant relevance before its peer projection refreshes");
		CharacterValue = std::make_shared<KinematicCharacter>();
		CharacterValue->SetPosition({5000.0f, 0.0f, 0.0f});
		CharacterValue->SetParent(World);
		Characters.push_back(CharacterValue);
		Owners.back() = CharacterValue->GetObjectId();
		Check(Owners.back() != RetiredCharacter && Relevance.SetOwnerCharacter(Connections.back(), Owners.back()),
			"replacement typed candidate uses a fresh authoritative generation");
		Drain();
		Check(Relevance.GetMetrics().PeerEvaluationsHighWater == 4 &&
			Relevance.GetMetrics().DeferredPeersHighWater <= Connections.size() &&
			Relevance.GetMetrics().StagingBytes <= 1024, "fixed cursor metadata stays bounded without a peer-object matrix");
		for (const auto Connection : Connections) Check(Relevance.RemovePeer(Connection), "stop removes all peer work");
		Check(Relevance.GetMetrics().CharacterCandidateBytes == 0, "all typed candidate storage is released on peer cleanup");
		World->Destroy();
		Advance();
		Check(Relevance.GetMetrics().DeferredPeers == 0 && Relevance.VerifySpatialIndex(), "stop with stale dirty world leaves no peer or projection work");
		Configuration.MaximumPeerEvaluationsPerTick = 1;
		Check(!Configuration.IsValid(), "budget must reserve both ordinary and critical service");
		Configuration.MaximumPeerEvaluationsPerTick = MaximumReplicationRelevancePeers + 1;
		Check(!Configuration.IsValid(), "native work budget cannot exceed hard peer limit");
	}

	void TestOrderedHysteresisReference() {
		auto World = std::make_shared<DataModel>();
		auto Identity = std::make_shared<Folder>(); Identity->SetParent(World);
		std::array<std::shared_ptr<Part>, 128> Parts;
		std::array<bool, 128> Expected{};
		for (std::size_t Index = 0; Index != Parts.size(); ++Index) {
			Parts[Index] = std::make_shared<Part>();
			Parts[Index]->SetCFrame(CFrame(static_cast<float>((Index * 37) % 800), 0.0f, 0.0f));
			Parts[Index]->SetParent(World);
		}
		ReplicationRelevanceConfiguration Configuration;
		Configuration.UpdateIntervalTicks = 1;
		ReplicationRelevance Relevance(World, {}, Configuration);
		const ConnectionId Connection{46, 1};
		Check(Relevance.AddPeer(Connection, Identity->GetObjectId()), "ordered hysteresis peer registers");
		for (std::uint64_t Tick = 1; Tick <= 48; ++Tick) {
			// Interleave retained roots, new candidates and missing old generations;
			// three focus volumes also exercise query deduplication and sorted output.
			const std::array Focus{glm::vec3(static_cast<float>((Tick / 8) * 50), 0.0f, 0.0f),
				glm::vec3(-50.0f, 0.0f, 0.0f), glm::vec3(-25.0f, 0.0f, 0.0f)};
			if (Tick % 3 == 0) {
				const auto Index = Tick % Parts.size();
				const auto Old = Parts[Index]->GetObjectId();
				Parts[Index]->Destroy();
				Parts[Index] = std::make_shared<Part>();
				Parts[Index]->SetCFrame(CFrame(280.0f, 0.0f, 0.0f));
				Parts[Index]->SetParent(World);
				Expected[Index] = false;
				Check(Parts[Index]->GetObjectId() != Old, "hysteresis reload does not reuse complete identity");
			}
			if (Tick % 5 == 0) Parts.back()->SetCFrame(CFrame(Tick % 10 == 0 ? 240.0f : 600.0f, 0.0f, 0.0f));
			Check(Relevance.SetTrustedFocus(Connection, Focus) && Relevance.Update(Tick), "ordered hysteresis evaluation succeeds");
			const auto *Selection = Relevance.GetSelection(Connection);
			for (std::size_t Index = 0; Index != Parts.size(); ++Index) {
				const auto Radius = Expected[Index] ? Configuration.LeaveRadius : Configuration.EnterRadius;
				Expected[Index] = std::ranges::any_of(Focus, [&](glm::vec3 Point) {
					const auto Offset = Parts[Index]->GetCFrame().Position - Point;
					return glm::dot(Offset, Offset) <= Radius * Radius;
				});
				Check(Selection && Contains(Selection->DesiredObjects, Parts[Index]->GetObjectId()) == Expected[Index],
					"ordered membership join equals independent per-object hysteresis reference");
			}
			Check(Relevance.VerifySpatialIndex(), "root identity and projection index remain coherent");
		}
		Check(Relevance.RemovePeer(Connection), "ordered hysteresis peer cleanup succeeds");
		World->Destroy();
	}

	void TestStableRelevanceSelection() {
		auto World = std::make_shared<DataModel>();
		auto Identity = std::make_shared<Folder>(); Identity->SetParent(World);
		auto First = std::make_shared<KinematicCharacter>(); First->SetParent(World);
		auto Second = std::make_shared<KinematicCharacter>(); Second->SetPosition({10.0f, 0.0f, 0.0f}); Second->SetParent(World);
		ReplicationRelevanceConfiguration Configuration;
		Configuration.UpdateIntervalTicks = 1;
		ReplicationRelevance Relevance(World, {}, Configuration);
		const ConnectionId Connection{45, 1};
		const std::array Focus{glm::vec3(0.0f)};
		Check(Relevance.AddPeer(Connection, Identity->GetObjectId(), First->GetObjectId()) &&
			Relevance.SetTrustedFocus(Connection, Focus) && Relevance.Update(1), "stable relevance fixture initializes");
		const auto Initial = *Relevance.GetSelection(Connection);
		const auto InitialSnapshot = Relevance.GetSelectionSnapshot(Connection);
		const auto CacheBefore = Relevance.GetMetrics().SelectionCacheHits;
		First->SetPosition({2.0f, 0.0f, 0.0f});
		Check(Relevance.Update(2) && *Relevance.GetSelection(Connection) == Initial &&
			Relevance.GetMetrics().SelectionCacheHits == CacheBefore + 1,
			"current positions with unchanged semantic roots reuse the complete selection");
		Check(Relevance.GetSelectionSnapshot(Connection) == InitialSnapshot, "unchanged semantic input reuses the immutable selection snapshot");
		Check(Relevance.SetOwnerCharacter(Connection, Second->GetObjectId()) && Relevance.Update(3) &&
			Contains(Relevance.GetSelection(Connection)->RequiredObjects, Second->GetObjectId()) &&
			!Contains(Relevance.GetSelection(Connection)->RequiredObjects, First->GetObjectId()) &&
			Relevance.GetMetrics().SelectionCacheHits == CacheBefore + 1,
			"owner replacement rebuilds required lifecycle even when both roots were already relevant");
		Check(*InitialSnapshot == Initial && Relevance.GetSelectionSnapshot(Connection) != InitialSnapshot,
			"retained prior selection stays immutable across Character replacement");
		auto Member = std::make_shared<Folder>(); Member->SetParent(First);
		const auto MemberId = Member->GetObjectId();
		Check(Relevance.Update(4) && Contains(Relevance.GetSelection(Connection)->DesiredObjects, MemberId) &&
			Relevance.GetMetrics().SelectionCacheHits == CacheBefore + 1,
			"new descendant in an existing root invalidates selection membership without a new root");
		Member->Destroy();
		Check(Relevance.Update(5) && !Contains(Relevance.GetSelection(Connection)->DesiredObjects, MemberId),
			"destroyed descendant cannot persist in a stable-root selection");
		First->SetPosition({280.0f, 0.0f, 0.0f});
		Check(Relevance.Update(6) && Contains(Relevance.GetSelection(Connection)->DesiredObjects, First->GetObjectId()),
			"cached root remains inside hysteresis leave radius");
		First->SetPosition({330.0f, 0.0f, 0.0f});
		Check(Relevance.Update(7) && !Contains(Relevance.GetSelection(Connection)->DesiredObjects, First->GetObjectId()),
			"current projection outside leave radius invalidates the cached root");
		First->SetPosition({280.0f, 0.0f, 0.0f});
		Check(Relevance.Update(8) && !Contains(Relevance.GetSelection(Connection)->DesiredObjects, First->GetObjectId()),
			"a previously removed root does not reenter through the hysteresis band");
		First->SetPosition({250.0f, 0.0f, 0.0f});
		Check(Relevance.Update(9) && Contains(Relevance.GetSelection(Connection)->DesiredObjects, First->GetObjectId()),
			"current projection inside enter radius invalidates the cached absence");
		const auto Candidates = Relevance.GetRuntimeCharacterCandidates(Connection);
		Check(Candidates.size() == 2 && std::ranges::is_sorted(Candidates), "typed candidates are unique and deterministic");
		auto Container = std::make_shared<Folder>(); Container->SetParent(World);
		First->SetParent(Container);
		Check(Relevance.Update(10) && Relevance.GetRuntimeCharacterCandidates(Connection).size() == 2,
			"subtree reparenting preserves the derived Character candidate");
		First->SetParent(nullptr);
		Check(!Relevance.IsRuntimeRelevant(Connection, First->GetObjectId()) && Relevance.Update(11) &&
			Relevance.GetRuntimeCharacterCandidates(Connection).size() == 1,
			"subtree removal cannot retain runtime Character relevance");
		First->SetParent(World);
		Check(Relevance.Update(12) && Relevance.GetRuntimeCharacterCandidates(Connection).size() == 2,
			"subtree return rederives candidates from current 3E semantics");
	}

	void TestBoundedJournalReads() {
		auto World = std::make_shared<DataModel>();
		auto Visible = std::make_shared<Folder>(); Visible->SetParent(World);
		auto Hidden = std::make_shared<Folder>(); Hidden->SetParent(World);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Visible->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const ConnectionId Connection{47, 1};
		ReplicaApplier Replica;
		auto Accept = [&](ReplicationProduceResult Produced) {
			return Produced.Frame && Replica.ApplyFrame(*Produced.Frame).Succeeded() &&
				Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded();
		};
		Check(Accept(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection)), "bounded journal fixture bootstraps");
		runtime_detail::WorkSample JournalWork{};
		runtime_detail::WorkCapture Capture(&JournalWork);
		for (std::size_t Index = 0; Index < 60; ++Index) Hidden->SetName("Hidden" + std::to_string(Index));
		Visible->SetName("AfterHiddenHistory");
		std::size_t Reads = 0, Frames = 0;
		for (std::size_t Tick = 0; Tick < 32; ++Tick) {
			auto Produced = Coordinator.ProduceIncremental(Connection, 32, MaximumReplicationFrameBytes, 7);
			Check(Produced.JournalRecordsExamined <= 7, "skipped journal records respect the explicit work budget");
			Reads += Produced.JournalRecordsExamined;
			if (Produced.Frame) {
				++Frames;
				const auto Lag = Coordinator.GetJournalLag(Connection);
				Check(Lag != 0 && Coordinator.GetView(Connection)->KnownObjects.size() == 2,
					"prepared journal work preserves accepted Known and leaves cursor pending");
				Check(Coordinator.ProduceIncremental(Connection, 32, MaximumReplicationFrameBytes, 7).JournalRecordsExamined == 0 &&
					Coordinator.GetJournalLag(Connection) == Lag, "unaccepted journal frame cannot consume more history");
				Check(Accept(std::move(Produced)), "bounded journal frame applies and commits");
			}
			else Check(Produced.Error == "No relevant replication changes are available" ||
				Produced.Error == "No replication changes are available", "bounded journal deferral is not an error");
			if (Coordinator.GetJournalLag(Connection) == 0) break;
		}
		Check(Reads == 61 && Frames == 1 && Coordinator.GetJournalLag(Connection) == 0 &&
			Replica.Resolve(Visible->GetObjectId())->GetName() == "AfterHiddenHistory",
			"skipped history drains across bounded calls without losing the later relevant mutation");
		for (std::size_t Index = 0; Index < 30; ++Index) Visible->SetName(std::string(256, 'x') + std::to_string(Index));
		bool Retried = false;
		for (std::size_t Tick = 0; Tick < 64; ++Tick) {
			auto Produced = Coordinator.ProduceIncremental(Connection, 32, 512, 31);
			Check(Produced.JournalRecordsExamined <= 31, "byte-limit retry reads remain charged to the same budget");
			Retried = Retried || Produced.JournalRecordsExamined > 16;
			Check(Accept(std::move(Produced)), "byte-limited journal frame remains dependency-safe");
			if (Coordinator.GetJournalLag(Connection) == 0) break;
		}
		Check(Retried && Coordinator.GetJournalLag(Connection) == 0 &&
			Replica.Resolve(Visible->GetObjectId())->GetName() == Visible->GetName(),
			"byte-limit retries converge without exceeding the read budget or losing the last mutation");
		Check(JournalWork[static_cast<std::size_t>(runtime_detail::WorkPhase::PeerViewCopy)].Calls == 0,
			"read-only journal slices and byte retries never copy accepted peer membership");
		Hidden->SetName("PendingDisconnect");
		Check(Coordinator.RemovePeer(Connection) && Coordinator.GetJournalLag(Connection) == 0 &&
			Coordinator.ProduceIncremental(Connection, 32, 512, 31).JournalRecordsExamined == 0,
			"disconnect drops the generation-scoped journal cursor without retaining peer work");
		StructuralReplicationConfiguration Invalid;
		Invalid.MaximumJournalRecordsPerPeerTick = 0;
		Check(!Invalid.IsValid(), "zero journal peer budget is invalid");
		Invalid = {}; Invalid.MaximumJournalRecordsPerTick = MaximumStructuralJournalRecordsPerTick + 1;
		Check(!Invalid.IsValid(), "journal global hard ceiling cannot be exceeded");
		Invalid = {}; Invalid.MaximumJournalRecordsPerTick = Invalid.MaximumJournalRecordsPerPeerTick - 1;
		Check(!Invalid.IsValid(), "journal peer budget cannot exceed the global budget");
	}

	void TestReparentBeforeParentLeave() {
		for (const bool NewParentKnown : {false, true}) for (const std::size_t Budget : {2u, 64u}) {
			auto World = std::make_shared<DataModel>();
			auto Left = std::make_shared<Folder>(); Left->SetParent(World);
			auto Right = std::make_shared<Folder>(); Right->SetParent(World);
			auto Child = std::make_shared<Part>(); Child->SetParent(Left);
			PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
				.DesiredObjects = {World->GetObjectId(), Child->GetObjectId()}};
			if (NewParentKnown) Selection.DesiredObjects.push_back(Right->GetObjectId());
			std::ranges::sort(Selection.DesiredObjects);
			ReplicationCoordinator Coordinator(World);
			const ConnectionId Connection{48, 1};
			ReplicaApplier Replica;
			auto Accept = [&](ReplicationProduceResult Produced) {
				if (!Produced.Frame) return false;
				const auto Applied = Replica.ApplyFrame(*Produced.Frame);
				if (!Applied.Succeeded()) {
					std::cerr << "[Replication:ReparentTest] " << Applied.Message << '\n';
					return false;
				}
				return Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded();
			};
			Check(Accept(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection)), "reparent fixture bootstraps");
			const auto OriginalReplica = Replica.Resolve(Child->GetObjectId());
			// Older Parent history must not replay after a current-parent fixup;
			// unrelated property history must still be delivered.
			Child->SetParent(Right); Child->SetName("MovedChild"); Child->SetParent(Left); Child->SetParent(Right);
			Check(Coordinator.RecordDesiredState(Connection, Selection, 1).Succeeded(), "reparent fixture records latest Desired");
			bool AppliedAll = true;
			for (std::uint64_t Tick = 1; Tick < 16 && Coordinator.HasPendingRelevance(Connection); ++Tick) {
				auto Produced = Coordinator.ProducePendingRelevance(Connection, Budget, Tick);
				Check(Produced.SelectedTransitions <= Budget, "reparent fixups count against the structural work cap");
				if (!Accept(std::move(Produced))) { AppliedAll = false; break; }
				Check(OriginalReplica && Replica.Resolve(Child->GetObjectId()) == OriginalReplica && !OriginalReplica->GetDestroyed(),
					"removing an old parent cannot destroy a still-desired moved child between structural batches");
			}
			Check(AppliedAll && !Coordinator.HasPendingRelevance(Connection) &&
				!Coordinator.GetView(Connection)->Knows(Left->GetObjectId()) && Coordinator.GetView(Connection)->Knows(Child->GetObjectId()) &&
				Replica.Resolve(Child->GetObjectId()) == OriginalReplica && OriginalReplica && !OriginalReplica->GetDestroyed() &&
				OriginalReplica->GetParent() == std::optional(Replica.Resolve(Right->GetObjectId())),
				"new-parent Enter, child reparent and old-parent Leave converge without changing child identity");
			if (AppliedAll) for (std::size_t Tick = 0; Tick < 16; ++Tick) {
				auto Produced = Coordinator.ProduceIncremental(Connection, 16, MaximumReplicationFrameBytes, 7);
				if (Produced.Frame) {
					if (!Accept(std::move(Produced))) { AppliedAll = false; break; }
				} else if (Produced.Error != "No relevant replication changes are available" &&
					Produced.Error != "No replication changes are available") { AppliedAll = false; break; }
				if (Coordinator.GetJournalLag(Connection) == 0) break;
			}
			Check(AppliedAll && Coordinator.GetJournalLag(Connection) == 0 && OriginalReplica && !OriginalReplica->GetDestroyed() &&
				OriginalReplica->GetName() == "MovedChild" &&
				OriginalReplica->GetParent() == std::optional(Replica.Resolve(Right->GetObjectId())),
				"parent-only coalescing prevents stale ancestry replay without swallowing an ordinary property mutation");
			std::cout << "[Replication:ReparentTest] targetKnown=" << NewParentKnown << " budget=" << Budget
				<< " applied=" << AppliedAll << " childRetained=" << (Replica.Resolve(Child->GetObjectId()) == OriginalReplica &&
					OriginalReplica && !OriginalReplica->GetDestroyed()) << '\n';
		}
		// When both objects leave, dependency order must follow accepted ancestry,
		// even though the server has already moved the child elsewhere.
		auto World = std::make_shared<DataModel>();
		auto Left = std::make_shared<Folder>(); Left->SetParent(World);
		auto Right = std::make_shared<Folder>(); Right->SetParent(World);
		auto Child = std::make_shared<Part>(); Child->SetParent(Left);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Child->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const ConnectionId Connection{49, 1};
		ReplicaApplier Replica;
		auto Accept = [&](ReplicationProduceResult Produced) {
			return Produced.Frame && Replica.ApplyFrame(*Produced.Frame).Succeeded() &&
				Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded();
		};
		Check(Accept(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection)), "moved-leave fixture bootstraps");
		Child->SetParent(Right);
		Selection.DesiredObjects = {World->GetObjectId()};
		Check(Coordinator.RecordDesiredState(Connection, Selection, 1).Succeeded(), "moved child and old parent become undesired");
		for (std::uint64_t Tick = 1; Tick < 5 && Coordinator.HasPendingRelevance(Connection); ++Tick) {
			Check(Accept(Coordinator.ProducePendingRelevance(Connection, 1, Tick)), "one-operation leave preserves accepted ancestry order");
			Check(Coordinator.GetView(Connection)->Knows(Child->GetObjectId()) == (Replica.Resolve(Child->GetObjectId()) != nullptr),
				"recursive parent removal never makes accepted Known disagree with the replica between slices");
			Check(Coordinator.GetMetrics().AcceptedAncestryObjects == Coordinator.GetView(Connection)->KnownObjects.size(),
				"accepted ancestry metadata exists exactly for Known objects, not for staged or retired identities");
		}
		Check(!Coordinator.HasPendingRelevance(Connection) && Coordinator.GetView(Connection)->KnownObjects.size() == 1,
			"moved-child removal drains to the root without publishing its irrelevant destination");
	}

	void TestAcceptedMembershipUpdates() {
		auto World = std::make_shared<DataModel>();
		std::array<std::shared_ptr<Folder>, 128> Objects;
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}};
		for (auto &Object : Objects) {
			Object = std::make_shared<Folder>(); Object->SetParent(World);
			Selection.DesiredObjects.push_back(Object->GetObjectId());
		}
		std::ranges::sort(Selection.DesiredObjects);
		StructuralReplicationConfiguration Configuration;
		Configuration.PeerQuantum = Configuration.MaximumTransitionsPerPeerTick = 32;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		const ConnectionId Connection{51, 1};
		ReplicaApplier Replica;
		auto CompareReference = [&] {
			const auto &View = *Coordinator.GetView(Connection);
			for (const auto Id : View.KnownObjects)
				Check(View.RelevantObjects.contains(Id) == Contains(Selection.DesiredObjects, Id),
					"accepted relevance membership equals independent Desired intersect Known");
			for (const auto Id : View.RelevantObjects)
				Check(View.Knows(Id) && Contains(Selection.DesiredObjects, Id), "no unaccepted or undesired member is exposed as relevant");
		};
		auto Accept = [&](ReplicationProduceResult Produced) {
			Check(Produced.Frame.has_value(), "membership fixture produces an accepted frame");
			if (!Produced.Frame) return;
			Check(Replica.ApplyFrame(*Produced.Frame).Succeeded(), "membership frame applies correctly");
			Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(), "membership frame commits correctly");
			CompareReference();
		};
		Accept(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection));
		Accept(Coordinator.ProducePendingRelevance(Connection, 32, 1));
		// Cancel both already-known and not-yet-known ordinary objects while other
		// Enters remain pending. Desired changes still refresh the intersection.
		Selection.DesiredObjects = {World->GetObjectId()};
		for (std::size_t Index = 1; Index < Objects.size(); Index += 2)
			Selection.DesiredObjects.push_back(Objects[Index]->GetObjectId());
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Connection, Selection, 2).Succeeded(), "membership cancellation records current Desired");
		CompareReference();
		for (std::uint64_t Tick = 3; Tick < 32 && Coordinator.HasPendingRelevance(Connection); ++Tick)
			Accept(Coordinator.ProducePendingRelevance(Connection, 7, Tick));
		Check(!Coordinator.HasPendingRelevance(Connection), "membership cancellation converges through bounded acceptance");
		Objects[1]->SetName("AcceptedPropertyOnly");
		runtime_detail::WorkSample Work{};
		{
			runtime_detail::WorkCapture Capture(&Work);
			for (std::size_t Tick = 0; Tick < 64; ++Tick) {
				auto Produced = Coordinator.ProduceIncremental(Connection, 7, MaximumReplicationFrameBytes, 31);
				if (Produced.Frame) Accept(std::move(Produced));
				else Check(Produced.Error == "No relevant replication changes are available" ||
					Produced.Error == "No replication changes are available", "bounded membership journal progress is valid");
				if (Coordinator.GetJournalLag(Connection) == 0) break;
			}
		}
		Check(Work[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralCommit)].Calls != 0 &&
			Work[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralCommit)].Units == 0 &&
			Replica.Resolve(Objects[1]->GetObjectId())->GetName() == "AcceptedPropertyOnly",
			"ordinary accepted property frame does no structural membership work");
		Coordinator.RemovePeer(Connection);
		World->Destroy();
	}

	void TestBoundedDependencyGroupExpansion() {
		auto World = std::make_shared<DataModel>();
		// Registry slots are reused: construction order is not ObjectId order.
		// Select the smallest actual identity as parent so the first ordinary
		// Leave candidate necessarily exercises the oversized dependency group.
		std::array<std::shared_ptr<Folder>, 129> GroupObjects;
		for (auto &Object : GroupObjects) { Object = std::make_shared<Folder>(); (void)Object->GetObjectId(); }
		std::ranges::sort(GroupObjects, [](const auto &Left, const auto &Right) { return Left->GetObjectId() < Right->GetObjectId(); });
		auto Region = GroupObjects.front(); Region->SetParent(World);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Region->GetObjectId()}};
		for (std::size_t Index = 1; Index < GroupObjects.size(); ++Index) {
			auto Child = GroupObjects[Index]; Child->SetParent(Region);
			Selection.DesiredObjects.push_back(Child->GetObjectId());
		}
		std::ranges::sort(Selection.DesiredObjects);
		StructuralReplicationConfiguration Configuration;
		Configuration.PeerQuantum = Configuration.MaximumTransitionsPerPeerTick = 16;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		const ConnectionId Connection{50, 1};
		ReplicaApplier Replica;
		auto Accept = [&](ReplicationProduceResult Produced) {
			return Produced.Frame && Produced.SelectedTransitions <= 16 && Replica.ApplyFrame(*Produced.Frame).Succeeded() &&
				Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded();
		};
		Check(Accept(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection)), "group bound fixture bootstraps");
		std::uint64_t Tick = 1;
		for (; Tick < 32 && Coordinator.HasPendingRelevance(Connection); ++Tick)
			Check(Accept(Coordinator.ProducePendingRelevance(Connection, 16, Tick)), "bounded groups enter parent-first across slices");
		Check(!Coordinator.HasPendingRelevance(Connection), "all group bound objects become Known before removal");
		const auto KnownBefore = Coordinator.GetView(Connection)->KnownObjects;
		Selection.DesiredObjects = {World->GetObjectId()};
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(), "oversized leave group is discovered");
		runtime_detail::WorkSample Sample{};
		ReplicationProduceResult Produced;
		{
			runtime_detail::WorkCapture Capture(&Sample);
			Produced = Coordinator.ProducePendingRelevance(Connection, 16, Tick++);
		}
		Check(!Produced.Frame && Produced.Error == "Atomic replication dependency group exceeds the transition work limit" &&
			Coordinator.GetView(Connection)->KnownObjects == KnownBefore,
			"oversized atomic group fails without accepting a partial removal or changing Known");
		Check(Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralGroupSelection)].Units <= 16,
			"oversized group is rejected during bounded expansion rather than after draining all descendants");
		Selection.DesiredObjects.push_back(Region->GetObjectId());
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(), "retaining parent cancels the oversized atomic removal");
		for (; Tick < 64 && Coordinator.HasPendingRelevance(Connection); ++Tick)
			Check(Accept(Coordinator.ProducePendingRelevance(Connection, 16, Tick)), "independent leaf removals progress after cancellation");
		Check(!Coordinator.HasPendingRelevance(Connection) && Coordinator.GetView(Connection)->KnownObjects.size() == 2 &&
			Replica.Resolve(Region->GetObjectId()) && Replica.Resolve(Region->GetObjectId())->GetChildren().empty(),
			"cancellation converges to the independent semantic reference without stale group state");
		Coordinator.RemovePeer(Connection);
		World->Destroy();
	}

	void TestSelectedAncestryOrdering() {
		auto World = std::make_shared<DataModel>();
		std::array<std::shared_ptr<Folder>, 31> Objects;
		std::array<std::size_t, 31> Depths{};
		std::vector<std::pair<ObjectId, std::size_t>> Expected{{World->GetObjectId(), 0}};
		for (auto &Object : Objects) { Object = std::make_shared<Folder>(); (void)Object->GetObjectId(); }
		for (std::size_t Index = Objects.size(); Index-- != 0;) {
			if (Index == Objects.size() - 1) { Objects[Index]->SetParent(World); Depths[Index] = 1; }
			else {
				const auto Parent = (Index + Objects.size()) / 2;
				Objects[Index]->SetParent(Objects[Parent]);
				Depths[Index] = Depths[Parent] + 1;
			}
			Expected.emplace_back(Objects[Index]->GetObjectId(), Depths[Index]);
		}
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()}};
		for (const auto &[Object, Depth] : Expected) { (void)Depth; Selection.DesiredObjects.push_back(Object); }
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const ConnectionId Connection{46, 1};
		ReplicaApplier Replica;
		auto CheckOrder = [&](ReplicationProduceResult Produced, bool Enter) {
			Check(Produced.Frame.has_value(), "nested ancestry fixture produces a selected frame");
			if (!Produced.Frame) return;
			std::ranges::sort(Expected, [Enter](const auto &Left, const auto &Right) {
				if (Left.second == Right.second) return Left.first < Right.first;
				return Enter ? Left.second < Right.second : Left.second > Right.second;
			});
			std::vector<ObjectId> Actual, Reference;
			for (const auto &Operation : Produced.Frame->Operations) Actual.push_back(GetReplicationObject(Operation.Intent));
			for (const auto &[Object, Depth] : Expected) { (void)Depth; Reference.push_back(Object); }
			Check(Actual == Reference, "selected ancestry order equals independent depth/identity reference");
			Check(Replica.ApplyFrame(*Produced.Frame).Succeeded(), "nested ancestry frame applies dependency-safely");
			Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(),
				"nested ancestry frame changes Known only at acceptance");
		};
		CheckOrder(Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection), true);
		Selection.DesiredObjects = {World->GetObjectId()};
		std::erase_if(Expected, [&](const auto &Entry) { return Entry.first == World->GetObjectId(); });
		Check(Coordinator.RecordDesiredState(Connection, Selection, 1).Succeeded(), "nested leave intent is recorded");
		CheckOrder(Coordinator.ProducePendingRelevance(Connection, 64, 1), false);
		Check(Coordinator.GetView(Connection)->KnownObjects.size() == 1, "all nested leaves converge to the retained root");
	}

	void TestDependencyInvalidation() {
		auto World = std::make_shared<DataModel>();
		auto Left = std::make_shared<Folder>(); Left->SetParent(World);
		auto Right = std::make_shared<Folder>(); Right->SetParent(World);
		auto PartValue = std::make_shared<Part>(); PartValue->SetParent(Left);
		auto PlayerValue = std::make_shared<Player>(); PlayerValue->SetParent(World);
		auto FirstCharacter = std::make_shared<KinematicCharacter>(); FirstCharacter->SetParent(World);
		auto SecondCharacter = std::make_shared<KinematicCharacter>(); SecondCharacter->SetParent(World);
		auto FirstRoot = std::make_shared<Part>(); FirstRoot->SetParent(FirstCharacter);
		auto SecondRoot = std::make_shared<Part>(); SecondRoot->SetParent(FirstCharacter);
		FirstCharacter->SetRootPart(FirstRoot);
		PlayerValue->SetCharacter(FirstCharacter);
		PeerRelevanceSelection Selection{
			.RequiredObjects = {World->GetObjectId(), PlayerValue->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), PlayerValue->GetObjectId(), Left->GetObjectId(), PartValue->GetObjectId()},
		};
		std::ranges::sort(Selection.RequiredObjects);
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const ConnectionId Connection{30, 1};
		ReplicaApplier Replica;
		auto Apply = [&](ReplicationProduceResult Produced) {
			Check(Produced.Succeeded() && Produced.Frame.has_value(), "dependency fixture produces a frame");
			if (!Produced.Succeeded() || !Produced.Frame) return false;
			auto Result = Replica.ApplyFrame(*Produced.Frame);
			if (!Result.Succeeded()) std::cerr << "[Replication:DependencyTest] " << Result.Message << '\n';
			Check(Result.Succeeded(), "dependency fixture remains dependency-safe on the client");
			Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(),
				"dependency fixture commits only accepted work");
			return Result.Succeeded();
		};
		Check(Coordinator.RegisterPeerBounded(Connection, ReplicationEpoch(1), Selection).Succeeded(),
			"dependency fixture registers exact required Player closure");
		Apply(Coordinator.ProducePendingBaseline(Connection, 32, 1));
		Check(Coordinator.GetView(Connection)->Knows(FirstCharacter->GetObjectId()) &&
			!Coordinator.GetView(Connection)->Knows(FirstRoot->GetObjectId()),
			"hard Player.Character expands closure but soft RootPart does not");
		std::uint64_t Tick = 2;
		auto UnchangedDependencies = [&](auto Mutate) {
			const auto Before = Coordinator.GetMetrics().DependencyObjects;
			Mutate();
			Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(),
				"ordinary mutations preserve a valid dependency plan");
			Check(Coordinator.GetMetrics().DependencyObjects == Before,
				"ordinary journal mutations must not rebuild dependency closure");
		};
		UnchangedDependencies([&] { PartValue->SetName("new scalar state"); });
		UnchangedDependencies([&] { PartValue->SetCFrame(CFrame(10.0f, 0.0f, 0.0f)); });
		UnchangedDependencies([&] { PartValue->SetArchivable(false); });
		UnchangedDependencies([&] {
			Check(PartValue->ApplyAttributeMutation("Scalar", WireValue(true), ScriptSecurityContext::CoreTrusted()) ==
				MutationStatus::Success, "dependency fixture changes a non-structural Attribute");
		});
		UnchangedDependencies([&] { FirstCharacter->SetRootPart(SecondRoot); });
		Apply(Coordinator.ProduceIncremental(Connection));
		Check(Replica.Resolve(PartValue->GetObjectId())->GetName() == "new scalar state",
			"cached dependency plans do not suppress ordinary property replication");

		auto ChangedDependencies = [&](auto Mutate) {
			const auto Before = Coordinator.GetMetrics().DependencyObjects;
			Mutate();
			Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(),
				"dependency mutation replans successfully");
			Check(Coordinator.GetMetrics().DependencyObjects > Before,
				"ancestry, hard reference, or lifetime mutation rebuilds dependency closure");
		};
		ChangedDependencies([&] { PartValue->SetParent(Right); });
		Check(Coordinator.GetView(Connection)->Knows(Left->GetObjectId()) &&
			!Coordinator.GetView(Connection)->Knows(Right->GetObjectId()),
			"replanning cannot advance Known before scheduler acceptance");
		Apply(Coordinator.ProducePendingRelevance(Connection, 32, Tick++));
		Check(Coordinator.GetView(Connection)->Knows(Right->GetObjectId()), "new ancestry expands dependency closure");
		auto ParentHistory = Coordinator.ProduceIncremental(Connection);
		if (ParentHistory.Frame) Apply(std::move(ParentHistory));
		else Check(ParentHistory.Error == "No relevant replication changes are available" ||
			ParentHistory.Error == "No replication changes are available", "accepted parent fixup coalesces only old Parent history");
		Check(Replica.Resolve(PartValue->GetObjectId())->GetParent().value() == Replica.Resolve(Right->GetObjectId()),
			"new parent is available before its journal reparent applies");
		ChangedDependencies([&] { PlayerValue->SetCharacter(SecondCharacter); });
		Apply(Coordinator.ProducePendingRelevance(Connection, 32, Tick++));
		Check(Coordinator.GetView(Connection)->Knows(SecondCharacter->GetObjectId()) &&
			!Coordinator.GetView(Connection)->Knows(FirstCharacter->GetObjectId()), "hard reference replacement stays current");
		const auto OldPart = PartValue->GetObjectId();
		ChangedDependencies([&] { PartValue->Destroy(); });
		Apply(Coordinator.ProducePendingRelevance(Connection, 32, Tick++));
		Check(!Coordinator.GetView(Connection)->Knows(OldPart), "destroy invalidates unchanged policy selection");
		auto Reloaded = std::make_shared<Part>();
		ChangedDependencies([&] { Reloaded->SetParent(Right); });
		Check(Reloaded->GetObjectId() != OldPart && !Coordinator.GetView(Connection)->Knows(Reloaded->GetObjectId()),
			"fresh generation cannot inherit stale selected identity");
		std::erase(Selection.DesiredObjects, OldPart);
		Selection.DesiredObjects.push_back(Reloaded->GetObjectId());
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(), "fresh generation is selected explicitly");
		Apply(Coordinator.ProducePendingRelevance(Connection, 32, Tick++));
		Check(Coordinator.GetView(Connection)->Knows(Reloaded->GetObjectId()) && !Replica.Resolve(OldPart),
			"reload publishes only fresh identity without resurrecting old work");
		Selection.RequiredObjects.push_back(Reloaded->GetObjectId());
		std::ranges::sort(Selection.RequiredObjects);
		const auto BeforeRequired = Coordinator.GetMetrics().DependencyObjects;
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded() &&
			Coordinator.GetMetrics().DependencyObjects > BeforeRequired,
			"required lifecycle selection changes invalidate plans without any journal mutation");
		auto Cancelled = std::make_shared<Part>();
		Cancelled->SetParent(Right);
		const auto CancelledId = Cancelled->GetObjectId();
		Selection.DesiredObjects.push_back(CancelledId);
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(), "unobserved region creates pending Enter");
		Cancelled->Destroy();
		auto Stale = Coordinator.ProducePendingRelevance(Connection, 32, Tick++);
		Check(!Stale.Frame && Stale.Error == "No replication relevance changes are available" &&
			!Coordinator.GetView(Connection)->Knows(CancelledId),
			"eviction invalidates pending publication before bounded relevance revisits the peer");
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded() &&
			!Coordinator.HasPendingRelevance(Connection), "reevaluation cancels obsolete Enter without advancing Known");
		Check(Coordinator.RemovePeer(Connection), "dependency fixture releases peer-owned plans");
		World->Destroy();
	}

	void TestBoundedCatalogRetirement() {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Objects;
		for (std::size_t Index = 0; Index < MaximumCatalogRetirementExaminationsPerTick + 32; ++Index) {
			auto Object = std::make_shared<Folder>(); Object->SetParent(World);
			Objects.push_back(std::move(Object));
		}
		const ConnectionId First{40, 1}, Second{41, 1};
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Objects.front()->GetObjectId(), Objects.back()->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		for (const auto Connection : {First, Second}) {
			Check(Coordinator.RegisterPeerBounded(Connection, ReplicationEpoch(1), Selection).Succeeded(),
				"retirement peers register");
			auto Baseline = Coordinator.ProducePendingBaseline(Connection, 32, 1);
			Check(Baseline.Frame && Coordinator.CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded(),
				"retirement fixture accepts dependency-complete baseline");
		}
		// Publication revisions must reuse identity ownership, including reparent
		// metadata. A current template must not forget a Known old revision.
		Objects.front()->SetName("current revision of Known identity");
		Check(Coordinator.RecordDesiredState(First, Selection, 2).Succeeded(), "retirement fixture refreshes scalar revision");
		const auto OldFirst = Objects.front()->GetObjectId();
		for (const auto &Object : Objects) Object->Destroy();
		Check(Coordinator.RecordDesiredState(First, Selection, 3).Succeeded(), "retirement fixture journals destroyed identities");
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == Objects.size(), "retirement fixture retains the complete destroy set");
		const auto Before = Coordinator.GetMetrics().CatalogRetirementExaminations;
		for (int Repeat = 0; Repeat < 8; ++Repeat) Coordinator.ProcessCatalogRetirement(3);
		Check(Coordinator.GetMetrics().CatalogRetirementExaminations - Before == MaximumCatalogRetirementExaminationsPerTick,
			"retirement examinations share one exact global tick cap across repeated calls");
		Coordinator.ProcessCatalogRetirement(2);
		Check(Coordinator.GetMetrics().CatalogRetirementExaminations - Before == MaximumCatalogRetirementExaminationsPerTick,
			"older ticks cannot refill retirement allowance");
		Coordinator.ProcessCatalogRetirement(4);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 2 && Coordinator.GetMetrics().CatalogObjects == 3,
			"known endpoints cannot starve unrelated retired identities across the cursor boundary");
		Check(Coordinator.GetView(First)->Knows(OldFirst) && Coordinator.GetView(Second)->Knows(OldFirst),
			"reclamation never changes acceptance-only Known");
		Coordinator.RemovePeer(First);
		Coordinator.ProcessCatalogRetirement(5);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 2, "one disconnect cannot release another peer's Known templates");
		Check(Coordinator.RecordDesiredState(Second, Selection, 6).Succeeded(), "remaining peer plans complete reverse Leaves");
		auto Leaves = Coordinator.ProducePendingRelevance(Second, 32, 6);
		Check(Leaves.Frame && Leaves.SelectedTransitions == 2 &&
			Coordinator.CommitSchedulerAcceptance(Second, Leaves.Frame->Sequence).Succeeded(),
			"retained templates produce both terminal Leaves through unchanged 3J acceptance");
		Coordinator.ProcessCatalogRetirement(7);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 0 && Coordinator.GetMetrics().CatalogObjects == 1,
			"accepted last Leaves release retirement ownership");

		auto Fresh = std::make_shared<Folder>(); Fresh->SetParent(World);
		const auto FreshId = Fresh->GetObjectId();
		Check(FreshId != OldFirst, "recreated object has a fresh generation-bearing identity");
		Selection.DesiredObjects = {World->GetObjectId(), FreshId};
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Second, Selection, 8).Succeeded(), "fresh generation enters discovery");
		auto Prepared = Coordinator.ProducePendingRelevance(Second, 32, 8);
		Check(Prepared.Frame && !Coordinator.GetView(Second)->Knows(FreshId), "prepared Enter is not Known");
		Fresh->Destroy();
		PeerRelevanceSelection RootOnly{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}};
		const ConnectionId Reconnected{First.Slot, First.Generation + 1};
		Check(Coordinator.RegisterPeerBounded(Reconnected, ReplicationEpoch(2), RootOnly).Succeeded(),
			"peer reconnect refreshes retirement while another Enter awaits acceptance");
		Coordinator.ProcessCatalogRetirement(9);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 1, "prepared acceptance metadata also protects a retired identity");
		Coordinator.RemovePeer(Second);
		Coordinator.ProcessCatalogRetirement(10);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 0, "disconnect drops prepared ownership without resurrecting Known");
		auto ReconnectedBaseline = Coordinator.ProducePendingBaseline(Reconnected, 32, 10);
		Check(ReconnectedBaseline.Frame && Coordinator.CommitSchedulerAcceptance(Reconnected, ReconnectedBaseline.Frame->Sequence).Succeeded(),
			"reconnected peer accepts only its current root baseline");
		auto Late = std::make_shared<Folder>(); Late->SetParent(World);
		const auto LateId = Late->GetObjectId();
		Selection.DesiredObjects = {World->GetObjectId(), LateId};
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Reconnected, Selection, 11).Succeeded(), "late acceptance fixture discovers current Enter");
		auto LateEnter = Coordinator.ProducePendingRelevance(Reconnected, 32, 11);
		Check(LateEnter.Frame.has_value(), "late acceptance fixture prepares Enter");
		Late->Destroy();
		const ConnectionId Observer{42, 1};
		Check(Coordinator.RegisterPeerBounded(Observer, ReplicationEpoch(1), RootOnly).Succeeded(),
			"independent observer refreshes destruction while Enter awaits acceptance");
		Coordinator.ProcessCatalogRetirement(12);
		Check(Coordinator.GetMetrics().CatalogRetiredObjects == 1 && LateEnter.Frame &&
			Coordinator.CommitSchedulerAcceptance(Reconnected, LateEnter.Frame->Sequence).Succeeded(),
			"prepared lease transfers to Known after intervening catalog retirement");
		Check(Coordinator.RecordDesiredState(Reconnected, RootOnly, 13).Succeeded(), "late accepted retired identity receives current Leave");
		auto LateLeave = Coordinator.ProducePendingRelevance(Reconnected, 32, 13);
		Check(LateLeave.Frame && LateLeave.SelectedTransitions == 1 &&
			Coordinator.CommitSchedulerAcceptance(Reconnected, LateLeave.Frame->Sequence).Succeeded(),
			"late accepted identity retires through the existing ordered acceptance path");
		Coordinator.ProcessCatalogRetirement(14);
		Check(!Coordinator.GetView(Reconnected)->Knows(LateId) && Coordinator.GetMetrics().CatalogRetiredObjects == 0,
			"late acceptance cannot strand a stale lifetime or a retained template");
		const auto Drained = Coordinator.GetMetrics().CatalogRetirementExaminations;
		Coordinator.ProcessCatalogRetirement(15);
		Check(Coordinator.GetMetrics().CatalogRetirementExaminations == Drained, "empty retirement has no graph or peer scan");
		Check(Coordinator.GetMetrics().CatalogRetirementMaximumTickExaminations == MaximumCatalogRetirementExaminationsPerTick,
			"retirement diagnostic reports exact cap saturation");
		Coordinator.RemovePeer(Reconnected);
		Coordinator.RemovePeer(Observer);
		World->Destroy();
	}

	void TestSoftReferenceReplacementBeforeLeave() {
		for (const bool JournalFirst : {false, true}) {
			auto World = std::make_shared<DataModel>();
			auto CharacterValue = std::make_shared<KinematicCharacter>(); CharacterValue->SetParent(World);
			auto OldRoot = std::make_shared<Part>(); OldRoot->SetParent(CharacterValue);
			CharacterValue->SetRootPart(OldRoot);
			const auto OldId = OldRoot->GetObjectId();
			PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
				.DesiredObjects = {World->GetObjectId(), CharacterValue->GetObjectId(), OldId}};
			std::ranges::sort(Selection.DesiredObjects);
			ReplicationCoordinator Coordinator(World);
			const ConnectionId Connection{52, 1};
			ReplicaApplier Replica;
			auto Baseline = Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection);
			Check(Baseline.Frame && Replica.ApplyFrame(*Baseline.Frame).Succeeded() &&
				Coordinator.CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded(),
				"soft replacement fixture establishes the accepted old reference");
			OldRoot->Destroy();
			auto NewRoot = std::make_shared<Part>(); NewRoot->SetParent(CharacterValue);
			CharacterValue->SetRootPart(NewRoot);
			Selection.DesiredObjects = {World->GetObjectId(), CharacterValue->GetObjectId()};
			std::ranges::sort(Selection.DesiredObjects);
			Check(Coordinator.RecordDesiredState(Connection, Selection, 1).Succeeded() &&
				NewRoot->GetObjectId() != OldId && Coordinator.GetView(Connection)->Knows(OldId) &&
				!Coordinator.GetView(Connection)->Knows(NewRoot->GetObjectId()),
				"replacement has fresh identity and remains unmaterialized before acceptance");
			// Both orderings are legal existing service opportunities. Structural
			// removal must not depend on journal service having cleared the old edge.
			auto Produced = JournalFirst ? Coordinator.ProduceIncremental(Connection, 32)
				: Coordinator.ProducePendingRelevance(Connection, 32, 1);
			Check(Produced.Frame && Produced.SelectedTransitions <= 32, "soft replacement produces bounded work");
			if (Produced.Frame) {
				std::size_t NilFixups = 0;
				for (const auto &Operation : Produced.Frame->Operations)
					if (const auto *Update = std::get_if<PropertyReplicationUpdate>(&Operation.Intent))
						NilFixups += Update->PropertyName == "RootPart" && std::holds_alternative<std::monostate>(Update->Value);
				const auto Applied = Replica.ApplyFrame(*Produced.Frame);
				std::cout << "[Replication:SoftReplacement] journalFirst=" << JournalFirst
					<< " operations=" << Produced.Frame->Operations.size() << " nilFixups=" << NilFixups
					<< " applied=" << Applied.Succeeded() << " error=" << Applied.Message << '\n';
				Check(Applied.Succeeded(), "old soft target removal must clear the accepted reference before client preflight");
				if (Applied.Succeeded())
					Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(),
						"soft replacement remains acceptance-only");
				if (JournalFirst && Applied.Succeeded()) {
					auto Removal = Coordinator.ProducePendingRelevance(Connection, 32, 2);
					const bool Removed = Removal.Frame && Replica.ApplyFrame(*Removal.Frame).Succeeded() &&
						Coordinator.CommitSchedulerAcceptance(Connection, Removal.Frame->Sequence).Succeeded();
					Check(Removed && !Replica.Resolve(OldId) && !Coordinator.GetView(Connection)->Knows(OldId),
						"journal-first control clears the reference then removes the old target successfully");
					std::cout << "[Replication:SoftReplacement] journalFirstRemovalApplied=" << Removed << '\n';
				}
			}
			Check(Coordinator.RemovePeer(Connection), "soft replacement failure or success permits disconnect cleanup");
			World->Destroy();
		}
	}

	void TestUnrelatedHardNilFixups() {
		auto World = std::make_shared<DataModel>();
		auto Region = std::make_shared<Folder>(); Region->SetParent(World);
		for (int Index = 0; Index < 31; ++Index) std::make_shared<Part>()->SetParent(Region);
		std::vector<ObjectId> Global{World->GetObjectId()};
		for (int Index = 0; Index < 20; ++Index) {
			auto Spectator = std::make_shared<Player>(); Spectator->SetParent(World);
			Global.push_back(Spectator->GetObjectId()); // Player.Character remains a typed hard nil.
		}
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = Global};
		Selection.DesiredObjects.push_back(Region->GetObjectId());
		for (const auto &Object : Region->GetDescendants()) Selection.DesiredObjects.push_back(Object->GetObjectId());
		std::ranges::sort(Selection.DesiredObjects);
		StructuralReplicationConfiguration Configuration;
		Configuration.PeerQuantum = 32;
		Configuration.MaximumTransitionsPerPeerTick = 32;
		ReplicationCoordinator Coordinator(World, {}, true, Configuration);
		const ConnectionId Connection{31, 1};
		ReplicaApplier Replica;
		Check(Coordinator.RegisterPeerBounded(Connection, ReplicationEpoch(1), Selection).Succeeded(), "typed-nil fixture registers");
		auto Apply = [&](ReplicationProduceResult Produced) {
			Check(Produced.Frame && Produced.SelectedTransitions <= 32, "typed-nil fixture preserves exact selected cap");
			if (!Produced.Frame) return;
			Check(Replica.ApplyFrame(*Produced.Frame).Succeeded(), "typed-nil fixture applies a coherent frame");
			Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(), "typed-nil fixture commits accepted work");
		};
		Apply(Coordinator.ProducePendingBaseline(Connection, 32, 1));
		std::uint64_t Tick = 2;
		for (; Coordinator.HasPendingRelevance(Connection) && Tick < 10; ++Tick)
			Apply(Coordinator.ProducePendingRelevance(Connection, 32, Tick));
		Check(!Coordinator.HasPendingRelevance(Connection), "typed-nil fixture fully materializes before eviction");
		const auto OldRegion = Region->GetObjectId();
		Region->Destroy();
		Selection.DesiredObjects = Global;
		std::ranges::sort(Selection.DesiredObjects);
		Check(Coordinator.RecordDesiredState(Connection, Selection, Tick++).Succeeded(), "typed-nil eviction replans");
		auto Leave = Coordinator.ProducePendingRelevance(Connection, 32, Tick++);
		Check(Leave.Frame && Leave.SelectedTransitions == 32,
			"unrelated hard nils cannot turn a legal 32-object region into an oversized atomic group");
		Apply(std::move(Leave));
		Check(!Replica.Resolve(OldRegion) && !Coordinator.HasPendingRelevance(Connection), "typed-nil region removal converges without resurrection");
		Coordinator.RemovePeer(Connection);
		World->Destroy();
	}

	void TestAcceptedReferenceRemovalMatrix(bool Planned = false) {
		// Two native properties on each of two independent referrers. The
		// frozen WeldConstraint schema supplies the reference policy; no test schema.
		for (int Scenario = 0; Scenario < 10; ++Scenario) {
			auto World = std::make_shared<DataModel>();
			auto A = std::make_shared<Part>(); A->SetParent(World);
			const auto OldId = A->GetObjectId();
			auto B = std::make_shared<Part>(); B->SetParent(World);
			std::array<std::shared_ptr<WeldConstraint>, 2> Referrers;
			PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
				.DesiredObjects = {World->GetObjectId(), OldId}};
			for (auto &Referrer : Referrers) {
				Referrer = std::make_shared<WeldConstraint>(); Referrer->SetParent(World);
				Referrer->SetPart0(A); Referrer->SetPart1(A);
				Selection.DesiredObjects.push_back(Referrer->GetObjectId());
			}
			if (Scenario == 2) Selection.DesiredObjects.push_back(B->GetObjectId());
			std::ranges::sort(Selection.DesiredObjects);
			StructuralReplicationConfiguration Configuration;
			if (Scenario >= 8) Configuration.PeerQuantum = Configuration.MaximumTransitionsPerPeerTick = 4;
			if (Planned) { Configuration.PlanningWorkPerTick = 11; Configuration.PlanningPeerQuantum = 3; }
			ReplicationCoordinator Coordinator(World, {}, true, Configuration);
			const ConnectionId Connection{53, 1};
			ReplicaApplier Replica;

			std::uint64_t PlanningTestTick = 0;
			auto Await = [&](ConnectionId ForPeer, bool Baseline, std::size_t Limit, std::size_t Bytes) -> ReplicationProduceResult {
				if (Planned) for (int Attempt = 0; Attempt != 10'000 && !Coordinator.IsPlanningReady(ForPeer); ++Attempt) {
					auto Probe = Baseline ? Coordinator.ProducePendingBaseline(ForPeer, Limit, PlanningTestTick, Bytes)
						: Coordinator.ProducePendingRelevance(ForPeer, Limit, PlanningTestTick, Bytes);
					if (Probe.Error != "No replication relevance changes are available") return Probe;
					Coordinator.ProcessPlanning(++PlanningTestTick);
				}
				return Baseline ? Coordinator.ProducePendingBaseline(ForPeer, Limit, PlanningTestTick, Bytes)
					: Coordinator.ProducePendingRelevance(ForPeer, Limit, PlanningTestTick, Bytes);
			};
			auto Add = [&](ConnectionId ForPeer, ReplicationEpoch Epoch, const PeerRelevanceSelection &Input) -> ReplicationProduceResult {
				if (!Planned) return Coordinator.AddPeerBounded(ForPeer, Epoch, Input);
				auto Registered = Coordinator.RegisterPeerPlanned(ForPeer, Epoch, std::make_shared<const PeerRelevanceSelection>(Input));
				if (!Registered.Succeeded()) return {{}, Registered.Error};
				return Await(ForPeer, true, Configuration.PeerQuantum, MaximumReplicationFrameBytes);
			};
			auto Record = [&](ConnectionId ForPeer, const PeerRelevanceSelection &Input, std::uint64_t Tick) {
				return Planned ? Coordinator.RequestPlanning(ForPeer, std::make_shared<const PeerRelevanceSelection>(Input), PlanningTestTick)
					: Coordinator.RecordDesiredState(ForPeer, Input, Tick);
			};
			auto Produce = [&](ConnectionId ForPeer, std::size_t Limit, std::uint64_t Tick, std::size_t Bytes = MaximumReplicationFrameBytes) {
				return Planned ? Await(ForPeer, false, Limit, Bytes) : Coordinator.ProducePendingRelevance(ForPeer, Limit, Tick, Bytes);
			};
			auto Pending = [&](ConnectionId ForPeer) {
				if (Planned) for (int Attempt = 0; Attempt != 10'000 && Coordinator.HasPendingStructuralWork() && !Coordinator.IsPlanningReady(ForPeer); ++Attempt)
					Coordinator.ProcessPlanning(++PlanningTestTick);
				return Coordinator.HasPendingRelevance(ForPeer);
			};
			auto Apply = [&](ReplicationProduceResult Produced) {
				Check(Produced.Frame.has_value(), "KI-007 matrix produces a dependency-complete frame");
				if (!Produced.Frame) { std::cerr << "[Replication:KI007] " << Produced.Error << '\n'; return false; }
				const auto Result = Replica.ApplyFrame(*Produced.Frame);
				if (!Result.Succeeded()) std::cerr << "[Replication:KI007] scenario=" << Scenario << " " << Result.Message << '\n';
				Check(Result.Succeeded(), "KI-007 matrix keeps strict client preflight valid");
				if (!Result.Succeeded()) return false;
				Check(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(),
					"KI-007 metadata and Known commit only with the accepted frame");
				return true;
			};
			Apply(Add(Connection, ReplicationEpoch(1), Selection));
			ReplicationFrame Invalid{ReplicationProtocolVersion, ReplicationMessageKind::Incremental,
				ReplicationEpoch(1), ReliableReplicationSequence(2)};
			Invalid.Operations.push_back({Invalid.Epoch, UnpublishReplication{OldId}});
			Check(!Replica.ApplyFrame(Invalid).Succeeded() && Replica.Resolve(OldId),
				"strict preflight still rejects removal without four required reference updates");
			for (const auto &Referrer : Referrers) {
				Referrer->SetPart0(Scenario == 1 ? std::nullopt : std::optional<std::shared_ptr<BasePart>>(B));
				Referrer->SetPart1(Scenario == 1 ? std::nullopt : std::optional<std::shared_ptr<BasePart>>(B));
			}
			std::erase(Selection.DesiredObjects, OldId);
			if (Scenario == 3) Selection.DesiredObjects.push_back(B->GetObjectId());
			if (Scenario == 4 || Scenario == 5 || Scenario == 9) {
				for (const auto &Referrer : Referrers) {
					std::erase(Selection.DesiredObjects, Referrer->GetObjectId());
					if (Scenario == 5) Referrer->Destroy();
				}
			}
			std::ranges::sort(Selection.DesiredObjects);
			Check(Record(Connection, Selection, 1).Succeeded(), "KI-007 matrix records current Desired");
			if (Scenario == 8) {
				const auto Oversized = Produce(Connection, 4, 1);
				Check(!Oversized.Frame && Oversized.Error == "Structural dependency group and reference fixups exceed the peer quantum" &&
					Coordinator.GetView(Connection)->Knows(OldId), "KI-007 oversized complete group fails without an invalid prefix");
				Coordinator.RemovePeer(Connection); World->Destroy(); continue;
			}
			if (Scenario == 9) {
				auto First = Produce(Connection, 4, 1);
				Check(First.SelectedTransitions <= 4, "temporary soft clear pressure never exceeds the peer quantum");
				Apply(std::move(First));
				// Full ObjectId order may let one referrer Leave make the remaining
				// clear/removal group fit in this same frame. Either order is valid.
				if (Pending(Connection)) Apply(Produce(Connection, 4, 2));
				Check(!Pending(Connection) && !Replica.Resolve(OldId), "soft-referrer-first progress makes removal fit the unchanged quantum");
				Coordinator.RemovePeer(Connection); World->Destroy(); continue;
			}
			if (Scenario < 3 || Scenario == 6 || Scenario == 7) {
				const auto Deferred = Produce(Connection, 4, 1);
				Check(!Deferred.Frame && Coordinator.GetView(Connection)->Knows(OldId),
					"four reference replacements plus removal defer together under a four-operation allowance");
			}
			if (Scenario == 0) {
				const auto ByteRejected = Produce(Connection, 32, 1, 40);
				Check(!ByteRejected.Frame && ByteRejected.Error == "Atomic structural group exceeds the negotiated reliable message limit" &&
					Coordinator.GetView(Connection)->Knows(OldId), "byte-limit retries never accept a partial reference/removal group");
			}
			if (Scenario == 7) {
				Check(Coordinator.RemovePeer(Connection) && !Coordinator.GetView(Connection), "disconnect discards pending accepted-reference ownership");
				const ConnectionId FreshConnection{53, 2};
				Replica.Reset();
				auto Fresh = Add(FreshConnection, ReplicationEpoch(2), Selection);
				Check(Fresh.Frame && Replica.ApplyFrame(*Fresh.Frame).Succeeded() &&
					Coordinator.CommitSchedulerAcceptance(FreshConnection, Fresh.Frame->Sequence).Succeeded() && !Replica.Resolve(OldId),
					"reconnect cannot inherit the previous generation's accepted references");
				Coordinator.RemovePeer(FreshConnection); World->Destroy(); continue;
			}
			auto Removal = Produce(Connection, 32, 2);
			Check(Coordinator.GetView(Connection)->Knows(OldId), "preparation never removes Known");
			if (Scenario < 3 || Scenario == 6) Check(Removal.SelectedTransitions == 5,
				"exact removal cost is four clear/replacement properties plus one Leave");
			if (Scenario == 6) {
				// Immutable prepared values survive both target and referrer destruction.
				A->Destroy(); Referrers[0]->Destroy();
			}
			Apply(std::move(Removal));
			Check(!Replica.Resolve(OldId) && !Coordinator.GetView(Connection)->Knows(OldId), "old identity is removed only after valid acceptance");
			if (Scenario < 4) {
				for (const auto &Referrer : Referrers) {
					const auto Client = std::dynamic_pointer_cast<WeldConstraint>(Replica.Resolve(Referrer->GetObjectId()));
					const bool HasReplacement = Scenario == 2 || Scenario == 3;
					Check(Client && Client->GetPart0().has_value() == HasReplacement && Client->GetPart1().has_value() == HasReplacement,
						"all reference properties reflect the peer-valid replacement or nil");
				}
			}
			if (Scenario == 0) {
				A->Destroy();
				auto Reload = std::make_shared<Part>(); Reload->SetParent(World);
				Check(Reload->GetObjectId() != OldId, "reacquisition has a fresh generation-bearing identity");
				for (const auto &Referrer : Referrers) { Referrer->SetPart0(Reload); Referrer->SetPart1(Reload); }
				Selection.DesiredObjects.push_back(Reload->GetObjectId()); std::ranges::sort(Selection.DesiredObjects);
				Check(Record(Connection, Selection, 3).Succeeded(), "reacquisition updates Desired without stale reference state");
				Apply(Produce(Connection, 32, 3));
				Check(Replica.Resolve(Reload->GetObjectId()) && !Replica.Resolve(OldId), "fresh Enter restores references without old-identity resurrection");
			}
			if (Scenario == 6) {
				Selection.DesiredObjects = {World->GetObjectId(), Referrers[1]->GetObjectId()};
				std::ranges::sort(Selection.DesiredObjects);
				Check(Record(Connection, Selection, 3).Succeeded(), "post-prepare destruction reconciles current generation");
				Apply(Produce(Connection, 32, 3));
			}
			Check(!Pending(Connection), "KI-007 matrix converges without invisible pending work");
			Coordinator.RemovePeer(Connection);
			Check(Coordinator.GetMetrics().AcceptedAncestryLogicalBytes == 0, "disconnect releases all accepted reference storage");
			World->Destroy();
		}
		std::cout << "[Replication:KI007] planned=" << Planned << " scenarios=10 referenceRecordBytes="
			<< sizeof(std::pair<const InstanceProperty *, ObjectId>) << " referenceVectorBytes="
			<< sizeof(std::vector<std::pair<const InstanceProperty *, ObjectId>>) << '\n';
	}

	void TestOrderedPlanningLookup() {
		std::map<ObjectId, std::uint64_t> Values;
		for (std::uint32_t Slot = 1; Slot <= 1024; ++Slot) Values.emplace(ObjectId{Slot, 2}, Slot);
		auto Verify = [&](const std::vector<ObjectId> &Queries) {
			runtime_detail::WorkSample Sample{};
			auto Cursor = Values.cbegin();
			{
				runtime_detail::WorkCapture Capture(&Sample);
				for (const auto Object : Queries)
					Check(detail::FindPlanningObject(Values, Cursor, Object) == Values.find(Object),
						"ordered planning join equals independent tree lookup, including absent generations");
			}
			Check(Sample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningLookupMaximumAdvances)] <=
				detail::MaximumPlanningLookupAdvances, "each planning lookup walks at most eight nodes before a tree jump");
			Check(Sample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningLookupAdvances)] <=
				Queries.size() * detail::MaximumPlanningLookupAdvances, "sparse planning lookup cannot drain the catalog");
			return Sample;
		};
		std::vector<ObjectId> Dense;
		for (std::uint32_t Slot = 0; Slot <= 1025; ++Slot)
			for (std::uint32_t Generation = 1; Generation <= 3; ++Generation) Dense.push_back({Slot, Generation});
		const auto DenseSample = Verify(Dense);
		Check(DenseSample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningLookupSearches)] == 0,
			"dense planning joins do not restart tree searches for every referrer");
		const auto SparseSample = Verify({{700, 1}, {700, 2}, {700, 3}, {1000, 2}, {2048, 1}});
		Check(SparseSample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningLookupSearches)] != 0,
			"sparse planning joins use bounded jumps rather than full-world traversal");
		Values.erase(ObjectId{700, 2}); Values.emplace(ObjectId{700, 3}, 1700);
		Verify({{700, 2}, {700, 3}, {1000, 2}}); // New safe-point pass, no iterator survives mutation.
		Values.clear(); Verify({{1, 1}, {2048, 3}});
		std::cout << "[Replication:PlanningLookup] maximumAdvances=" << detail::MaximumPlanningLookupAdvances
			<< " iteratorBytes=" << sizeof(decltype(Values)::const_iterator) << '\n';
	}

	void TestReferencePropertyIndex() {
		auto World = std::make_shared<DataModel>();
		auto A = std::make_shared<Part>(); A->SetParent(World);
		auto B = std::make_shared<Part>(); B->SetParent(World);
		auto Referrer = std::make_shared<WeldConstraint>(); Referrer->SetParent(World);
		Referrer->SetPart0(A); Referrer->SetPart1(A);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), A->GetObjectId(), Referrer->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const ConnectionId Connection{54, 1};
		ReplicaApplier Replica;
		auto Baseline = Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection);
		Check(Baseline.Frame && Replica.ApplyFrame(*Baseline.Frame).Succeeded() &&
			Coordinator.CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded(), "reference-index fixture bootstraps");
		Referrer->SetPart0(B); Referrer->SetPart1(std::nullopt);
		std::erase(Selection.DesiredObjects, A->GetObjectId());
		Check(Coordinator.RecordDesiredState(Connection, Selection, 1).Succeeded(), "reference and nil mutations replace indexed publication");
		const auto IndexBytes = Coordinator.GetMetrics().CatalogReferenceIndexBytes;
		Referrer->SetName("scalar replacement keeps reference topology");
		Check(Coordinator.RecordDesiredState(Connection, Selection, 2).Succeeded() &&
			Coordinator.GetMetrics().CatalogReferenceIndexBytes == IndexBytes, "scalar publication replacement preserves bounded index footprint");
		runtime_detail::WorkSample Sample{};
		ReplicationProduceResult Removal;
		{
			runtime_detail::WorkCapture Capture(&Sample);
			Removal = Coordinator.ProducePendingRelevance(Connection, 4, 2);
		}
		Check(Sample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::FixupProperties)] == 1 &&
			Sample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::RestoreFixupProperties)] == 1 &&
			Sample.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::FixupScalarValues)] == 0,
			"fixup passes inspect only the current non-nil canonical reference property");
		Check(Removal.Frame && Removal.SelectedTransitions == 3 && Replica.ApplyFrame(*Removal.Frame).Succeeded() &&
			Coordinator.CommitSchedulerAcceptance(Connection, Removal.Frame->Sequence).Succeeded(),
			"index filtering preserves both accepted-old-edge clears, including current nil");
		Referrer->Destroy(); A->Destroy(); B->Destroy();
		Selection.DesiredObjects = {World->GetObjectId()};
		Check(Coordinator.RecordDesiredState(Connection, Selection, 3).Succeeded(), "index retirement observes destroyed generation");
		auto Destroy = Coordinator.ProducePendingRelevance(Connection, 4, 3);
		Check(Destroy.Frame && Replica.ApplyFrame(*Destroy.Frame).Succeeded() &&
			Coordinator.CommitSchedulerAcceptance(Connection, Destroy.Frame->Sequence).Succeeded(), "indexed referrer retires dependency-safely");
		Coordinator.ProcessCatalogRetirement(4);
		Check(Coordinator.GetMetrics().CatalogObjects == 1 && Coordinator.GetMetrics().CatalogReferenceIndexBytes ==
			sizeof(std::vector<const std::map<std::string, WireValue>::value_type *>), "retirement releases all borrowed index nodes except the live root's empty vector");
		Coordinator.RemovePeer(Connection); World->Destroy();
	}
}

int main() {
	BootstrapNativeRuntimeSchema();
	try {
		test::TestReliableEnvelopeProfileModel();
		test::TestAtomicGroupDistributions();
		test::TestPreAcceptanceByteDeferral();
	} catch (const std::exception &Error) {
		std::cerr << "[Network:EnvelopeContract] " << Error.what() << '\n';
		++Failures;
	}
	TestPlanningContinuation();
	TestPlanningReferenceChange();
	TestPlanningStaleCriticalInput();
	TestPlanningLimits();
	TestDependencyInvalidation();
	TestBoundedCatalogRetirement();
	TestBoundedPeerEvaluation();
	TestStableRelevanceSelection();
	TestOrderedHysteresisReference();
	TestSelectedAncestryOrdering();
	TestBoundedDependencyGroupExpansion();
	TestAcceptedMembershipUpdates();
	TestBoundedJournalReads();
	TestReparentBeforeParentLeave();
	TestUnrelatedHardNilFixups();
	TestSoftReferenceReplacementBeforeLeave();
	TestAcceptedReferenceRemovalMatrix();
	TestAcceptedReferenceRemovalMatrix(true);
	TestOrderedPlanningLookup();
	TestReferencePropertyIndex();
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

	// The opposite scheduling order is equally legal: Enter can be accepted
	// before this peer has read any pre-publication journal history.
	{
		auto HistoryWorld = std::make_shared<DataModel>();
		ReplicationCoordinator HistoryCoordinator(HistoryWorld, {}, true, ScheduledConfiguration);
		const ConnectionId HistoryConnection{31, 1};
		PeerRelevanceSelection HistorySelection{
			.RequiredObjects = {HistoryWorld->GetObjectId()}, .DesiredObjects = {HistoryWorld->GetObjectId()}
		};
		ReplicaApplier HistoryReplica;
		Check(HistoryCoordinator.RegisterPeerBounded(HistoryConnection, ReplicationEpoch(1), HistorySelection).Succeeded(),
			"history fixture registers bounded peer");
		auto AcceptHistory = [&](ReplicationProduceResult Produced) {
			if (!Produced.Succeeded()) return false;
			return HistoryReplica.ApplyFrame(*Produced.Frame).Succeeded() &&
				HistoryCoordinator.CommitSchedulerAcceptance(HistoryConnection, Produced.Frame->Sequence).Succeeded();
		};
		Check(AcceptHistory(HistoryCoordinator.ProducePendingBaseline(HistoryConnection, 1, 1)),
			"history fixture accepts root baseline");
		auto HistoryObject = std::make_shared<Folder>();
		HistoryObject->SetParent(HistoryWorld);
		(void)HistoryObject->ApplyAttributeMutation("Revision", WireValue(1.0));
		(void)HistoryObject->ApplyAttributeMutation("Revision", WireValue(2.0));
		HistoryObject->SetName("CurrentPublication");
		HistorySelection.DesiredObjects.push_back(HistoryObject->GetObjectId());
		std::ranges::sort(HistorySelection.DesiredObjects);
		Check(HistoryCoordinator.RecordDesiredState(HistoryConnection, HistorySelection, 2).Succeeded(),
			"history fixture records ordinary Enter");
		auto PreparedHistory = HistoryCoordinator.ProducePendingRelevance(HistoryConnection, 1, 2);
		Check(PreparedHistory.Succeeded() && !HistoryCoordinator.GetView(HistoryConnection)->Knows(HistoryObject->GetObjectId()),
			"prepared publication is not Known before exact acceptance");
		// A post-prepare mutation must not be swallowed by an acceptance-time
		// watermark, even if another peer refreshes the shared catalog first.
		(void)HistoryObject->ApplyAttributeMutation("Revision", WireValue(3.0));
		const ConnectionId RefreshConnection{32, 1};
		Check(HistoryCoordinator.AddPeer(RefreshConnection, ReplicationEpoch(1), HistorySelection).Succeeded(),
			"second peer refreshes catalog after first publication was prepared");
		Check(AcceptHistory(std::move(PreparedHistory)), "history fixture accepts exact prepared publication");
		auto HistoryReplicaObject = HistoryReplica.Resolve(HistoryObject->GetObjectId());
		Check(HistoryReplicaObject && HistoryReplicaObject->GetAttributeValue("Revision") == std::optional<WireValue>(2.0),
			"prepared publication preserves its captured value");
		std::size_t HistoryOperations = 0;
		bool ReplayedOldValue = false;
		for (std::size_t Iteration = 0; Iteration < 32; ++Iteration) {
			auto Produced = HistoryCoordinator.ProduceIncremental(HistoryConnection, 1);
			if (!Produced.Succeeded()) break;
			HistoryOperations += Produced.Frame->Operations.size();
			Check(AcceptHistory(std::move(Produced)), "bounded history frame applies and commits");
			ReplayedOldValue = ReplayedOldValue || HistoryReplicaObject->GetAttributeValue("Revision") == std::optional<WireValue>(1.0);
		}
		Check(!ReplayedOldValue, "newly materialized object never rolls back to pre-publication Attribute history");
		Check(HistoryOperations == 1, "only the post-prepare mutation consumes journal work after complete Enter");
		Check(HistoryReplicaObject && HistoryReplicaObject->GetAttributeValue("Revision") == std::optional<WireValue>(3.0),
			"post-prepare mutation is not lost across scheduler acceptance");
		HistoryCoordinator.RemovePeer(HistoryConnection);
		HistoryCoordinator.RemovePeer(RefreshConnection);
		HistoryWorld->Destroy();
	}

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

	for (const bool Planned : {false, true}) for (const std::size_t NameBytes : {0u, 24u * 1024, 60u * 1024}) {
		// Player.Character is a hard dependency. A transport-byte retry may not
		// publish half that group or advance Known after an oversized attempt.
		auto BytePlayer = Runtime.Players->CreateSessionPlayer({"byte-relevance-test", "hard-group"});
		auto ByteCharacter = *BytePlayer->GetCharacter();
		if (NameBytes) {
			BytePlayer->SetName(std::string(NameBytes, 'p'));
			ByteCharacter->SetName(std::string(NameBytes, 'c'));
		}
		PeerRelevanceSelection HardSelection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), BytePlayer->GetObjectId()}};
		std::ranges::sort(HardSelection.DesiredObjects);
		ReplicationCoordinator HardByteCoordinator(World);
		const ConnectionId HardByteConnection{21, 1};
		Check((Planned ? HardByteCoordinator.RegisterPeerPlanned(HardByteConnection, ReplicationEpoch(1),
			std::make_shared<const PeerRelevanceSelection>(HardSelection)) :
			HardByteCoordinator.RegisterPeerBounded(HardByteConnection, ReplicationEpoch(1), HardSelection)).Succeeded(),
			"hard-reference byte fixture registers");
		ReplicaApplier HardByteReplica;
		std::uint64_t PlanningTick = 0;
		std::uint64_t LastPlanningGroups = 0;
		for (std::uint64_t Tick = 1; Tick <= 12 && !HardByteCoordinator.GetView(HardByteConnection)->Knows(BytePlayer->GetObjectId()); ++Tick) {
			const auto KnownBefore = HardByteCoordinator.GetView(HardByteConnection)->KnownObjects;
			std::size_t Limit = 40 * 1024;
			auto Produce = [&] {
				runtime_detail::WorkSample GroupWork{};
				runtime_detail::WorkCapture Capture(&GroupWork);
				if (Planned) for (int Attempt = 0; Attempt != 10000 && !HardByteCoordinator.IsPlanningReady(HardByteConnection); ++Attempt)
					HardByteCoordinator.ProcessPlanning(++PlanningTick);
				const auto Groups = GroupWork.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningCompletedGroups)];
				if (Groups) LastPlanningGroups = Groups;
				return Tick == 1 ? HardByteCoordinator.ProducePendingBaseline(HardByteConnection, 16, Tick, Limit)
					: HardByteCoordinator.ProducePendingRelevance(HardByteConnection, 16, Tick, Limit);
			};
			auto LegalGroup = Produce();
			if (!LegalGroup.Succeeded()) {
				Check(HardByteCoordinator.GetView(HardByteConnection)->KnownObjects == KnownBefore,
					"over-limit hard-reference candidate does not advance Known");
				Limit = 2 * NameBytes + 16 * 1024;
				LegalGroup = Produce();
			}
			Check(LegalGroup.Succeeded() && LegalGroup.Frame && HardByteReplica.ApplyFrame(*LegalGroup.Frame).Succeeded(),
				"byte-bounded hard-reference frame applies without a dangling reference");
			if (!LegalGroup.Succeeded() || !LegalGroup.Frame) break;
			const auto Encoded = EncodeReplicationFrame(*LegalGroup.Frame);
			Check(Encoded && Encoded->size() <= Limit, "hard-reference wire frame respects its supplied byte budget");
			if (Encoded) std::cout << "[Network:AtomicBytes] planned=" << Planned << " nameBytes=" << NameBytes << " frameBytes=" << Encoded->size()
				<< " operations=" << LegalGroup.Frame->Operations.size() << " knownBefore=" << KnownBefore.size()
				<< " planningGroups=" << LastPlanningGroups << '\n';
			auto PlayerReplica = std::dynamic_pointer_cast<Player>(HardByteReplica.Resolve(BytePlayer->GetObjectId()));
			Check(!PlayerReplica || (PlayerReplica->GetCharacter() &&
				*PlayerReplica->GetCharacter() == HardByteReplica.Resolve(ByteCharacter->GetObjectId())),
				"published hard reference always resolves within the accepted prefix");
			Check(HardByteCoordinator.CommitSchedulerAcceptance(HardByteConnection, LegalGroup.Frame->Sequence).Succeeded(),
				"hard-reference group commits only after scheduler acceptance");
		}
		// HasPendingRelevance includes the continuation's charged disposal, not
		// just unaccepted transitions. Let that existing cleanup finish too.
		if (Planned) for (int Attempt = 0; Attempt != 10000 && HardByteCoordinator.HasPendingStructuralWork(); ++Attempt)
			HardByteCoordinator.ProcessPlanning(++PlanningTick);
		Check(!HardByteCoordinator.HasPendingRelevance(HardByteConnection) && HardByteReplica.Resolve(BytePlayer->GetObjectId()),
			"hard-reference byte slices eventually converge");
		HardByteCoordinator.RemovePeer(HardByteConnection);
	}
	Runtime.Destroy();
	{
		// Legal immutable content can expand beyond a negotiated reliable frame.
		// Production selection must drain smaller slices without advancing Known
		// before scheduler acceptance or raising any existing transition ceiling.
		auto ByteWorld = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> ByteObjects;
		PeerRelevanceSelection ByteSelection{.RequiredObjects = {ByteWorld->GetObjectId()},
			.DesiredObjects = {ByteWorld->GetObjectId()}};
		for (std::size_t Index = 0; Index < 8; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetName(std::string(16 * 1024, static_cast<char>('a' + Index)));
			Object->SetParent(ByteWorld);
			ByteSelection.DesiredObjects.push_back(Object->GetObjectId());
			ByteObjects.push_back(std::move(Object));
		}
		std::ranges::sort(ByteSelection.DesiredObjects);
		ReplicationCoordinator ByteCoordinator(ByteWorld);
		const ConnectionId ByteConnection{20, 1};
		constexpr std::size_t ByteLimit = 48 * 1024;
		Check(ByteCoordinator.RegisterPeerBounded(ByteConnection, ReplicationEpoch(1), ByteSelection).Succeeded(),
			"byte-bounded peer registration succeeds");
		ReplicaApplier ByteReplica;
		auto ApplyByteFrame = [&](ReplicationProduceResult Produced) {
			Check(Produced.Succeeded() && Produced.Frame.has_value(), "byte-bounded frame is produced");
			if (!Produced.Succeeded() || !Produced.Frame) return false;
			auto Encoded = EncodeReplicationFrame(*Produced.Frame);
			Check(Encoded && Encoded->size() <= ByteLimit && Produced.SelectedTransitions <= 9,
				"structural slice respects both work and negotiated byte ceilings");
			if (!Encoded) return false;
			auto Decoded = DecodeReplicationFrame(*Encoded);
			Check(Decoded && ByteReplica.ApplyFrame(*Decoded).Succeeded(), "byte-bounded wire frame applies normally");
			Check(ByteCoordinator.CommitSchedulerAcceptance(ByteConnection, Produced.Frame->Sequence).Succeeded(),
				"byte-bounded Known state commits only on scheduler acceptance");
			return true;
		};
		Check(!ByteCoordinator.ProducePendingBaseline(ByteConnection, 9, 1, 0).Succeeded(),
			"zero negotiated byte budget fails without consuming pending work");
		ApplyByteFrame(ByteCoordinator.ProducePendingBaseline(ByteConnection, 9, 1, ByteLimit));
		for (std::uint64_t Tick = 2; Tick < 20 && ByteCoordinator.HasPendingRelevance(ByteConnection); ++Tick)
			if (!ApplyByteFrame(ByteCoordinator.ProducePendingRelevance(ByteConnection, 9, Tick, ByteLimit))) break;
		Check(!ByteCoordinator.HasPendingRelevance(ByteConnection), "large legal publication drains under transport limit");
		for (const auto &Object : ByteObjects) {
			auto Replica = ByteReplica.Resolve(Object->GetObjectId());
			Check(Replica && Replica->GetName() == Object->GetName(), "sliced publication preserves exact names");
			Object->SetName(std::string(17 * 1024, 'z'));
		}
		for (std::size_t Iteration = 0; Iteration < 10; ++Iteration) {
			auto Produced = ByteCoordinator.ProduceIncremental(ByteConnection, 9, ByteLimit);
			if (!Produced.Succeeded()) break;
			if (!ApplyByteFrame(std::move(Produced))) break;
		}
		for (const auto &Object : ByteObjects) {
			auto Replica = ByteReplica.Resolve(Object->GetObjectId());
			Check(Replica && Replica->GetName() == Object->GetName(), "byte-bounded journal update preserves every property");
		}
		ByteObjects.front()->SetName(std::string(64 * 1024, 'q'));
		Check(!ByteCoordinator.ProduceIncremental(ByteConnection, 9, ByteLimit).Succeeded(),
			"indivisible oversized property fails closed without false acceptance");
		ByteCoordinator.RemovePeer(ByteConnection);
		ByteWorld->Destroy();
	}
	if (Failures == 0) std::cout << "Replication relevance tests passed\n";
	return Failures == 0 ? 0 : 1;
}
