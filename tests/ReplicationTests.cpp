#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationTransport.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <span>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (!Condition) {
			std::cerr << "FAIL: " << Message << '\n';
			++Failures;
		}
	}

	std::vector<TransportEvent> Drain(const std::shared_ptr<SimulatedTransport> &Transport) {
		std::array<TransportEvent, 64> Buffer;
		std::vector<TransportEvent> Result;
		for (;;) {
			const auto Count = Transport->PollEvents(Buffer);
			Result.insert(
				Result.end(),
				std::make_move_iterator(Buffer.begin()),
				std::make_move_iterator(Buffer.begin() + static_cast<std::ptrdiff_t>(Count))
			);
			if (Count < Buffer.size()) return Result;
		}
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events)
			if (const auto *State = std::get_if<ConnectionStateEvent>(&Event);
				State && State->Current == ConnectionState::Connected)
				return State->Connection;
		return {};
	}

	bool Deliver(
		const ReplicationFrame &Frame,
		ConnectionId ServerConnection,
		const NetworkLimits &Limits,
		NetworkScheduler &Scheduler,
		const std::shared_ptr<SimulatedNetwork> &Network,
		const std::shared_ptr<SimulatedTransport> &Client,
		ReplicaApplier &Applier
	) {
		auto Queued = QueueReplicationFrame(Frame, ServerConnection, Limits, Scheduler);
		if (!Queued || !Queued->Accepted()) return false;
		auto Flushed = Scheduler.Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
		if (Flushed.Status != SchedulerFlushStatus::Drained) return false;
		(void)Network->Advance(10ms);
		Network->Pump();
		for (const auto &Event : Drain(Client))
			if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event);
				Message && Message->Traffic == TrafficClass::StructuralReplication) {
				auto Applied = Applier.ApplyBytes(Message->Payload);
				if (!Applied.Succeeded()) std::cerr << "replication delivery rejected: " << Applied.Message << '\n';
				return Applied.Succeeded();
			}
		return false;
	}

	const PreparedPublishReplication *FindPreparedPublish(const ReplicationFrame &Frame, ObjectId Object) {
		for (const auto &Operation : Frame.Operations)
			if (const auto *Publish = std::get_if<PreparedPublishReplication>(&Operation.Intent);
				Publish && Publish->Object == Object)
				return Publish;
		return nullptr;
	}

	void TestRevisionedStructuralMaterialization() {
		auto World = std::make_shared<DataModel>();
		auto Shared = std::make_shared<Folder>();
		Shared->SetName("SharedBeforeRevision");
		Shared->SetParent(World);
		auto Hidden = std::make_shared<Part>();
		Hidden->SetName("PeerSpecificReferenceTarget");
		Hidden->SetParent(World);
		auto Referrer = std::make_shared<WeldConstraint>();
		Referrer->SetName("PeerSpecificReferrer");
		Referrer->SetPart0(Hidden);
		Referrer->SetParent(World);

		ReplicationCoordinator Coordinator(World);
		auto First = Coordinator.AddPeer({101, 1}, ReplicationEpoch(1));
		auto Second = Coordinator.AddPeer({102, 1}, ReplicationEpoch(1));
		Check(First.Succeeded() && Second.Succeeded(), "shared structural baselines are prepared for two peers");
		if (!First.Succeeded() || !Second.Succeeded()) return;
		auto FirstBytes = EncodeReplicationFrame(*First.Frame);
		auto SecondBytes = EncodeReplicationFrame(*Second.Frame);
		Check(
			FirstBytes && SecondBytes && *FirstBytes == *SecondBytes,
			"shared structural templates preserve byte-identical baseline encoding"
		);
		for (const auto &Operation : First.Frame->Operations) {
			const auto *FirstPublish = std::get_if<PreparedPublishReplication>(&Operation.Intent);
			const auto *SecondPublish = FirstPublish ? FindPreparedPublish(*Second.Frame, FirstPublish->Object)
													 : nullptr;
			Check(
				FirstPublish && SecondPublish && FirstPublish->Template == SecondPublish->Template,
				"unchanged peers share the same immutable structural revision"
			);
		}
		const auto SharedBefore = FindPreparedPublish(*First.Frame, Shared->GetObjectId());
		const auto MetricsAfterReuse = Coordinator.GetMetrics();
		Check(
			MetricsAfterReuse.StructuralTemplateBuilds == First.Frame->Operations.size() &&
				MetricsAfterReuse.StructuralTemplateMisses == First.Frame->Operations.size() &&
				MetricsAfterReuse.StructuralTemplateHits == Second.Frame->Operations.size() &&
				MetricsAfterReuse.StructuralBytesReused > 0,
			"template build, miss, hit, and reused-byte metrics distinguish shared work"
		);

		Shared->SetName("SharedAfterRevision");
		auto Incremental = Coordinator.ProduceIncremental({101, 1});
		auto Third = Coordinator.AddPeer({103, 1}, ReplicationEpoch(1));
		const auto SharedAfter = Third.Succeeded() ? FindPreparedPublish(*Third.Frame, Shared->GetObjectId()) : nullptr;
		Check(
			Incremental.Succeeded() && Third.Succeeded() && SharedBefore && SharedAfter &&
				SharedBefore->Template != SharedAfter->Template &&
				SharedBefore->Template->Publication.Name == "SharedBeforeRevision" &&
				SharedAfter->Template->Publication.Name == "SharedAfterRevision" &&
				SharedBefore->Template->Key.StructuralRevision < SharedAfter->Template->Key.StructuralRevision,
			"a structural mutation atomically replaces the revision while prepared old work remains valid"
		);
		const auto InvalidationsBeforeNonReplicated = Coordinator.GetMetrics().StructuralTemplateInvalidations;
		const auto RevisedTemplate = SharedAfter ? SharedAfter->Template : nullptr;
		Shared->SetArchivable(false);
		auto Fourth = Coordinator.AddPeer({104, 1}, ReplicationEpoch(1));
		const auto SharedAfterNonReplicated = Fourth.Succeeded()
												  ? FindPreparedPublish(*Fourth.Frame, Shared->GetObjectId())
												  : nullptr;
		Check(
			Fourth.Succeeded() && SharedAfterNonReplicated && SharedAfterNonReplicated->Template == RevisedTemplate &&
				Coordinator.GetMetrics().StructuralTemplateInvalidations == InvalidationsBeforeNonReplicated,
			"a non-replicated property mutation does not invalidate a structural template"
		);

		PeerRelevanceSelection HiddenSelection{
			.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Referrer->GetObjectId()},
		};
		PeerRelevanceSelection VisibleSelection{
			.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), Hidden->GetObjectId(), Referrer->GetObjectId()},
		};
		auto HiddenPeer = Coordinator.AddPeer({105, 1}, ReplicationEpoch(1), HiddenSelection);
		auto VisiblePeer = Coordinator.AddPeer({106, 1}, ReplicationEpoch(1), VisibleSelection);
		auto HiddenDecoded = HiddenPeer.Succeeded()
								 ? DecodeReplicationFrame(*EncodeReplicationFrame(*HiddenPeer.Frame))
								 : SerializationResult<ReplicationFrame>(SerializationFailure(
									   SerializationErrorCode::InternalFailure, "missing hidden frame"
								   ));
		auto VisibleDecoded = VisiblePeer.Succeeded()
								  ? DecodeReplicationFrame(*EncodeReplicationFrame(*VisiblePeer.Frame))
								  : SerializationResult<ReplicationFrame>(SerializationFailure(
										SerializationErrorCode::InternalFailure, "missing visible frame"
									));
		auto FindDecodedValue = [&](const SerializationResult<ReplicationFrame> &Frame) -> const WireValue * {
			if (!Frame) return nullptr;
			for (const auto &Operation : Frame->Operations)
				if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent);
					Publish && Publish->Object == Referrer->GetObjectId()) {
					auto Value = Publish->Properties.find("Part0");
					return Value == Publish->Properties.end() ? nullptr : &Value->second;
				}
			return nullptr;
		};
		const auto *HiddenValue = FindDecodedValue(HiddenDecoded);
		const auto *VisibleValue = FindDecodedValue(VisibleDecoded);
		Check(
			HiddenPeer.Succeeded() && VisiblePeer.Succeeded() && HiddenValue && VisibleValue &&
				std::holds_alternative<std::monostate>(*HiddenValue) &&
				std::holds_alternative<WireObjectReference>(*VisibleValue) &&
				Coordinator.GetMetrics().ReferencePatchOperations > 0,
			"peer-specific reference visibility patches a shared template without leaking hidden identity"
		);

		ReplicationCoordinator UncachedCoordinator(World, {}, false);
		auto UncachedPeer = UncachedCoordinator.AddPeer({107, 1}, ReplicationEpoch(1), HiddenSelection);
		auto PreparedBytes = HiddenPeer.Succeeded()
								 ? EncodeReplicationFrame(*HiddenPeer.Frame)
								 : SerializationResult<std::vector<std::byte>>(SerializationFailure(
									   SerializationErrorCode::InternalFailure, "missing prepared frame"
								   ));
		auto UncachedBytes = UncachedPeer.Succeeded()
								 ? EncodeReplicationFrame(*UncachedPeer.Frame)
								 : SerializationResult<std::vector<std::byte>>(SerializationFailure(
									   SerializationErrorCode::InternalFailure, "missing uncached frame"
								   ));
		Check(
			UncachedPeer.Succeeded() && PreparedBytes && UncachedBytes && *PreparedBytes == *UncachedBytes &&
				std::ranges::none_of(
					UncachedPeer.Frame->Operations,
					[](const auto &Operation) {
						return std::holds_alternative<PreparedPublishReplication>(Operation.Intent);
					}
				) &&
				UncachedCoordinator.GetMetrics().StructuralTemplateHits == 0,
			"prepared and explicitly uncached structural publication paths are byte-equivalent"
		);

		const auto InvalidationsBeforeStructuralMutations = Coordinator.GetMetrics().StructuralTemplateInvalidations;
		Shared->SetParent(Hidden);
		Check(
			Shared->ApplyAttributeMutation("Revisioned", WireValue(7), ScriptSecurityContext::CoreTrusted()) ==
				MutationStatus::Success,
			"revisioned structural Attribute mutation is accepted"
		);
		(void)World->Tags.Add(
			World->GetObjectId(), Shared->GetObjectId(), "Revisioned", ScriptSecurityContext::CoreTrusted()
		);
		Referrer->SetPart0(std::nullopt);
		auto MutationPeer = Coordinator.AddPeer({108, 1}, ReplicationEpoch(1));
		const auto RevisedShared = MutationPeer.Succeeded()
									   ? FindPreparedPublish(*MutationPeer.Frame, Shared->GetObjectId())
									   : nullptr;
		const auto RevisedReferrer = MutationPeer.Succeeded()
										 ? FindPreparedPublish(*MutationPeer.Frame, Referrer->GetObjectId())
										 : nullptr;
		Check(
			MutationPeer.Succeeded() && RevisedShared && RevisedReferrer &&
				RevisedShared->Template->Publication.Parent == std::optional(Hidden->GetObjectId()) &&
				RevisedShared->Template->Publication.Attributes.contains("Revisioned") &&
				std::ranges::find(RevisedShared->Template->Publication.Tags, "Revisioned") !=
					RevisedShared->Template->Publication.Tags.end() &&
				std::holds_alternative<std::monostate>(RevisedReferrer->Template->Publication.Properties.at("Part0")) &&
				Coordinator.GetMetrics().StructuralTemplateInvalidations == InvalidationsBeforeStructuralMutations + 2,
			"hierarchy, Attribute, Tag, and reference mutations rebuild only affected object revisions"
		);
	}

	void TestStructuralTemplateDifferential() {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Folders;
		for (std::size_t Index = 0; Index < 16; ++Index) {
			auto FolderValue = std::make_shared<Folder>();
			FolderValue->SetName("DifferentialFolder" + std::to_string(Index));
			FolderValue->SetParent(World);
			Folders.push_back(std::move(FolderValue));
		}
		auto MovingLeaf = std::make_shared<Part>();
		MovingLeaf->SetName("DifferentialLeaf");
		MovingLeaf->SetParent(Folders.front());
		auto ReferenceTarget = std::make_shared<Part>();
		ReferenceTarget->SetName("DifferentialReferenceTarget");
		ReferenceTarget->SetParent(World);
		auto Reference = std::make_shared<WeldConstraint>();
		Reference->SetName("DifferentialReference");
		Reference->SetParent(World);
		auto PlayerValue = std::make_shared<Player>();
		PlayerValue->SetName("DifferentialPlayer");
		PlayerValue->SetParent(World);
		auto CharacterValue = std::make_shared<Character>();
		CharacterValue->SetName("DifferentialCharacter");
		CharacterValue->SetParent(World);
		std::shared_ptr<Folder> Ephemeral;
		ReplicationCoordinator PreparedCoordinator(World);
		ReplicationCoordinator UncachedCoordinator(World, {}, false);
		std::uint64_t State = 0x9e3779b97f4a7c15ULL;
		bool Tagged = false;
		for (std::uint32_t Iteration = 0; Iteration < 64; ++Iteration) {
			State = State * 6364136223846793005ULL + 1442695040888963407ULL;
			const auto Index = static_cast<std::size_t>((State >> 32) % Folders.size());
			switch (Iteration % 8) {
			case 0:
				Folders[Index]->SetName("DifferentialRevision" + std::to_string(State));
				break;
			case 1:
				(void)Folders[Index]->ApplyAttributeMutation(
					"Revision", WireValue(static_cast<int>(Iteration)), ScriptSecurityContext::CoreTrusted()
				);
				break;
			case 2:
				if (Tagged)
					(void)World->Tags.Remove(
						World->GetObjectId(),
						MovingLeaf->GetObjectId(),
						"Differential",
						ScriptSecurityContext::CoreTrusted()
					);
				else
					(void)World->Tags.Add(
						World->GetObjectId(),
						MovingLeaf->GetObjectId(),
						"Differential",
						ScriptSecurityContext::CoreTrusted()
					);
				Tagged = !Tagged;
				break;
			case 3:
				MovingLeaf->SetParent(Folders[Index]);
				break;
			case 4:
				if (Ephemeral) {
					Ephemeral->Destroy();
					Ephemeral.reset();
				} else {
					Ephemeral = std::make_shared<Folder>();
					Ephemeral->SetName("DifferentialEphemeral" + std::to_string(State));
					Ephemeral->SetParent(Folders[Index]);
				}
				break;
			case 5:
				Reference->SetPart0((State & 1) != 0 ? std::optional(ReferenceTarget) : std::nullopt);
				break;
			case 6:
				PlayerValue->SetCharacter((State & 1) != 0 ? std::optional(CharacterValue) : std::nullopt);
				break;
			default:
				ReferenceTarget->SetName("DifferentialReferenceRevision" + std::to_string(State));
				break;
			}
			PeerRelevanceSelection Selection{
				.RequiredObjects = {World->GetObjectId()},
				.DesiredObjects = {
					World->GetObjectId(),
					MovingLeaf->GetObjectId(),
					Reference->GetObjectId(),
					PlayerValue->GetObjectId()
				},
			};
			if ((State & 2) != 0) Selection.DesiredObjects.push_back(ReferenceTarget->GetObjectId());
			if ((State & 4) != 0) Selection.DesiredObjects.push_back(CharacterValue->GetObjectId());
			if (Ephemeral) Selection.DesiredObjects.push_back(Ephemeral->GetObjectId());
			for (std::size_t Offset = 0; Offset < 6; ++Offset)
				Selection.DesiredObjects.push_back(Folders[(Index + Offset * 3) % Folders.size()]->GetObjectId());
			std::ranges::sort(Selection.DesiredObjects);
			const ConnectionId Connection{static_cast<std::uint32_t>(200 + Iteration), 1};
			auto Prepared = PreparedCoordinator.AddPeer(Connection, ReplicationEpoch(1), Selection);
			auto Uncached = UncachedCoordinator.AddPeer(Connection, ReplicationEpoch(1), Selection);
			auto PreparedBytes = Prepared.Succeeded()
									 ? EncodeReplicationFrame(*Prepared.Frame)
									 : SerializationResult<std::vector<std::byte>>(SerializationFailure(
										   SerializationErrorCode::InternalFailure, "missing prepared differential"
									   ));
			auto UncachedBytes = Uncached.Succeeded()
									 ? EncodeReplicationFrame(*Uncached.Frame)
									 : SerializationResult<std::vector<std::byte>>(SerializationFailure(
										   SerializationErrorCode::InternalFailure, "missing uncached differential"
									   ));
			Check(
				Prepared.Succeeded() && Uncached.Succeeded() && PreparedBytes && UncachedBytes &&
					*PreparedBytes == *UncachedBytes,
				"deterministic structural mutation and relevance differential matches the uncached path"
			);
		}
		Check(
			PreparedCoordinator.GetMetrics().StructuralTemplateInvalidations > 0 &&
				PreparedCoordinator.GetMetrics().StructuralTemplateHits > 0,
			"differential workload exercises both template invalidation and reuse"
		);
	}

	void TestMixedSimulatorComposition() {
		auto World = std::make_shared<DataModel>();
		auto Event = std::make_shared<RemoteEvent>();
		auto Function = std::make_shared<RemoteFunction>();
		Event->SetParent(World);
		Function->SetParent(World);
		const auto Limits = NetworkLimits::NativeCeilings();
		SimulatedTransportConfiguration Configuration;
		Configuration.BaseLatency = 2ms;
		Configuration.MaximumJitter = 3ms;
		Configuration.MaximumReorderDelay = 5ms;
		Configuration.UnreliableReorderProbability = 1.0;
		auto Network = SimulatedNetwork::Create(Configuration);
		auto ServerTransport = Network->CreateTransport();
		auto ClientTransport = Network->CreateTransport();
		Check(
			ServerTransport->Start({TransportRole::Server, {"mixed-composition", 1}, Limits, {}}).Succeeded() &&
				ClientTransport->Start({TransportRole::Client, {"mixed-composition", 1}, Limits, {}}).Succeeded(),
			"mixed simulator endpoints start"
		);
		Network->Pump();
		const auto ServerConnection = ConnectedId(Drain(ServerTransport));
		const auto ClientConnection = ConnectedId(Drain(ClientTransport));
		NetworkScheduler ServerScheduler(*ServerTransport);
		NetworkScheduler ClientScheduler(*ClientTransport);
		Check(
			ServerScheduler.RegisterConnection(ServerConnection, Limits) &&
				ClientScheduler.RegisterConnection(ClientConnection, Limits),
			"mixed simulator schedulers share one connection session"
		);
		ReplicationCoordinator Coordinator(World);
		auto Baseline = Coordinator.AddPeer(ServerConnection, ReplicationEpoch(1));
		ReplicaApplier ClientReplica;
		Check(
			Baseline.Succeeded() &&
				Deliver(
					*Baseline.Frame, ServerConnection, Limits, ServerScheduler, Network, ClientTransport, ClientReplica
				),
			"mixed simulator establishes the replication baseline"
		);
		auto ServerVisibility = [&](ConnectionId Connection, ObjectId Object) {
			const auto *View = Coordinator.GetView(Connection);
			return View && View->Knows(Object);
		};
		auto ClientVisibility = [&](ConnectionId, ObjectId Object) { return ClientReplica.Resolve(Object) != nullptr; };
		RemoteManager ServerRemotes(RemoteManagerRole::Server, ServerScheduler, ServerVisibility, [](ObjectId Object) {
			return ObjectRegistry::Get().Lookup(Object);
		});
		RemoteManager ClientRemotes(RemoteManagerRole::Client, ClientScheduler, ClientVisibility, [&](ObjectId Object) {
			return ClientReplica.Resolve(Object);
		});
		Check(
			ServerRemotes.AddPeer(ServerConnection, ReplicationEpoch(1), Limits) &&
				ClientRemotes.AddPeer(ClientConnection, ReplicationEpoch(1), Limits),
			"mixed simulator RemoteManagers bind the baseline epoch"
		);
		for (const auto [Object, Kind] : {
				 std::pair{Event->GetObjectId(), RemoteInstanceKind::ReliableEvent},
				 std::pair{Function->GetObjectId(), RemoteInstanceKind::Function},
			 }) {
			Check(
				ServerRemotes.RegisterRemote(Object, Kind) && ClientRemotes.RegisterRemote(Object, Kind) &&
					ServerRemotes.PublishRemote(ServerConnection, Object) &&
					ClientRemotes.PublishRemote(ClientConnection, Object),
				"mixed simulator publishes replicated Remote identity"
			);
		}

		auto NewObject = std::make_shared<Folder>();
		NewObject->SetParent(World);
		auto Publication = Coordinator.ProduceIncremental(ServerConnection);
		Check(Publication.Succeeded(), "mixed simulator produces a new Object publication");
		if (Publication.Succeeded()) {
			auto Queued = QueueReplicationFrame(*Publication.Frame, ServerConnection, Limits, ServerScheduler);
			Check(Queued && Queued->Accepted(), "mixed simulator queues publication on shared scheduler");
		}
		bool EventResolved = false;
		bool RequestHandled = false;
		std::optional<RemoteRequestTerminalStatus> RequestStatus;
		ClientRemotes.SetEventHandler(Event->GetObjectId(), [&](const RemoteInvocation &Invocation) {
			const auto *Reference = std::get_if<WireObjectReference>(&Invocation.Arguments.front());
			EventResolved = Reference && ClientReplica.Resolve(Reference->Object.ToObjectId()) != nullptr;
		});
		ServerRemotes.SetRequestHandler(
			Function->GetObjectId(), [&](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
				RequestHandled = Reply({42}, std::nullopt);
			}
		);
		Check(
			ServerRemotes
					.SendEvent(
						ServerConnection,
						Event->GetObjectId(),
						{WireObjectReference{WireObjectId::FromObjectId(NewObject->GetObjectId())}}
					)
					.Status == RemoteSendStatus::DeferredForMaterialization,
			"mixed simulator reliable event waits on explicit publication"
		);
		Check(
			ClientRemotes
				.StartRequest(
					ClientConnection,
					Function->GetObjectId(),
					{},
					[&](RemoteRequestResult Result) { RequestStatus = Result.Outcome.Status; },
					1s
				)
				.Accepted(),
			"mixed simulator request starts beside replication traffic"
		);
		bool PublicationApplied = false;
		for (int Tick = 0; Tick < 20 && (!EventResolved || !RequestStatus); ++Tick) {
			(void)ServerScheduler.Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			(void)ClientScheduler.Flush(ClientConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			(void)Network->Advance(10ms);
			Network->Pump();
			for (const auto &TransportEventValue : Drain(ServerTransport))
				(void)ServerRemotes.HandleTransportEvent(TransportEventValue);
			for (const auto &TransportEventValue : Drain(ClientTransport)) {
				if (const auto *Received = std::get_if<ReceivedMessageEvent>(&TransportEventValue);
					Received && Received->Traffic == TrafficClass::StructuralReplication) {
					PublicationApplied = ClientReplica.ApplyBytes(Received->Payload).Succeeded();
					if (PublicationApplied) {
						(void)ServerRemotes.MarkMaterialized(ServerConnection, NewObject->GetObjectId());
						(void)ClientRemotes.MarkMaterialized(ClientConnection, NewObject->GetObjectId());
					}
				} else {
					(void)ClientRemotes.HandleTransportEvent(TransportEventValue);
				}
			}
			ServerRemotes.Pump();
			ClientRemotes.Pump();
		}
		Check(
			PublicationApplied && EventResolved && RequestHandled &&
				RequestStatus == RemoteRequestTerminalStatus::Success,
			"mixed simulator composes replication, dependency-gated RemoteEvent, and request/response"
		);
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	try {
		BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Error) {
		std::cerr << Error.what() << '\n';
		return 1;
	}
	TestMixedSimulatorComposition();
	TestRevisionedStructuralMaterialization();
	TestStructuralTemplateDifferential();

	auto Game = std::make_shared<DataModel>();
	Game->SetName("ServerWorld");
	auto FolderValue = std::make_shared<Folder>();
	FolderValue->SetName("ReplicatedFolder");
	FolderValue->SetParent(Game);
	auto FirstPart = std::make_shared<Part>();
	FirstPart->SetName("FirstPart");
	FirstPart->SetParent(FolderValue);
	auto SecondPart = std::make_shared<Part>();
	SecondPart->SetName("SecondPart");
	SecondPart->SetParent(FolderValue);
	auto Weld = std::make_shared<WeldConstraint>();
	Weld->SetName("ReferenceWeld");
	Weld->SetPart0(FirstPart);
	Weld->SetPart1(SecondPart);
	Weld->SetParent(FolderValue);
	Check(
		FirstPart->ApplyAttributeMutation("Health", WireValue(100), ScriptSecurityContext::CoreTrusted()) ==
			MutationStatus::Success,
		"authoritative Attribute setup succeeds"
	);
	(void)Game->Tags.Add(
		Game->GetObjectId(), FirstPart->GetObjectId(), "Replicated", ScriptSecurityContext::CoreTrusted()
	);
	ChangeJournal::Get().Clear();

	SimulatedTransportConfiguration Configuration;
	Configuration.BaseLatency = 1ms;
	Configuration.MaximumJitter = 2ms;
	Configuration.MaximumReorderDelay = 5ms;
	Configuration.UnreliableDuplicationProbability = 1.0;
	Configuration.UnreliableReorderProbability = 1.0;
	Configuration.BandwidthBytesPerSecond = 1024 * 1024;
	auto Network = SimulatedNetwork::Create(Configuration);
	auto Server = Network->CreateTransport();
	auto Client = Network->CreateTransport();
	const auto Limits = NetworkLimits::NativeCeilings();
	Check(
		Server->Start({TransportRole::Server, {"replication-test", 1}, Limits, {}}).Succeeded() &&
			Client->Start({TransportRole::Client, {"replication-test", 1}, Limits, {}}).Succeeded(),
		"simulated server/client endpoints start"
	);
	Network->Pump();
	auto ServerConnection = ConnectedId(Drain(Server));
	auto ClientConnection = ConnectedId(Drain(Client));
	Check(ServerConnection.IsValid() && ClientConnection.IsValid(), "simulated replication connection becomes active");
	NetworkScheduler Scheduler(*Server);
	Check(
		Scheduler.RegisterConnection(ServerConnection, Limits), "production scheduler registers replication connection"
	);
	ReplicationCoordinator Coordinator(Game);
	auto Baseline = Coordinator.AddPeer(ServerConnection, ReplicationEpoch(1));
	Check(
		Baseline.Succeeded() && Baseline.Frame->Kind == ReplicationMessageKind::Baseline,
		"coordinator produces a bounded schema-carrying baseline"
	);
	auto BaselineBytes = Baseline.Succeeded()
							 ? EncodeReplicationFrame(*Baseline.Frame)
							 : SerializationResult<std::vector<std::byte>>(
								   SerializationFailure(SerializationErrorCode::InternalFailure, "missing baseline")
							   );
	Check(BaselineBytes && DecodeReplicationFrame(*BaselineBytes).has_value(), "binary baseline codec round-trips");
	if (Baseline.Succeeded()) {
		auto DuplicateSchema = *Baseline.Frame;
		DuplicateSchema.Schema.push_back(DuplicateSchema.Schema.front());
		Check(!EncodeReplicationFrame(DuplicateSchema), "duplicate schema identities fail before encoding");
	}
	std::vector<std::byte> OversizedFrame(MaximumReplicationFrameBytes + 1);
	Check(!DecodeReplicationFrame(OversizedFrame), "oversized replication frames fail before structural parsing");
	if (BaselineBytes) {
		auto Truncated = std::span<const std::byte>(*BaselineBytes).first(BaselineBytes->size() - 1);
		Check(!DecodeReplicationFrame(Truncated), "truncated replication frames fail closed");
		auto ForgedLength = *BaselineBytes;
		ForgedLength[32] = static_cast<std::byte>(0xff);
		Check(!DecodeReplicationFrame(ForgedLength), "forged replication payload lengths fail closed");
		auto CountBomb = std::vector<std::byte>(BaselineBytes->begin(), BaselineBytes->begin() + 36);
		for (std::size_t Index = 24; Index < 36; ++Index)
			CountBomb[Index] = std::byte{0};
		CountBomb[30] = std::byte{1};
		Check(
			!DecodeReplicationFrame(CountBomb),
			"payload-infeasible operation counts fail before attacker-directed reserve allocation"
		);
	}
	ReplicationFrame AggregateOversized{
		ReplicationProtocolVersion,
		ReplicationMessageKind::Incremental,
		ReplicationEpoch(1),
		ReliableReplicationSequence(2)
	};
	for (std::size_t Index = 0; Index < 132; ++Index)
		AggregateOversized.Operations.push_back(
			{ReplicationEpoch(1),
			 PropertyReplicationUpdate{
				 FolderValue->GetObjectId(), "Name", std::string(MaximumProtocolStringBytes, 'x')
			 }}
		);
	Check(
		!EncodeReplicationFrame(AggregateOversized),
		"aggregate frame limits stop encoding before an oversized payload is materialized"
	);
	ReplicaApplier ClientReplica;
	(void)Network->Advance(10ms);
	Check(
		Baseline.Succeeded() &&
			Deliver(*Baseline.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica),
		"baseline traverses production scheduler and deterministic transport into the replica applicator"
	);
	auto ReplicaFolder = ClientReplica.GetReplicaRoot()->FindFirstChild("ReplicatedFolder", false);
	auto ReplicaPart = ReplicaFolder ? ReplicaFolder->FindFirstChild("FirstPart", false) : nullptr;
	auto StableReplicaPart = ClientReplica.Resolve(FirstPart->GetObjectId());
	auto StableSecondReplicaPart = ClientReplica.Resolve(SecondPart->GetObjectId());
	Check(
		ReplicaFolder && ReplicaPart && ReplicaPart->GetAttributeValue("Health") == WireValue(100),
		"baseline materializes hierarchy, properties, and Attributes"
	);
	auto ReplicaGame = std::dynamic_pointer_cast<DataModel>(ClientReplica.GetReplicaRoot());
	Check(
		ReplicaPart && ReplicaGame &&
			ReplicaGame->Tags.GetTags(
				ReplicaGame->GetObjectId(), ReplicaPart->GetObjectId(), ScriptSecurityContext::CoreTrusted()
			) == std::vector<std::string>{"Replicated"},
		"baseline materializes replica Tag indexes"
	);
	auto BeforeRejectedRoot = ClientReplica.GetReplicaRoot();
	ReplicationFrame RejectedBatch{
		ReplicationProtocolVersion,
		ReplicationMessageKind::Incremental,
		ReplicationEpoch(1),
		ReliableReplicationSequence(2),
		{},
		{
			{ReplicationEpoch(1),
			 PropertyReplicationUpdate{FolderValue->GetObjectId(), "Name", std::string("PartialMutation")}},
			{ReplicationEpoch(1), ReparentReplication{FirstPart->GetObjectId(), ObjectId{999999, 1}}},
		}
	};
	Check(
		ClientReplica.ApplyFrame(RejectedBatch).Status == ReplicaApplyStatus::SemanticRejection &&
			ClientReplica.GetReplicaRoot() == BeforeRejectedRoot &&
			ClientReplica.GetReplicaRoot()->FindFirstChild("ReplicatedFolder", false),
		"a rejected grouped frame cannot commit a valid prefix or partially replace the live replica"
	);

	FolderValue->SetName("RenamedFolder");
	FirstPart->ApplyAttributeMutation("Health", WireValue(75), ScriptSecurityContext::CoreTrusted());
	(void)Game->Tags.Remove(
		Game->GetObjectId(), FirstPart->GetObjectId(), "Replicated", ScriptSecurityContext::CoreTrusted()
	);
	SecondPart->SetParent(Game);
	auto Incremental = Coordinator.ProduceIncremental(ServerConnection);
	(void)Network->Advance(10ms);
	Check(
		Incremental.Succeeded() &&
			Deliver(*Incremental.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica),
		"reliable property, Attribute, Tag, and reparent operations apply as one coherent frame"
	);
	ReplicaFolder = ClientReplica.GetReplicaRoot()->FindFirstChild("RenamedFolder", false);
	ReplicaPart = ReplicaFolder ? ReplicaFolder->FindFirstChild("FirstPart", false) : nullptr;
	Check(
		ReplicaFolder && ReplicaPart && ReplicaPart->GetAttributeValue("Health") == WireValue(75) &&
			ClientReplica.GetReplicaRoot()->FindFirstChild("SecondPart", false) &&
			ClientReplica.Resolve(FirstPart->GetObjectId()) == StableReplicaPart &&
			ClientReplica.Resolve(SecondPart->GetObjectId()) == StableSecondReplicaPart,
		"incremental replica state matches authoritative mutations"
	);

	auto Unpublish = Coordinator.SetRelevant(ServerConnection, FirstPart->GetObjectId(), false);
	(void)Network->Advance(10ms);
	Check(
		Unpublish.Succeeded() &&
			Deliver(*Unpublish.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica) &&
			!ClientReplica.Resolve(FirstPart->GetObjectId()),
		"Unpublish removes only peer knowledge while the server object remains alive"
	);
	Check(!FirstPart->GetDestroyed(), "Unpublish is not authoritative destruction");
	auto HiddenDependentPublish = Coordinator.SetRelevant(ServerConnection, Weld->GetObjectId(), true);
	Check(
		!HiddenDependentPublish.Succeeded() &&
			HiddenDependentPublish.Error.find("references an object not materialized") != std::string::npos,
		"republishing cannot expose a reference to a peer-hidden object"
	);
	auto HiddenReferenceWeld = std::make_shared<WeldConstraint>();
	HiddenReferenceWeld->SetName("HiddenReferenceWeld");
	HiddenReferenceWeld->SetPart0(FirstPart);
	HiddenReferenceWeld->SetPart1(SecondPart);
	HiddenReferenceWeld->SetParent(FolderValue);
	auto HiddenCreation = Coordinator.ProduceIncremental(ServerConnection);
	Check(
		!HiddenCreation.Succeeded() &&
			HiddenCreation.Error.find("references an object not materialized") != std::string::npos,
		"new-object publication cannot bypass peer reference visibility"
	);
	auto Republish = Coordinator.SetRelevant(ServerConnection, FirstPart->GetObjectId(), true);
	(void)Network->Advance(10ms);
	Check(
		Republish.Succeeded() &&
			Deliver(*Republish.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica) &&
			ClientReplica.Resolve(FirstPart->GetObjectId()),
		"a live authoritative identity can be republished"
	);
	auto VisibleCreation = Coordinator.ProduceIncremental(ServerConnection);
	(void)Network->Advance(10ms);
	Check(
		VisibleCreation.Succeeded() &&
			Deliver(*VisibleCreation.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica) &&
			ClientReplica.Resolve(HiddenReferenceWeld->GetObjectId()),
		"a deferred new object publishes after all of its references become visible"
	);

	const auto StaleFrame = *Incremental.Frame;
	Weld->SetPart0(std::nullopt);
	HiddenReferenceWeld->SetPart0(std::nullopt);
	FirstPart->Destroy();
	auto DestroyFrame = Coordinator.ProduceIncremental(ServerConnection);
	(void)Network->Advance(10ms);
	Check(
		DestroyFrame.Succeeded() &&
			Deliver(*DestroyFrame.Frame, ServerConnection, Limits, Scheduler, Network, Client, ClientReplica) &&
			!ClientReplica.Resolve(FirstPart->GetObjectId()),
		"authoritative destruction removes the known replica"
	);
	Check(
		ClientReplica.ApplyFrame(StaleFrame).Status == ReplicaApplyStatus::StaleSequence,
		"stale reliable operations cannot mutate a newer replica"
	);

	ReplicationCoordinator ReconnectCoordinator(Game);
	auto Rebaseline = ReconnectCoordinator.AddPeer({ServerConnection.Slot + 1, 1}, ReplicationEpoch(2));
	Check(
		Rebaseline.Succeeded() && ClientReplica.ApplyFrame(*Rebaseline.Frame).Succeeded() &&
			ClientReplica.GetEpoch() == ReplicationEpoch(2),
		"reconnect establishes a clean new replication epoch"
	);
	Check(
		ClientReplica.ApplyFrame(*DestroyFrame.Frame).Status == ReplicaApplyStatus::StaleEpoch,
		"operations from an old epoch cannot mutate the rebaselined replica"
	);
	Check(
		ClientReplica.ApplyFrame(*Baseline.Frame).Status == ReplicaApplyStatus::StaleEpoch,
		"a replayed old baseline cannot replace a newer replica epoch"
	);

	if (Rebaseline.Succeeded()) {
		auto Mismatch = *Rebaseline.Frame;
		++Mismatch.Schema.front().DefinitionVersion;
		ReplicaApplier RejectedReplica;
		Check(
			RejectedReplica.ApplyFrame(Mismatch).Status == ReplicaApplyStatus::SchemaMismatch &&
				!RejectedReplica.GetReplicaRoot(),
			"schema mismatch fails closed before partial baseline publication"
		);
	}

	ReplicationFrame ClientForged{
		ReplicationProtocolVersion,
		ReplicationMessageKind::Incremental,
		ReplicationEpoch(99),
		ReliableReplicationSequence(1),
		{},
		{{ReplicationEpoch(99), DestroyReplication{Game->GetObjectId()}}}
	};
	auto ForgedBytes = EncodeReplicationFrame(ClientForged);
	Check(
		ForgedBytes && Game->GetName() == "ServerWorld" && !Game->GetDestroyed(),
		"client-creatable protocol bytes have no path into authoritative server mutation"
	);

	if (BaselineBytes) {
		NetworkLimits PressureLimits{
			.MaximumReliableMessageBytes = BaselineBytes->size(),
			.MaximumUnreliableMessageBytes = 1,
			.MaximumQueuedReliableBytes = BaselineBytes->size(),
			.MaximumInFlightRemoteRequests = 1,
			.MaximumDecodedMessageBytes = BaselineBytes->size(),
			.MaximumSendBytesPerTick = BaselineBytes->size(),
			.MaximumReceiveBytesPerTick = BaselineBytes->size(),
			.MaximumMessagesPerTick = 2,
		};
		NetworkScheduler PressureScheduler(*Server);
		PressureScheduler.RegisterConnection(ServerConnection, PressureLimits);
		auto FirstQueued = QueueReplicationFrame(*Baseline.Frame, ServerConnection, PressureLimits, PressureScheduler);
		auto OverflowQueued = QueueReplicationFrame(
			*Baseline.Frame, ServerConnection, PressureLimits, PressureScheduler
		);
		Check(
			FirstQueued && FirstQueued->Accepted() && OverflowQueued &&
				OverflowQueued->Status == SchedulerSubmitStatus::ReliableBacklogExhausted &&
				OverflowQueued->IsTerminal(),
			"reliable replication queue pressure terminates structurally instead of growing without bound"
		);
	}

	{
		SimulatedTransportConfiguration InterruptedConfiguration;
		InterruptedConfiguration.BaseLatency = 100ms;
		InterruptedConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto InterruptedNetwork = SimulatedNetwork::Create(InterruptedConfiguration);
		auto InterruptedServer = InterruptedNetwork->CreateTransport();
		auto InterruptedClient = InterruptedNetwork->CreateTransport();
		InterruptedServer->Start({TransportRole::Server, {"interrupted-baseline", 2}, Limits, {}});
		InterruptedClient->Start({TransportRole::Client, {"interrupted-baseline", 2}, Limits, {}});
		InterruptedNetwork->Pump();
		auto InterruptedServerConnection = ConnectedId(Drain(InterruptedServer));
		(void)Drain(InterruptedClient);
		NetworkScheduler InterruptedScheduler(*InterruptedServer);
		InterruptedScheduler.RegisterConnection(InterruptedServerConnection, Limits);
		auto InterruptedQueued = QueueReplicationFrame(
			*Baseline.Frame, InterruptedServerConnection, Limits, InterruptedScheduler
		);
		InterruptedScheduler.Flush(InterruptedServerConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
		InterruptedServer->Disconnect(
			InterruptedServerConnection, {DisconnectReason::LocalShutdown, "Interrupt replication baseline"}
		);
		(void)InterruptedNetwork->Advance(200ms);
		InterruptedNetwork->Pump();
		bool ReceivedBaseline = false;
		for (const auto &Event : Drain(InterruptedClient))
			ReceivedBaseline = ReceivedBaseline || std::holds_alternative<ReceivedMessageEvent>(Event);
		Check(
			InterruptedQueued && InterruptedQueued->Accepted() && !ReceivedBaseline,
			"disconnecting during an in-flight baseline never exposes a partial client replica frame"
		);
	}

	auto SchedulerStats = Scheduler.GetStatistics(ServerConnection);
	Check(
		SchedulerStats && SchedulerStats->MessagesSubmittedToTransport >= 5 &&
			Coordinator.GetMetrics().BaselineObjects >= 5 && Coordinator.GetMetrics().ObjectsUnpublished >= 1,
		"replication, scheduler, and transport accounting remain independently observable"
	);

	if (Failures == 0) std::cout << "All basic client replication tests passed\n";
	return Failures == 0 ? 0 : 1;
}
