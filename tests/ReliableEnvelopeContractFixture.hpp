#pragma once

// Historical test-only numeric model and isolated production-planner tests.
// Runtime credit is tested separately by ReliableByteAdmissionFixture.hpp.
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gargantuan::test {
inline void EnvelopeRequire(bool Value, const char *Message) {
	if (!Value) throw std::runtime_error(Message);
}

// Proposed reservation profile: R is a reserved payload-service budget, not an
// opportunistic peak. Physical capacity/overhead validation is a deployment gate.
struct ReliableEnvelopeProfileModel {
	std::uint64_t ConnectionRate = 262144, AggregateRate = 262144;
	std::uint32_t MaximumConnections = 1, StructuralPermille = 750;
	std::uint64_t MaximumGroup = 524288, PeerBurst = 524288, GlobalBurst = 524288;
	std::uint64_t GameplayBurst = 4096, PeerBacklog = 528384, GlobalBacklog = 528384;
	std::uint64_t QueueWindowMilliseconds = 2100;
	bool IsValid() const {
		if (!ConnectionRate || ConnectionRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
			!MaximumConnections || MaximumConnections > 4096 || !StructuralPermille || StructuralPermille >= 1000 ||
			AggregateRate / MaximumConnections < ConnectionRate ||
			AggregateRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) * MaximumConnections ||
			!MaximumGroup || MaximumGroup > 524288 ||
			PeerBacklog > network::NativeMaximumQueuedReliableBytes ||
			GlobalBacklog > network::NativeMaximumSchedulerQueuedReliableBytes ||
			PeerBurst < MaximumGroup || GlobalBurst < MaximumGroup || !GameplayBurst ||
			PeerBacklog < GameplayBurst || PeerBurst > PeerBacklog - GameplayBurst ||
			GlobalBacklog < PeerBacklog || GameplayBurst > GlobalBacklog / MaximumConnections ||
			GlobalBurst > GlobalBacklog - GameplayBurst * MaximumConnections ||
			!QueueWindowMilliseconds || QueueWindowMilliseconds > 60000) return false;
		// Division-first checked arithmetic: supported window never implies an
		// impossible FIFO service class. This is queue time only, not total RTT.
		return PeerBacklog <= ConnectionRate * QueueWindowMilliseconds / 1000 &&
			GlobalBacklog <= AggregateRate * QueueWindowMilliseconds / 1000;
	}
};

inline void TestReliableEnvelopeProfileModel() {
	ReliableEnvelopeProfileModel Profile;
	EnvelopeRequire(Profile.IsValid(), "full structural group plus small-probe burst is numerically feasible");
	// The small-probe burst is not full Remote compatibility. Reserve the current
	// codec ceiling plus adapter framing without reducing allowed Remote payloads.
	auto LargeRemote = Profile;
	LargeRemote.GameplayBurst = network::MaximumRemoteFrameBytes + 32;
	LargeRemote.PeerBacklog = LargeRemote.GlobalBacklog = LargeRemote.MaximumGroup + LargeRemote.GameplayBurst;
	EnvelopeRequire(!LargeRemote.IsValid(), "full Remote allowance cannot fit the small-probe 2.1-second class");
	LargeRemote.QueueWindowMilliseconds = 3100;
	EnvelopeRequire(LargeRemote.IsValid(), "full Remote allowance fits a slower numeric class, not an interactive guarantee");
	Profile.QueueWindowMilliseconds = 100;
	EnvelopeRequire(!Profile.IsValid(), "512 KiB group cannot promise 100 ms at 256 KiB/s");
	Profile.MaximumGroup = Profile.PeerBurst = Profile.GlobalBurst = 2000;
	Profile.PeerBacklog = Profile.GlobalBacklog = 26000;
	EnvelopeRequire(Profile.IsValid(), "small-group laboratory profile fits a 100 ms queue class");
	Profile.MaximumConnections = 500; Profile.ConnectionRate = 1024 * 1024;
	Profile.GlobalBacklog = 500 * Profile.PeerBacklog;
	Profile.AggregateRate = 64 * 1024 * 1024;
	EnvelopeRequire(!Profile.IsValid(), "500 reserved MiB/s cannot fit 64 MiB/s aggregate");
	Profile.AggregateRate = 500ull * 1024 * 1024;
	EnvelopeRequire(Profile.IsValid(), "explicit aggregate reservation funds all admitted peers");
	for (int Case = 0; Case != 11; ++Case) {
		auto Invalid = Profile;
		switch (Case) {
		case 0: Invalid.ConnectionRate = 0; break;
		case 1: Invalid.ConnectionRate = std::numeric_limits<std::uint64_t>::max(); break;
		case 2: Invalid.MaximumConnections = 4097; break;
		case 3: Invalid.StructuralPermille = 1000; break;
		case 4: Invalid.MaximumGroup = 524289; break;
		case 5: Invalid.PeerBurst = 1999; break;
		case 6: Invalid.GlobalBurst = 1999; break;
		case 7: Invalid.PeerBacklog = 1; break;
		case 8: Invalid.AggregateRate = std::numeric_limits<std::uint64_t>::max(); break;
		case 9: Invalid.GlobalBacklog = Invalid.PeerBacklog; break;
		case 10: Invalid.GlobalBacklog = network::NativeMaximumSchedulerQueuedReliableBytes + 1; break;
		}
		EnvelopeRequire(!Invalid.IsValid(), "invalid or overflowing profile fails closed");
	}
	std::cout << "[Network:EnvelopeContract] profileCases=18 result=pass runtimeAdmission=not-tested-by-model\n";
}

