#pragma once
#include "ReliableEnvelopeContractFixture.hpp"

namespace gargantuan::test {
struct NameCoalescingFixture {
	std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
	std::shared_ptr<Folder> First = std::make_shared<Folder>(), Second = std::make_shared<Folder>();
	std::unique_ptr<network::ReplicationCoordinator> Coordinator;
	network::ReplicaApplier Replica;
	network::ConnectionId Connection{98, 1};
	network::PeerRelevanceSelection Selection;
	NameCoalescingFixture() {
		First->SetParent(World); Second->SetParent(World);
		Selection.RequiredObjects = {World->GetObjectId()};
		Selection.DesiredObjects = {World->GetObjectId(), First->GetObjectId(), Second->GetObjectId()};
		std::ranges::sort(Selection.DesiredObjects);
		Coordinator = std::make_unique<network::ReplicationCoordinator>(World);
		Accept(Coordinator->AddPeerBounded(Connection, network::ReplicationEpoch(1), Selection));
	}
	~NameCoalescingFixture() { Coordinator->RemovePeer(Connection); World->Destroy(); }
	void Accept(network::ReplicationProduceResult Produced) {
		EnvelopeRequire(Produced.Frame && Replica.ApplyFrame(*Produced.Frame).Succeeded() &&
			Coordinator->CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(), "Name frame applies and accepts");
	}
	network::ReplicationProduceResult Produce(std::size_t Operations = 512, std::size_t Reads = 2048,
		std::size_t Available = 524256) {
		return Coordinator->ProduceIncremental(Connection, Operations, 524256, Reads, Available);
	}
	static std::vector<std::string> Names(const network::ReplicationProduceResult &Produced) {
		std::vector<std::string> Values;
		if (Produced.Frame) for (const auto &Operation : Produced.Frame->Operations)
			if (const auto *Property = std::get_if<network::PropertyReplicationUpdate>(&Operation.Intent);
				Property && Property->PropertyName == "Name") Values.push_back(std::get<std::string>(Property->Value));
		return Values;
	}
	void RefreshDependencies() {
		EnvelopeRequire(Coordinator->RecordDesiredState(Connection, Selection, 1).Succeeded(), "Name fixture refreshes dependencies");
	}
};

inline void TestNameCoalescing() {
	{
		NameCoalescingFixture F;
		F.First->SetName("A"); F.First->SetName("B"); F.First->SetName("C");
		auto Deferred = F.Produce(512, 2048, 0);
		EnvelopeRequire(Deferred.DeferredForBytes && F.Coordinator->GetJournalLag(F.Connection) == 3,
			"Name credit deferral preserves all three source records");
		auto Prepared = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Prepared) == std::vector<std::string>{"C"} &&
			F.Coordinator->GetJournalLag(F.Connection) == 3, "three Names prepare one current output without cursor progress");
		EnvelopeRequire(!F.Coordinator->CommitSchedulerAcceptance(F.Connection,
			network::ReliableReplicationSequence(Prepared.Frame->Sequence.Value() + 1)).Succeeded(),
			"wrong sequence cannot accept covered history");
		EnvelopeRequire(F.Coordinator->DiscardSchedulerPreparation(F.Connection, Prepared.Frame->Sequence).Succeeded(),
			"discard Name preparation");
		F.First->SetName("D");
		Prepared = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Prepared) == std::vector<std::string>{"D"} &&
			F.Coordinator->GetJournalLag(F.Connection) == 4, "discarded Name is reconstructed from current catalog");
		F.Accept(std::move(Prepared));
		EnvelopeRequire(F.Coordinator->GetJournalLag(F.Connection) == 0 &&
			F.Replica.Resolve(F.First->GetObjectId())->GetName() == F.First->GetName(), "acceptance commits exactly four covered records");
		F.First->SetName("single");
		Prepared = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Prepared) == std::vector<std::string>{"single"}, "single Name remains one unchanged update");
		F.Accept(std::move(Prepared));
	}
	{
		NameCoalescingFixture F;
		for (int Index = 0; Index < 100; ++Index) {
			F.First->SetName("first" + std::to_string(Index));
			F.Second->SetName("second" + std::to_string(Index));
		}
		std::size_t Frames = 0, Operations = 0, Reads = 0;
		for (int Attempt = 0; Attempt < 100; ++Attempt) {
			auto Prepared = F.Produce(1, 7);
			EnvelopeRequire(Prepared.JournalRecordsExamined <= 7, "coalescing preserves bounded source examination");
			Reads += Prepared.JournalRecordsExamined;
			if (Prepared.Frame) { ++Frames; Operations += Prepared.Frame->Operations.size(); F.Accept(std::move(Prepared)); }
			if (!F.Coordinator->GetJournalLag(F.Connection)) break;
		}
		EnvelopeRequire(Frames == 2 && Operations == 2 && Reads >= 200 && F.Coordinator->GetJournalLag(F.Connection) == 0 &&
			F.Replica.Resolve(F.First->GetObjectId())->GetName() == F.First->GetName() &&
			F.Replica.Resolve(F.Second->GetObjectId())->GetName() == F.Second->GetName(),
			"independent objects cover bounded read slices with two accepted current Names");
	}
	{
		NameCoalescingFixture F;
		F.First->SetName("prepared");
		auto Prepared = F.Produce();
		F.First->SetName("later");
		// Refresh the shared catalog while this peer still has an unaccepted frame.
		const network::ConnectionId Observer{99, 1};
		auto Baseline = F.Coordinator->AddPeerBounded(Observer, network::ReplicationEpoch(1), F.Selection);
		EnvelopeRequire(Baseline.Frame.has_value(), "second peer refreshes current catalog");
		F.Accept(std::move(Prepared));
		Prepared = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Prepared) == std::vector<std::string>{"later"},
			"acceptance cannot extend coverage to a later catalog revision");
		F.Accept(std::move(Prepared)); F.Coordinator->RemovePeer(Observer);
	}
	// Every non-Name journal operation is a conservative barrier, including
	// unrelated identity/hierarchy/reference records. Closed segments stay ordered.
	for (int Barrier = 0; Barrier < 4; ++Barrier) {
		NameCoalescingFixture F;
		F.First->SetName("before-A"); F.First->SetName("before-B");
		std::shared_ptr<Instance> Created;
		if (Barrier == 0) (void)F.First->ApplyAttributeMutation("Barrier", WireValue(true));
		if (Barrier == 1) { Created = std::make_shared<Folder>(); Created->SetParent(F.World); }
		if (Barrier == 2) F.First->SetParent(F.Second);
		if (Barrier == 3) {
			auto Weld = std::make_shared<WeldConstraint>(); Weld->SetParent(F.World);
			auto PartValue = std::make_shared<Part>(); PartValue->SetParent(F.World);
			Weld->SetPart0(PartValue); Created = Weld;
		}
		F.First->SetName("after-A"); F.First->SetName("after-B");
		F.RefreshDependencies();
		auto Prepared = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Prepared) ==
			std::vector<std::string>{"before-A", "before-B", "after-B"},
			"lifecycle, hierarchy, attribute and reference barriers preserve closed Name history");
		F.Accept(std::move(Prepared));
	}
	{
		NameCoalescingFixture F;
		F.First->SetName("old");
		F.Accept(F.Produce());
		const auto OldId = F.First->GetObjectId();
		F.First->Destroy();
		auto Replacement = std::make_shared<Folder>(); Replacement->SetParent(F.World); Replacement->SetName("replacement");
		F.Selection.DesiredObjects = {F.World->GetObjectId(), F.Second->GetObjectId(), Replacement->GetObjectId()};
		std::ranges::sort(F.Selection.DesiredObjects);
		F.Accept(F.Coordinator->UpdateRelevance(F.Connection, F.Selection));
		Replacement->SetName("new-A"); Replacement->SetName("new-B");
		for (int Attempt = 0; Attempt < 10; ++Attempt) {
			auto Prepared = F.Produce();
			if (Prepared.Frame) F.Accept(std::move(Prepared));
			if (!F.Coordinator->GetJournalLag(F.Connection)) break;
		}
		EnvelopeRequire(OldId != Replacement->GetObjectId() && !F.Replica.Resolve(OldId) &&
			F.Replica.Resolve(Replacement->GetObjectId())->GetName() == "new-B",
			"destroy/recreate does not inherit accepted Name coverage");
	}
	{
		NameCoalescingFixture F;
		F.First->SetName("before-resnapshot"); F.Accept(F.Produce());
		for (std::size_t Index = 0; Index <= DefaultChangeJournalCapacity; ++Index) F.First->SetName(std::to_string(Index));
		F.RefreshDependencies();
		auto Lost = F.Produce();
		EnvelopeRequire(!Lost.Frame && Lost.Error == "Authoritative journal cursor requires a new baseline",
			"accepted Name watermark cannot bridge evicted source history");
		F.Coordinator->RemovePeer(F.Connection);
		F.Connection = {98, 2}; F.Replica = network::ReplicaApplier{};
		F.Accept(F.Coordinator->AddPeerBounded(F.Connection, network::ReplicationEpoch(2), F.Selection));
		F.First->SetName("reload-A"); F.First->SetName("reload-B");
		auto Fresh = F.Produce();
		EnvelopeRequire(NameCoalescingFixture::Names(Fresh) == std::vector<std::string>{"reload-B"}, "fresh baseline owns new Name coverage");
		F.Accept(std::move(Fresh));
	}
	std::cout << "[Network:NameCoalescing] scalar/acceptance/read-bounds/barriers/generation/resnapshot result=pass\n";
}
}
