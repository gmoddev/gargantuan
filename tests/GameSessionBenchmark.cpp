#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
	std::atomic<std::uint64_t> GameSessionBenchmarkAllocations = 0;
}

void *operator new(std::size_t Size) {
	GameSessionBenchmarkAllocations.fetch_add(1, std::memory_order_relaxed);
	if (auto *Value = std::malloc(Size)) return Value;
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept {
	std::free(Value);
}

void operator delete(void *Value, std::size_t) noexcept {
	std::free(Value);
}

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	GameSessionConfiguration Configuration(GameSessionRole Role, std::uint64_t Nonce = 0) {
		return {
			.Role = Role,
			.Endpoint = {"session-benchmark", 27030},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 1200,
			.ClientNonce = Nonce,
			.AllowInsecureDevelopmentNetwork = true,
		};
	}

	double Milliseconds(std::chrono::steady_clock::time_point Started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
	}

	void RunAdmission(std::size_t PeerCount, std::size_t SpatialObjectCount = 0) {
		struct RawPeer {
			std::shared_ptr<SimulatedTransport> Transport;
			ConnectionId Connection;
			bool ReadySent = false;
			std::vector<std::byte> DuplicateReady;
		};

		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		TransportConfiguration.MaximumTransports = MaximumGameSessionPeers + 1;
		TransportConfiguration.MaximumConnections = MaximumGameSessionPeers;
		TransportConfiguration.MaximumPendingEventsPerTransport = MaximumGameSessionPeers * 4;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(
			World,
			&Renderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		std::vector<std::shared_ptr<Part>> SpatialObjects;
		SpatialObjects.reserve(SpatialObjectCount);
		for (std::size_t Index = 0; Index < SpatialObjectCount; ++Index) {
			auto Object = std::make_shared<Part>();
			Object->SetCFrame(CFrame(10'000.0f + static_cast<float>(Index) * 512.0f, 0.0f, 0.0f));
			Object->SetParent(World);
			SpatialObjects.push_back(std::move(Object));
		}
		auto SpatialPlacement = Runtime.Players->PlayerAdded->Connect([](std::shared_ptr<Player> PlayerValue) {
			if (!PlayerValue || !PlayerValue->GetCharacter()) return;
			auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			if (CharacterValue)
				CharacterValue->SetPosition({static_cast<float>(PlayerValue->GetPlayerId() - 1) * 1024.0f, 6.0f, 0.0f});
		});
		const auto ServerConfiguration = Configuration(GameSessionRole::Server);
		const auto ClientConfiguration = Configuration(GameSessionRole::Client);
		GameSession Server(ServerTransport, ServerConfiguration, &Runtime);
		if (!Server.Start().Succeeded()) throw std::runtime_error("session benchmark server did not start");
		std::vector<RawPeer> Peers;
		Peers.reserve(PeerCount);
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			auto Transport = Network->CreateTransport();
			if (!Transport || !Transport
								   ->Start({
									   .Role = TransportRole::Client,
									   .Endpoint = ClientConfiguration.Endpoint,
									   .AdvertisedLimits = ClientConfiguration.Limits,
								   })
								   .Succeeded())
				throw std::runtime_error("session benchmark client transport did not start");
			Network->Pump();
			(void)Server.Poll();
			std::array<TransportEvent, 8> Events;
			const auto Count = Transport->PollEvents(Events);
			ConnectionId Connection;
			for (std::size_t EventIndex = 0; EventIndex < Count; ++EventIndex)
				if (const auto *Changed = std::get_if<ConnectionStateEvent>(&Events[EventIndex]);
					Changed && Changed->Current == ConnectionState::Connected)
					Connection = Changed->Connection;
			if (!Connection.IsValid()) throw std::runtime_error("session benchmark client did not connect");
			auto Hello = EncodeGameSessionMessage(
				GameSessionClientHello{static_cast<std::uint64_t>(Index + 1), ClientConfiguration.Limits}
			);
			auto Intent = Hello ? MakeNetworkMessageIntent(
									  Connection,
									  DeliveryMode::ReliableOrdered,
									  TrafficClass::Control,
									  {},
									  std::move(*Hello),
									  ClientConfiguration.Limits
								  )
								: std::nullopt;
			if (!Intent || !Transport->Send(*Intent).Succeeded())
				throw std::runtime_error("session benchmark client hello was not submitted");
			Peers.push_back({std::move(Transport), Connection});
		}
		std::uint64_t Tick = 1;
		while (Tick <= 1200 && Server.GetMetrics().ReadyPeers != PeerCount) {
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick);
			(void)Network->Advance(std::chrono::milliseconds(1));
			Network->Pump();
			for (auto &Peer : Peers) {
				std::array<TransportEvent, 64> Events;
				const auto Count = Peer.Transport->PollEvents(Events);
				for (std::size_t Index = 0; Index < Count && !Peer.ReadySent; ++Index) {
					const auto *Received = std::get_if<ReceivedMessageEvent>(&Events[Index]);
					if (!Received || !IsGameSessionFrame(Received->Payload)) continue;
					auto Decoded = DecodeGameSessionMessage(Received->Payload);
					const auto *Accepted = Decoded ? std::get_if<GameSessionServerAccepted>(&*Decoded) : nullptr;
					if (!Accepted) continue;
					auto Ready = EncodeGameSessionMessage(
						GameSessionClientReady{Accepted->SessionEpoch, Accepted->Replication, Accepted->Player}
					);
					if (Ready) Peer.DuplicateReady = *Ready;
					auto ReadyIntent = Ready ? MakeNetworkMessageIntent(
												   Peer.Connection,
												   DeliveryMode::ReliableOrdered,
												   TrafficClass::Control,
												   {},
												   std::move(*Ready),
												   Accepted->NegotiatedLimits
											   )
											 : std::nullopt;
					if (!ReadyIntent || !Peer.Transport->Send(*ReadyIntent).Succeeded())
						throw std::runtime_error("session benchmark client readiness was not submitted");
					Peer.ReadySent = true;
				}
			}
			++Tick;
		}
		const auto Duration = Milliseconds(Started);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto Metrics = Server.GetMetrics();
		if (Metrics.ReadyPeers != PeerCount)
			throw std::runtime_error(
				"session benchmark did not activate every peer: ready=" + std::to_string(Metrics.ReadyPeers) +
				" accepted=" + std::to_string(Metrics.AcceptedPeers) +
				" connected=" + std::to_string(Metrics.TransportConnections) + " playersCreated=" +
				std::to_string(Metrics.PlayersCreated) + " playersRemoved=" + std::to_string(Metrics.PlayersRemoved) +
				" rejected=" + std::to_string(Metrics.RejectedHandshakes) + " protocol=" +
				std::to_string(Metrics.ProtocolRejects) + " timeouts=" + std::to_string(Metrics.HandshakeTimeouts)
			);
		std::cout << (SpatialObjectCount == 0 ? "Admission," : "WorldAdmission,")
				  << (SpatialObjectCount == 0 ? PeerCount : SpatialObjectCount) << ',' << Duration << ','
				  << Duration / static_cast<double>(PeerCount) << ',' << Tick - 1 << ',' << Metrics.PlayersCreated
				  << ',' << Metrics.CharacterControlBindings << ',' << Allocations << ','
				  << Metrics.SessionAcceptanceCpuNanoseconds << ',' << Metrics.PlayerCreationCpuNanoseconds << ','
				  << Metrics.ServerGraphSynchronizationCpuNanoseconds << ',' << Metrics.BaselineSnapshotCpuNanoseconds
				  << ',' << Metrics.BaselineDiscoveryCpuNanoseconds << ',' << Metrics.BaselineEncodeCpuNanoseconds
				  << ',' << Metrics.GameplayRegistrationCpuNanoseconds << ',' << Metrics.RelevantObjects << ','
				  << Metrics.RelevanceEnters << ',' << Metrics.RelevanceLeaves << ',' << Metrics.RelevanceQueries << ','
				  << Metrics.RelevanceCandidates << ',' << Metrics.RelevanceCpuNanoseconds << ','
				  << Metrics.MaterializedObjects << ',' << Metrics.MaterializedCharacters << ','
				  << Metrics.RelevanceInitializationCpuNanoseconds << ',' << Metrics.MaterializationBacklog << ','
				  << Metrics.MaterializationTransitions << ',' << Metrics.MaterializationCpuNanoseconds << ','
				  << Metrics.StructuralTemplateBuilds << ',' << Metrics.StructuralTemplateHits << ','
				  << Metrics.StructuralTemplateMisses << ',' << Metrics.StructuralTemplateInvalidations << ','
				  << Metrics.StructuralTemplateBytes << ',' << Metrics.PeerMaterializationPlans << ','
				  << Metrics.PeerPatchOperations << ',' << Metrics.ReferencePatchOperations << ','
				  << Metrics.StructuralBytesReused << ',' << Metrics.StructuralBytesEncoded << ','
				  << Metrics.ScratchHighWaterBytes << '\n';
		if (SpatialObjectCount == 0 && PeerCount <= 32) {
			const auto Before = Server.GetMetrics();
			const auto FailureAllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto FailureStarted = std::chrono::steady_clock::now();
			for (auto &Peer : Peers) {
				auto Intent = MakeNetworkMessageIntent(
					Peer.Connection,
					DeliveryMode::ReliableOrdered,
					TrafficClass::Control,
					{},
					std::move(Peer.DuplicateReady),
					ClientConfiguration.Limits
				);
				if (!Intent || !Peer.Transport->Send(*Intent).Succeeded())
					throw std::runtime_error("session benchmark failure trigger was not submitted");
				(void)Network->Advance(std::chrono::milliseconds(1));
				Network->Pump();
				(void)Server.Poll();
				Server.Step(++Tick);
			}
			const auto FailureDuration = Milliseconds(FailureStarted);
			const auto FailureAllocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) -
											FailureAllocationsBefore;
			const auto After = Server.GetMetrics();
			if (After.PlayersRemoved - Before.PlayersRemoved != PeerCount)
				throw std::runtime_error("session benchmark did not tear down every failed peer");
			std::cout << "PeerFailure," << PeerCount << ',' << FailureDuration << ','
					  << FailureDuration / static_cast<double>(PeerCount) << ',' << PeerCount << ','
					  << After.PlayersRemoved - Before.PlayersRemoved << ','
					  << After.CharacterControlRevocations - Before.CharacterControlRevocations << ','
					  << FailureAllocations << ",0,0,0,0,0,0,0," << After.RelevantObjects << ",0,"
					  << After.RelevanceLeaves - Before.RelevanceLeaves << ','
					  << After.RelevanceQueries - Before.RelevanceQueries << ','
					  << After.RelevanceCandidates - Before.RelevanceCandidates << ','
					  << After.RelevanceCpuNanoseconds - Before.RelevanceCpuNanoseconds << ','
					  << After.MaterializedObjects << ',' << After.MaterializedCharacters << ",0,"
					  << After.MaterializationBacklog << ','
					  << After.MaterializationTransitions - Before.MaterializationTransitions << ','
					  << After.MaterializationCpuNanoseconds - Before.MaterializationCpuNanoseconds << ",0,0,0,0,"
					  << After.StructuralTemplateBytes << ",0,0,0,0,0,0\n";
		} else {
			for (auto &Peer : Peers)
				(void)Peer.Transport->Stop({DisconnectReason::LocalShutdown, "session benchmark complete"});
		}
		Server.Stop();
		Runtime.Destroy();
	}

	void RunStructuralMaterialization(
		std::string_view Kind,
		std::size_t PeerCount,
		std::size_t ObjectCount,
		bool TemplateReuse,
		std::size_t MutationStride = 0,
		std::size_t SparseObjectsPerPeer = 0
	) {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Objects;
		Objects.reserve(ObjectCount);
		for (std::size_t Index = 0; Index < ObjectCount; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetName("StructuralObject" + std::to_string(Index));
			Object->SetParent(World);
			Objects.push_back(std::move(Object));
		}
		ReplicationCoordinator Coordinator(World, {}, TemplateReuse);
		std::uint64_t PublishedObjects = 0;
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		for (std::size_t Peer = 0; Peer < PeerCount; ++Peer) {
			if (MutationStride != 0 && Peer != 0 && Peer % MutationStride == 0) {
				const auto Object = (Peer / MutationStride - 1) % Objects.size();
				Objects[Object]->SetName("RevisedStructuralObject" + std::to_string(Peer));
			}
			ReplicationProduceResult Baseline;
			if (SparseObjectsPerPeer == 0) {
				Baseline = Coordinator.AddPeer({static_cast<std::uint32_t>(Peer + 1), 1}, ReplicationEpoch(1));
			} else {
				PeerRelevanceSelection Selection{
					.RequiredObjects = {World->GetObjectId()},
					.DesiredObjects = {World->GetObjectId()},
				};
				for (std::size_t Offset = 0; Offset < SparseObjectsPerPeer; ++Offset)
					Selection.DesiredObjects.push_back(
						Objects[(Peer * SparseObjectsPerPeer + Offset) % Objects.size()]->GetObjectId()
					);
				std::ranges::sort(Selection.DesiredObjects);
				Baseline = Coordinator.AddPeer(
					{static_cast<std::uint32_t>(Peer + 1), 1}, ReplicationEpoch(1), Selection
				);
			}
			if (!Baseline.Succeeded()) throw std::runtime_error("structural materialization benchmark failed");
			PublishedObjects += Baseline.Frame->Operations.size();
		}
		const auto Duration = Milliseconds(Started);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto &Metrics = Coordinator.GetMetrics();
		std::cout << Kind << ',' << PeerCount << ',' << Duration << ',' << Duration / static_cast<double>(PeerCount)
				  << ",1," << PublishedObjects << ",0," << Allocations << ",0,0,0,"
				  << Metrics.SnapshotCaptureCpuNanoseconds << ',' << Metrics.BaselineDiscoveryCpuNanoseconds << ','
				  << Metrics.BaselineEncodeCpuNanoseconds << ",0,0,0,0,0,0,0," << PublishedObjects << ",0,0,"
				  << Metrics.MaterializationBacklog << ',' << Metrics.RelevanceTransitions << ','
				  << Metrics.RelevanceTransitionCpuNanoseconds << ',' << Metrics.StructuralTemplateBuilds << ','
				  << Metrics.StructuralTemplateHits << ',' << Metrics.StructuralTemplateMisses << ','
				  << Metrics.StructuralTemplateInvalidations << ',' << Metrics.StructuralTemplateBytes << ','
				  << Metrics.PeerMaterializationPlans << ',' << Metrics.PeerPatchOperations << ','
				  << Metrics.ReferencePatchOperations << ',' << Metrics.StructuralBytesReused << ','
				  << Metrics.StructuralBytesEncoded << ',' << Metrics.ScratchHighWaterBytes << '\n';
	}

	void RunInterestMaterialization(std::size_t InterestSize) {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Objects;
		Objects.reserve(InterestSize);
		PeerRelevanceSelection Selection{
			.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId()},
		};
		for (std::size_t Index = 0; Index < InterestSize; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetParent(World);
			Selection.DesiredObjects.push_back(Object->GetObjectId());
			Objects.push_back(std::move(Object));
		}
		std::ranges::sort(Selection.DesiredObjects);
		ReplicationCoordinator Coordinator(World);
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		auto Baseline = Coordinator.AddPeer({1, 1}, ReplicationEpoch(1), Selection);
		const auto Duration = Milliseconds(Started);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		if (!Baseline.Succeeded()) throw std::runtime_error("interest materialization benchmark failed");
		const auto &Metrics = Coordinator.GetMetrics();
		std::cout << "InterestMaterialization," << InterestSize << ',' << Duration << ','
				  << Duration / static_cast<double>(InterestSize) << ",1," << Baseline.Frame->Operations.size() << ",0,"
				  << Allocations << ",0,0,0," << Metrics.SnapshotCaptureCpuNanoseconds << ','
				  << Metrics.BaselineDiscoveryCpuNanoseconds << ',' << Metrics.BaselineEncodeCpuNanoseconds
				  << ",0,0,0,0,0,0,0," << Baseline.Frame->Operations.size() << ",0,0," << Metrics.MaterializationBacklog
				  << ',' << Metrics.RelevanceTransitions << ',' << Metrics.RelevanceTransitionCpuNanoseconds << ','
				  << Metrics.StructuralTemplateBuilds << ',' << Metrics.StructuralTemplateHits << ','
				  << Metrics.StructuralTemplateMisses << ',' << Metrics.StructuralTemplateInvalidations << ','
				  << Metrics.StructuralTemplateBytes << ',' << Metrics.PeerMaterializationPlans << ','
				  << Metrics.PeerPatchOperations << ',' << Metrics.ReferencePatchOperations << ','
				  << Metrics.StructuralBytesReused << ',' << Metrics.StructuralBytesEncoded << ','
				  << Metrics.ScratchHighWaterBytes << '\n';
	}

	void RunRelevanceUpdate(std::size_t PeerCount) {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		Characters.reserve(PeerCount);
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			auto CharacterValue = std::make_shared<KinematicCharacter>();
			CharacterValue->SetPosition({static_cast<float>(Index) * 64.0f, 6.0f, 0.0f});
			CharacterValue->SetParent(World);
			Characters.push_back(std::move(CharacterValue));
		}
		ReplicationRelevance Relevance(World);
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			const ConnectionId Connection{static_cast<std::uint32_t>(Index + 1), 1};
			const auto Character = Characters[Index]->GetObjectId();
			if (!Relevance.AddPeer(Connection, Character, Character))
				throw std::runtime_error("relevance update benchmark peer registration failed");
		}
		for (std::size_t Index = PeerCount / 2; Index < PeerCount; ++Index) {
			const ConnectionId Connection{static_cast<std::uint32_t>(Index + 1), 1};
			const float Offset = Index < PeerCount * 4 / 5 ? 128.0f : 32'000.0f;
			const std::array Focus{glm::vec3(static_cast<float>(Index) * 64.0f + Offset, 6.0f, 0.0f)};
			if (!Relevance.SetTrustedFocus(Connection, Focus))
				throw std::runtime_error("relevance update benchmark focus assignment failed");
		}
		const auto Before = Relevance.GetMetrics();
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		if (!Relevance.Update(6)) throw std::runtime_error("relevance update benchmark failed");
		const auto Duration = Milliseconds(Started);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto After = Relevance.GetMetrics();
		std::cout << "RelevanceUpdate," << PeerCount << ',' << Duration << ','
				  << Duration / static_cast<double>(PeerCount) << ",1," << After.DesiredObjects << ",0," << Allocations
				  << ",0,0,0,0,0,0,0," << After.DesiredObjects << ',' << After.RelevanceEnters - Before.RelevanceEnters
				  << ',' << After.RelevanceLeaves - Before.RelevanceLeaves << ','
				  << After.SpatialQueries - Before.SpatialQueries << ','
				  << After.CandidateObjects - Before.CandidateObjects << ','
				  << After.UpdateCpuNanoseconds - Before.UpdateCpuNanoseconds << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
	}

	void RunCommandBridge(std::size_t CharacterCount) {
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(
			World,
			&Renderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		Characters.reserve(CharacterCount);
		for (std::size_t Index = 0; Index < CharacterCount; ++Index)
			Characters.push_back(std::make_shared<KinematicCharacter>());
		constexpr std::uint64_t Ticks = 120;
		for (std::size_t Index = 0; Index < CharacterCount; ++Index) {
			CharacterInputCommand Warmup{
				.Character = Characters[Index]->GetObjectId(),
				.ControlEpoch = CharacterControlEpoch(1),
				.InputSequence = CharacterInputSequence(1),
				.SimulationTick = 1,
				.DeltaSeconds = 1.0f / 60.0f,
			};
			(void)Runtime.CharacterControl->EvaluateMovement(Warmup, *Characters[Index]);
		}
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		for (std::uint64_t Tick = 1; Tick <= Ticks; ++Tick) {
			for (std::size_t Index = 0; Index < CharacterCount; ++Index) {
				CharacterInputCommand Command{
					.Character = Characters[Index]->GetObjectId(),
					.ControlEpoch = CharacterControlEpoch(1),
					.InputSequence = CharacterInputSequence(Tick),
					.SimulationTick = Tick,
					.DeltaSeconds = 1.0f / 60.0f,
					.MoveIntent = {1.0f, 0.0f},
				};
				(void)Runtime.CharacterControl->EvaluateMovement(Command, *Characters[Index]);
			}
		}
		const auto Duration = Milliseconds(Started);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto Calls = static_cast<double>(CharacterCount * Ticks);
		std::cout << "CommandBridge," << CharacterCount << ',' << Duration << ',' << Duration * 1000.0 / Calls << ','
				  << Ticks << ',' << static_cast<std::uint64_t>(Calls) << ",0," << Allocations
				  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
		Runtime.Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		std::cout << "Kind,Count,DurationMs,MillisecondsPerUnit,Ticks,ResultCount,ControlBindings,Allocations,"
					 "SessionAcceptanceNs,PlayerCreationNs,ServerGraphSynchronizationNs,BaselineSnapshotNs,"
					 "BaselineDiscoveryNs,BaselineEncodeNs,GameplayRegistrationNs,RelevantObjects,RelevanceEnters,"
					 "RelevanceLeaves,RelevanceQueries,RelevanceCandidates,RelevanceCpuNs,MaterializedObjects,"
					 "MaterializedCharacters,RelevanceInitializationNs,MaterializationBacklog,"
					 "MaterializationTransitions,MaterializationCpuNs,StructuralTemplateBuilds,"
					 "StructuralTemplateHits,StructuralTemplateMisses,StructuralTemplateInvalidations,"
					 "StructuralTemplateBytes,PeerMaterializationPlans,PeerPatchOperations,"
					 "ReferencePatchOperations,StructuralBytesReused,StructuralBytesEncoded,ScratchHighWaterBytes\n";
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--admission-500") {
			RunAdmission(500);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--world-scale") {
			for (const auto WorldSize : {1'000u, 10'000u, 50'000u})
				RunAdmission(1, WorldSize);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--relevance-500") {
			RunRelevanceUpdate(500);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--structural-materialization") {
			RunStructuralMaterialization("StructuralShared128", 500, 128, true);
			RunStructuralMaterialization("StructuralUncached128", 500, 128, false);
			RunStructuralMaterialization("StructuralMutation128", 256, 128, true, 4);
			RunStructuralMaterialization("StructuralSparse4096", 500, 4'096, true, 0, 16);
			return 0;
		}
		for (const auto Count : {1u, 32u, 100u, 500u})
			RunAdmission(Count);
		for (const auto WorldSize : {1'000u, 10'000u, 50'000u})
			RunAdmission(1, WorldSize);
		for (const auto InterestSize : {50u, 500u, 5'000u})
			RunInterestMaterialization(InterestSize);
		RunRelevanceUpdate(500);
		for (const auto Count : {1u, 10u, 100u, 500u})
			RunCommandBridge(Count);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:SessionBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