struct AtomicGroupFixture {
	std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
	network::ReplicationCoordinator Coordinator{World};
	network::ReplicaApplier Replica;
	network::ConnectionId Connection{97, 1};
	network::PeerRelevanceSelection Selection{{World->GetObjectId()}, {World->GetObjectId()}};
	std::uint64_t Tick = 0;
	std::vector<std::size_t> Sizes, Counts;
	AtomicGroupFixture() {
		EnvelopeRequire(Coordinator.RegisterPeerPlanned(Connection, network::ReplicationEpoch(1),
			std::make_shared<const network::PeerRelevanceSelection>(Selection)).Succeeded(), "group fixture registration");
		Advance(true, false);
	}
	~AtomicGroupFixture() { Coordinator.RemovePeer(Connection); World->Destroy(); }
	void Desired(std::vector<ObjectId> Objects) {
		Objects.push_back(World->GetObjectId()); std::ranges::sort(Objects);
		Objects.erase(std::unique(Objects.begin(), Objects.end()), Objects.end());
		Selection.DesiredObjects = std::move(Objects);
		EnvelopeRequire(Coordinator.RequestPlanning(Connection,
			std::make_shared<const network::PeerRelevanceSelection>(Selection), Tick).Succeeded(), "group fixture Desired");
	}
	void Advance(bool Baseline = false, bool Measure = true, bool ExpectGnsOversize = false) {
		runtime_detail::WorkSample Work{};
		const auto Known = Coordinator.GetView(Connection)->KnownObjects;
		{
			runtime_detail::WorkCapture Capture(&Work);
			for (int Attempt = 0; Attempt != 10000 && !Coordinator.IsPlanningReady(Connection); ++Attempt)
				Coordinator.ProcessPlanning(++Tick);
		}
		EnvelopeRequire(Coordinator.IsPlanningReady(Connection), "group fixture bounded planning converges");
		if (ExpectGnsOversize) {
			const auto Rejected = Coordinator.ProducePendingRelevance(Connection, 512, Tick, 524288 - 32);
			EnvelopeRequire(!Rejected.Frame && Rejected.Error == "Atomic structural group exceeds the negotiated reliable message limit" &&
				Coordinator.GetView(Connection)->KnownObjects == Known, "oversized atomic group rejects without advancing Known");
		}
		auto Produced = Baseline ? Coordinator.ProducePendingBaseline(Connection, 512, Tick)
			: Coordinator.ProducePendingRelevance(Connection, 512, Tick);
		EnvelopeRequire(Produced.Frame.has_value(), "group fixture produces complete frame");
		EnvelopeRequire(Coordinator.GetView(Connection)->KnownObjects == Known, "group discovery is not acceptance");
		const auto Encoded = network::EncodeReplicationFrame(*Produced.Frame);
		EnvelopeRequire(Encoded.has_value(), "group fixture actual codec succeeds");
		const auto Groups = Work.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::PlanningCompletedGroups)];
		if (Measure) {
			// Fail rather than label a multi-group frame as one atomic group.
			EnvelopeRequire(Groups == 1, "isolated observation must cross exactly one actual planner group boundary");
			EnvelopeRequire(Sizes.size() < 1024, "fixed diagnostic sample ceiling");
			Sizes.push_back(Encoded->size()); Counts.push_back(Produced.Frame->Operations.size());
		}
		EnvelopeRequire(Replica.ApplyFrame(*Produced.Frame).Succeeded(), "group fixture strict dependency preflight");
		EnvelopeRequire(Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded(), "group accepted exactly once");
		for (int Attempt = 0; Attempt != 10000 && Coordinator.HasPendingStructuralWork(); ++Attempt)
			Coordinator.ProcessPlanning(++Tick);
		EnvelopeRequire(!Coordinator.HasPendingStructuralWork(), "group fixture disposal drains");
	}
	void Report(const char *Name) {
		EnvelopeRequire(!Sizes.empty(), "group distribution has samples");
		auto Print = [](std::vector<std::size_t> Values) {
			std::ranges::sort(Values);
			for (double P : {.5, .95, .99, 1.0}) std::cout << '/' << Values[static_cast<std::size_t>((Values.size() - 1) * P)];
		};
		std::cout << "[Network:GroupDistribution] case=" << Name << " groups=" << Sizes.size() << " bytesP50P95P99Max=";
		Print(Sizes); std::cout << " opsP50P95P99Max="; Print(Counts); std::cout << '\n';
		Sizes.clear(); Counts.clear();
	}
};

