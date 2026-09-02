#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"

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
		};
	}

	double Milliseconds(std::chrono::steady_clock::time_point Started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
	}

	void RunAdmission(std::size_t PeerCount) {
		struct RawPeer {
			std::shared_ptr<SimulatedTransport> Transport;
			ConnectionId Connection;
			bool ReadySent = false;
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
		if (Metrics.ReadyPeers != PeerCount) throw std::runtime_error("session benchmark did not activate every peer");
		std::cout << "Admission," << PeerCount << ',' << Duration << ',' << Duration / static_cast<double>(PeerCount)
				  << ',' << Tick - 1 << ',' << Metrics.PlayersCreated << ',' << Metrics.CharacterControlBindings << ','
				  << Allocations << '\n';
		for (auto &Peer : Peers)
			(void)Peer.Transport->Stop({DisconnectReason::LocalShutdown, "session benchmark complete"});
		Server.Stop();
		Runtime.Destroy();
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
				  << Ticks << ',' << static_cast<std::uint64_t>(Calls) << ",0," << Allocations << '\n';
		Runtime.Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		std::cout << "Kind,Count,DurationMs,MicrosecondsPerUnit,Ticks,ResultCount,ControlBindings,Allocations\n";
		for (const auto Count : {1u, 32u, 100u, 500u})
			RunAdmission(Count);
		for (const auto Count : {1u, 10u, 100u, 500u})
			RunCommandBridge(Count);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:SessionBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