inline void TestPreAcceptanceByteDeferral() {
	AtomicGroupFixture Fixture;
	auto Parent = std::make_shared<Folder>(); Parent->SetParent(Fixture.World);
	auto Child = std::make_shared<Folder>(); Child->SetParent(Parent); Child->SetName("before-credit");
	Fixture.Desired({Child->GetObjectId()});
	for (int Attempt = 0; Attempt != 10000 && !Fixture.Coordinator.IsPlanningReady(Fixture.Connection); ++Attempt)
		Fixture.Coordinator.ProcessPlanning(++Fixture.Tick);
	const auto Known = Fixture.Coordinator.GetView(Fixture.Connection)->KnownObjects;
	auto Deferred = Fixture.Coordinator.ProducePendingRelevance(Fixture.Connection, 512, Fixture.Tick, 524256, 0);
	EnvelopeRequire(Deferred.DeferredForBytes && !Deferred.Frame && Deferred.EncodedFrame.empty() &&
		Deferred.RequiredFrameBytes > 0 && Deferred.SelectedTransitions == 2, "exact cost defers whole dependency closure without retaining bytes");
	EnvelopeRequire(Fixture.Coordinator.IsPlanningReady(Fixture.Connection) &&
		Fixture.Coordinator.GetView(Fixture.Connection)->KnownObjects == Known, "byte deferral neither replans nor accepts Known");
	Child->SetName("changed-during-credit-wait");
	auto Produced = Fixture.Coordinator.ProducePendingRelevance(Fixture.Connection, 512, Fixture.Tick, 524256, 524256);
	EnvelopeRequire(Produced.Frame && !Produced.DeferredForBytes && !Produced.EncodedFrame.empty(), "funded group prepares current payload");
	auto Encoded = network::EncodeReplicationFrame(*Produced.Frame);
	EnvelopeRequire(Encoded && *Encoded == Produced.EncodedFrame, "pre-acceptance bytes equal exact wire encoding");
	EnvelopeRequire(Fixture.Coordinator.GetView(Fixture.Connection)->KnownObjects == Known, "prepared funded group still awaits acceptance");
	EnvelopeRequire(Fixture.Replica.ApplyFrame(*Produced.Frame).Succeeded() &&
		Fixture.Coordinator.CommitSchedulerAcceptance(Fixture.Connection, Produced.Frame->Sequence).Succeeded(), "byte-funded dependency group accepts once");
	EnvelopeRequire(Fixture.Replica.Resolve(Child->GetObjectId())->GetName() == Child->GetName(), "credit wait cannot publish stale ordinary mutation");
	for (int Attempt = 0; Attempt != 10000 && Fixture.Coordinator.HasPendingStructuralWork(); ++Attempt)
		Fixture.Coordinator.ProcessPlanning(++Fixture.Tick);
	Child->SetName("journal-byte-defer");
	auto Journal = Fixture.Coordinator.ProduceIncremental(Fixture.Connection, 512, 524256, 2048, 0);
	EnvelopeRequire(Journal.DeferredForBytes && !Journal.Frame && Journal.JournalRecordsExamined != 0, "journal deferral charges bounded examination without accepting");
	auto FundedJournal = Fixture.Coordinator.ProduceIncremental(Fixture.Connection, 512, 524256, 2048, 524256);
	EnvelopeRequire(FundedJournal.Frame && Fixture.Replica.ApplyFrame(*FundedJournal.Frame).Succeeded() &&
		Fixture.Coordinator.CommitSchedulerAcceptance(Fixture.Connection, FundedJournal.Frame->Sequence).Succeeded(), "journal cursor was not lost while byte-deferred");
	EnvelopeRequire(Fixture.Replica.Resolve(Child->GetObjectId())->GetName() == Child->GetName(), "deferred journal eventual exact state");

	auto Obsolete = std::make_shared<Folder>(); Obsolete->SetParent(Fixture.World);
	const auto OldId = Obsolete->GetObjectId();
	Fixture.Desired({Child->GetObjectId(), OldId});
	for (int Attempt = 0; Attempt != 10000 && !Fixture.Coordinator.IsPlanningReady(Fixture.Connection); ++Attempt)
		Fixture.Coordinator.ProcessPlanning(++Fixture.Tick);
	Deferred = Fixture.Coordinator.ProducePendingRelevance(Fixture.Connection, 512, Fixture.Tick, 524256, 0);
	EnvelopeRequire(Deferred.DeferredForBytes, "generation fixture defers old candidate");
	Obsolete->Destroy();
	auto Fresh = std::make_shared<Folder>(); Fresh->SetParent(Fixture.World);
	EnvelopeRequire(Fresh->GetObjectId() != OldId, "reacquisition has fresh identity");
	Fixture.Desired({Child->GetObjectId(), Fresh->GetObjectId()});
	Fixture.Advance(false, false);
	EnvelopeRequire(!Fixture.Coordinator.GetView(Fixture.Connection)->Knows(OldId) &&
		Fixture.Coordinator.GetView(Fixture.Connection)->Knows(Fresh->GetObjectId()) && !Fixture.Replica.Resolve(OldId),
		"old byte hint cannot resurrect destroyed generation or bind replacement identity");
	std::cout << "[Network:ByteAdmission] preacceptance dependency/journal/mutation/recreation result=pass\n";
}

inline void TestAtomicGroupDistributions() {
	{
		AtomicGroupFixture Fixture;
		auto Player = std::make_shared<gargantuan::Player>(); Player->SetName("EnvelopePlayer"); Player->SetParent(Fixture.World);
		auto Character = std::make_shared<KinematicCharacter>(); Character->SetName("EnvelopeCharacter"); Character->SetParent(Fixture.World);
		Player->SetCharacter(Character);
		Fixture.Desired({Player->GetObjectId()}); Fixture.Advance(); Fixture.Report("player-character");
		Fixture.Desired({}); Fixture.Advance(false, false);
		Player->SetName(std::string(61440, 'p')); Character->SetName(std::string(61440, 'c'));
		Fixture.Desired({Player->GetObjectId()}); Fixture.Advance(); Fixture.Report("player-character-legal-large");
	}
	// Ordinary canonical Part payload, offered individually after its parent is
	// Known. This measures isolated group sizes, not 200/500-peer group frequency.
	{
		AtomicGroupFixture Fixture;
		std::vector<std::shared_ptr<Part>> Parts;
		std::vector<ObjectId> Desired;
		for (int Index = 0; Index != 128; ++Index) {
			auto Value = std::make_shared<Part>(); Value->SetName("ContentScalePart" + std::to_string(Index));
			Value->SetParent(Fixture.World); Parts.push_back(Value); Desired.push_back(Value->GetObjectId());
			Fixture.Desired(Desired); Fixture.Advance();
		}
		Fixture.Report("ordinary-part");
		for (auto &Part : Parts) {
			std::erase(Desired, Part->GetObjectId()); Part->Destroy();
			Fixture.Desired(Desired); Fixture.Advance();
		}
		Fixture.Report("ordinary-destroy");
	}
	{
		AtomicGroupFixture Fixture;
		std::array<std::shared_ptr<Folder>, 32> Nodes;
		for (auto &Node : Nodes) { Node = std::make_shared<Folder>(); Node->SetName("DependencyNode"); }
		std::ranges::sort(Nodes, {}, [](const auto &Node) { return Node->GetObjectId(); });
		// Lowest identity is the deepest node: its first actual group brings all
		// 31 unknown ancestors. No scheduling policy or production limit changes.
		for (std::size_t Index = 0; Index != Nodes.size(); ++Index)
			Nodes[Index]->SetParent(Index + 1 == Nodes.size() ? std::shared_ptr<Instance>(Fixture.World) : Nodes[Index + 1]);
		Fixture.Desired({Nodes.front()->GetObjectId()}); Fixture.Advance(); Fixture.Report("ancestry-32-enter");
	}
	// Deliberately legal worst-case names, separate from representative payloads.
	// Seven nodes fit GNS; eight full names exceed it; reducing one by 644 bytes
	// reaches the exact current 512 KiB complete-message ceiling including adapter.
	for (int Case = 0; Case != 3; ++Case) {
		AtomicGroupFixture Fixture;
		std::vector<std::shared_ptr<Folder>> Nodes(Case == 0 ? 7 : 8);
		for (auto &Node : Nodes) { Node = std::make_shared<Folder>(); Node->SetName(std::string(65536, 'g')); }
		std::ranges::sort(Nodes, {}, [](const auto &Node) { return Node->GetObjectId(); });
		if (Case == 2) Nodes[0]->SetName(std::string(65536 - 644, 'g'));
		for (std::size_t Index = 0; Index != Nodes.size(); ++Index)
			Nodes[Index]->SetParent(Index + 1 == Nodes.size() ? std::shared_ptr<Instance>(Fixture.World) : Nodes[Index + 1]);
		Fixture.Desired({Nodes.front()->GetObjectId()}); Fixture.Advance(false, true, Case == 1);
		if (Case == 2) EnvelopeRequire(Fixture.Sizes.back() + 32 == 524288, "exact complete-message ceiling includes adapter bytes");
		Fixture.Report(Case == 0 ? "legal-large-7" : Case == 1 ? "legal-gns-incompatible-8" : "legal-gns-ceiling-8");
	}
	{
		AtomicGroupFixture Fixture;
		auto A = std::make_shared<Part>(); A->SetParent(Fixture.World);
		auto B = std::make_shared<Part>(); B->SetParent(Fixture.World);
		std::array<std::shared_ptr<WeldConstraint>, 2> Referrers;
		std::vector<ObjectId> Desired{A->GetObjectId(), B->GetObjectId()};
		for (auto &Referrer : Referrers) {
			Referrer = std::make_shared<WeldConstraint>(); Referrer->SetParent(Fixture.World);
			Referrer->SetPart0(A); Referrer->SetPart1(A); Desired.push_back(Referrer->GetObjectId());
		}
		Fixture.Desired(Desired); Fixture.Advance(false, false);
		std::erase(Desired, A->GetObjectId());
		for (auto &Referrer : Referrers) { Referrer->SetPart0(B); Referrer->SetPart1(B); }
		Fixture.Desired(Desired); Fixture.Advance(); Fixture.Report("ki007-replace-remove");
		const auto OldId = A->GetObjectId(); A->Destroy();
		auto Reload = std::make_shared<Part>(); Reload->SetParent(Fixture.World);
		EnvelopeRequire(Reload->GetObjectId() != OldId, "reloaded identity differs");
		for (auto &Referrer : Referrers) { Referrer->SetPart0(Reload); Referrer->SetPart1(Reload); }
		Desired.push_back(Reload->GetObjectId()); Fixture.Desired(Desired); Fixture.Advance(); Fixture.Report("ki007-reload-restore");
		for (auto &Referrer : Referrers) { Referrer->SetPart0(std::nullopt); Referrer->SetPart1(std::nullopt); }
		std::erase(Desired, Reload->GetObjectId()); Fixture.Desired(Desired); Fixture.Advance(); Fixture.Report("ki007-clear-remove");
	}
	{
		AtomicGroupFixture Fixture;
		std::array<std::shared_ptr<Folder>, 32> Nodes;
		for (auto &Node : Nodes) { Node = std::make_shared<Folder>(); Node->SetName("EvictionNode"); }
		std::ranges::sort(Nodes, {}, [](const auto &Node) { return Node->GetObjectId(); });
		Nodes[0]->SetParent(Fixture.World);
		std::vector<ObjectId> Desired{Nodes[0]->GetObjectId()};
		for (std::size_t Index = 1; Index != Nodes.size(); ++Index) { Nodes[Index]->SetParent(Nodes[0]); Desired.push_back(Nodes[Index]->GetObjectId()); }
		Fixture.Desired(Desired); Fixture.Advance(false, false);
		Fixture.Desired({}); Fixture.Advance(); Fixture.Report("subtree-32-evict");
	}
}
}
