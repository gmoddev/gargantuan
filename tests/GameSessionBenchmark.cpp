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
#include "ContentScaleFixture.hpp"
#include "ContentScaleGameplay.hpp"
#include "../src/network/GameSessionTestAccess.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// clang-format off: psapi.h requires the Win32 API types.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif
// clang-format on

namespace {
	std::atomic<std::uint64_t> GameSessionBenchmarkAllocations = 0;
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GARGANTUAN_SCALE_ASAN 1
#endif
#endif

// ASan must own every allocation entry point, including shared gRPC's nothrow
// new overloads. A partial global replacement mixes its new with our free.
// Count allocations only in ordinary benchmark builds; never suppress ASan.
#if !defined(GARGANTUAN_SCALE_ASAN) && !defined(__SANITIZE_ADDRESS__)
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
#endif

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

	double Percentile(std::vector<double> Samples, double Fraction) {
		if (Samples.empty()) return 0.0;
		std::ranges::sort(Samples);
		const auto Index = static_cast<std::size_t>(Fraction * static_cast<double>(Samples.size() - 1));
		return Samples[Index];
	}

	void PrintScaleMemory() {
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters), sizeof(Counters)))
			throw std::runtime_error("scale process memory query failed");
		std::cout << " rssBytes=" << Counters.WorkingSetSize << " rssHighWaterBytes=" << Counters.PeakWorkingSetSize
			<< " privateBytes=" << Counters.PrivateUsage;
#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t Pages = 0, Resident = 0;
		Input >> Pages >> Resident;
		rusage Usage{};
		if (!Input || getrusage(RUSAGE_SELF, &Usage) != 0) throw std::runtime_error("scale process memory query failed");
		std::cout << " rssBytes=" << Resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE))
			<< " rssHighWaterBytes=" << static_cast<std::uint64_t>(Usage.ru_maxrss) * 1024;
#endif
	}

	void RunContentScale(Engine &Runtime, GameSession &Server, const std::shared_ptr<SimulatedNetwork> &Network,
		auto &Peers, std::uint64_t &Tick, bool FullRateInput) {
		Runtime.ProcessService->Alive = true;
		std::vector<std::shared_ptr<AnimationTrack>> RootTracks;
		std::vector<std::shared_ptr<KinematicCharacter>> RootCharacters;
		for (const auto &PlayerValue : Runtime.Players->GetPlayers()) {
			if (RootTracks.size() == 10) break;
			if (!PlayerValue->GetCharacter()) throw std::runtime_error("root-motion workload has no Character");
			const auto CharacterValue = *PlayerValue->GetCharacter();
			RootCharacters.push_back(std::dynamic_pointer_cast<KinematicCharacter>(CharacterValue));
			for (auto &Peer : Peers) Peer.Content.RootMotionCharacters.insert(CharacterValue->GetObjectId());
			auto Rig = std::make_shared<MeshPart>();
			Rig->SetName("ScaleAnimationRig");
			Rig->SetMesh("asset://d549080bd1e64aaee8041f4ece3e9f75");
			Rig->SetAnchored(true); Rig->SetCanCollide(false); Rig->SetCanTouch(false);
			Rig->SetCFrame(CharacterValue->GetCFrame()); Rig->SetParent(CharacterValue);
			auto AnimatorValue = std::make_shared<Animator>();
			AnimatorValue->SetParent(Rig);
			auto Track = AnimatorValue->CreateTrack("asset://d9d9e9649adbad59588d137c2a642e1d");
			Track->SetRootMotionEnabled(true); Track->SetLooped(true); Track->Play();
			RootTracks.push_back(std::move(Track));
		}
		const auto Objects = Runtime.Content->GetManifest()->Entries.front().ObjectCount;
		std::vector<double> TickTimes;
		std::uint64_t PendingHighWater = 0;
		std::map<ObjectId, SpatialCellAddress> RootCells;
		std::uint64_t RootCellCrossings = 0;
		bool GameplayFailed = false;
		const auto InputPeriod = Peers.size() == 500 && !FullRateInput ? 5u : 1u;
		auto Step = [&] {
			for (auto &Peer : Peers) if (Peer.Content.Control && !Peer.Session) {
				// The existing host polls 128 events per tick. Preserve that bound;
				// stagger representative 500-peer input at 12 Hz, while keeping the
				// explicit 60 Hz overload diagnostic available as a separate case.
				if (Tick % InputPeriod != Peer.Connection.Slot % InputPeriod) continue;
				const auto &Binding = *Peer.Content.Control;
				auto Encoded = EncodeCharacterMessage(CharacterInputCommand{
					.Character = Binding.Character, .ControlEpoch = Binding.ControlEpoch,
					.InputSequence = CharacterInputSequence{++Peer.Content.InputSequence}, .SimulationTick = Tick,
					.DeltaSeconds = 1.0f / 60.0f, .MoveIntent = {0.25f, 0.0f},
				});
				auto Intent = Encoded ? MakeNetworkMessageIntent(Peer.Connection, DeliveryMode::UnreliableSequenced,
					TrafficClass::RealtimeState, RealtimeStateOrder{Binding.Channel, RealtimeStateSequence{Peer.Content.InputSequence}},
					std::move(*Encoded), Configuration(GameSessionRole::Client).Limits) : std::nullopt;
				if (!Intent || !Peer.Transport->Send(*Intent).Succeeded())
					throw std::runtime_error("scale owner input rejected by transport");
			}
			Network->Pump();
			const auto Started = std::chrono::steady_clock::now();
			(void)Server.Poll();
			Runtime.Step();
			Server.Step(Tick++);
			TickTimes.push_back(Milliseconds(Started));
			for (const auto &CharacterValue : RootCharacters) {
				const auto Id = CharacterValue->GetObjectId();
				if (const auto Cell = detail::GameSessionTestAccess::GetSpatialCellAddress(Server, Id)) {
					const auto Previous = RootCells.find(Id);
					if (Previous != RootCells.end() && Previous->second != *Cell) ++RootCellCrossings;
					RootCells[Id] = *Cell;
				}
			}
			const auto Metrics = Server.GetMetrics();
			PendingHighWater = std::max(PendingHighWater, Metrics.StructuralPendingEnters + Metrics.StructuralPendingLeaves);
			if (Metrics.StructuralMaximumTransitionsSelectedPerTick > 8192 || Metrics.StructuralJournalLagFailures != 0 ||
				Metrics.StructuralMaximumJournalLagRecords > 16384 || Metrics.ReadyPeers != Peers.size())
				throw std::runtime_error("scale structural cap, journal, or peer invariant failed");
			(void)Network->Advance(std::chrono::milliseconds(17));
			Network->Pump();
			for (auto &Peer : Peers) {
				if (Peer.Session) {
					(void)Peer.Session->Poll();
					Peer.Runtime->Step();
					Peer.Session->Step(Tick);
					if (Peer.Session->GetStatus() != GameSessionStatus::Ready)
						throw std::runtime_error("real scale client lost Ready at tick " + std::to_string(Tick) + ": " +
							Peer.Session->GetFailure() + " transport=" + Peer.Content.DisconnectDiagnostic);
					continue;
				}
				std::array<TransportEvent, 256> Events;
				for (;;) {
					const auto Count = Peer.Transport->PollEvents(Events);
					for (std::size_t Index = 0; Index < Count; ++Index) {
						if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Events[Index]))
							Peer.Content.Observe(Message->Payload);
						if (std::holds_alternative<DisconnectedEvent>(Events[Index]))
							throw std::runtime_error("scale peer disconnected");
					}
					if (Count < Events.size()) break;
				}
			}
		};
		auto RunPhase = [&](std::string_view Phase, std::size_t ExpectedObjects, bool WaitForConvergence) {
			// Fixture placement at a cell edge exercises ordinary animation-driven
			// dirty projection; no spatial index or Desired/Known mutation is used.
			RootCells.clear();
			RootCellCrossings = 0;
			for (const auto &CharacterValue : RootCharacters) {
				auto Position = CharacterValue->GetCFrame().Position;
				Position.x = 127.99f;
				CharacterValue->SetPosition(Position);
				RootCells.emplace(CharacterValue->GetObjectId(), *SpatialCellAddressForPosition(glm::dvec3(Position)));
			}
			const auto Started = std::chrono::steady_clock::now();
			const auto StartTick = Tick;
			const auto Before = Server.GetMetrics();
			const auto CharacterBefore = detail::GameSessionTestAccess::GetCharacterMetrics(Server);
			const auto RootBefore = Runtime.GetCharacterRootMotionMetrics();
			const auto ClientControl = Peers.front().Runtime->CharacterControl;
			auto Number = [](const std::shared_ptr<Instance> &Value, std::string_view Name) {
				const auto Attribute = Value->GetAttributeValue(Name);
				return Attribute && std::holds_alternative<double>(*Attribute) ? std::get<double>(*Attribute) : 0.0;
			};
			const auto ActionsBefore = Number(ClientControl, "ScaleActionResolutions");
			const auto EndingsBefore = Number(ClientControl, "ScaleActionEndings");
			const auto SubmissionFailuresBefore = Number(ClientControl, "ScaleActionSubmissionFailures");
			const auto RejectionsBefore = Number(ClientControl, "ScaleActionRejections");
			const auto EventsBefore = Number(Runtime.CharacterControl, "ScaleEvents");
			(void)ClientControl->ApplyAttributeMutation("ScalePhase", WireValue(std::string(Phase)));
			TickTimes.clear();
			std::vector<double> Convergence(Peers.size(), 0.0);
			std::vector<double> ConvergenceTicks(Peers.size(), 0.0);
			for (auto &Peer : Peers) {
				Peer.Content.MaximumGapTicks = 0;
				Peer.Content.LastStateTick.clear();
				Peer.Content.RootMotionStates = 0;
				Peer.Content.RootMotionMaximumGapTicks = 0;
			}
			while (Tick - StartTick < 1200) {
				Step();
				bool Complete = true;
				for (std::size_t Index = 0; Index < Peers.size(); ++Index) {
					if (Peers[Index].Content.Objects.size() != ExpectedObjects) Complete = false;
					else if (Convergence[Index] == 0.0) {
						Convergence[Index] = Milliseconds(Started);
						ConvergenceTicks[Index] = static_cast<double>(Tick - StartTick);
					}
				}
				const bool RemotesComplete = ClientControl->GetAttributeValue("ScaleRemoteDone") ==
					std::optional<WireValue>(WireValue(std::string(Phase)));
				if ((!WaitForConvergence || Complete) && RemotesComplete && Tick - StartTick >= 240) break;
			}
			for (const auto &Peer : Peers) if (Peer.Content.Objects.size() != ExpectedObjects) {
				for (const auto &PlayerValue : Runtime.Players->GetPlayers()) {
					if (!PlayerValue->GetCharacter()) continue;
					const auto Position = (*PlayerValue->GetCharacter())->GetCFrame().Position;
					std::cerr << "[Content:Scale] focus=" << Position.x << ',' << Position.y << ',' << Position.z << '\n';
					break;
				}
				throw std::runtime_error("scale content failed peer convergence: phase=" + std::string(Phase) +
					" observed=" + std::to_string(Peer.Content.Objects.size()) + " expected=" + std::to_string(ExpectedObjects) +
					" acquisitions=" + std::to_string(Runtime.Content->GetMetrics().Acquisitions) +
					" failures=" + std::to_string(Runtime.Content->GetMetrics().Failures) +
					" resident=" + std::to_string(Runtime.Content->GetMetrics().ResidentUnits) +
					" backlog=" + std::to_string(Server.GetMetrics().MaterializationBacklog));
			}
			const auto After = Server.GetMetrics();
			const bool RemotesComplete = ClientControl->GetAttributeValue("ScaleRemoteDone") ==
				std::optional<WireValue>(WireValue(std::string(Phase)));
			GameplayFailed = GameplayFailed || !RemotesComplete || Number(ClientControl, "ScaleRemoteErrors") != 0;
			const bool ActionEventStalled = Number(ClientControl, "ScaleActionResolutions") <= ActionsBefore ||
				Number(ClientControl, "ScaleActionEndings") <= EndingsBefore ||
				Number(Runtime.CharacterControl, "ScaleEvents") <= EventsBefore;
			GameplayFailed = GameplayFailed || ActionEventStalled;
			const auto RemoteMetrics = ClientControl->GetAttributeValue("ScaleRemoteMetrics");
			if (RemoteMetrics) std::cout << std::get<std::string>(*RemoteMetrics) << '\n';
			const auto RootAfter = Runtime.GetCharacterRootMotionMetrics();
			const auto PhaseTicks = Tick - StartTick;
			const auto WallMilliseconds = Milliseconds(Started);
			const auto CharacterAfter = detail::GameSessionTestAccess::GetCharacterMetrics(Server);
			std::uint64_t MaximumGap = 0, RootMaximumGap = 0, RootStates = 0;
			for (const auto &Peer : Peers) {
				MaximumGap = std::max(MaximumGap, Peer.Content.MaximumGapTicks);
				RootMaximumGap = std::max(RootMaximumGap, Peer.Content.RootMotionMaximumGapTicks);
				RootStates += Peer.Content.RootMotionStates;
			}
			const auto PhaseRootCellCrossings = RootCellCrossings;
			if (RootStates == 0 || PhaseRootCellCrossings == 0)
				throw std::runtime_error("root-motion spatial crossing or GCHR publication stalled");
			if (RootAfter.Accepted == RootBefore.Accepted || RootAfter.Rejected != RootBefore.Rejected)
				throw std::runtime_error("Animator root motion did not commit cleanly during scale phase");
			// Authority processes owner input after the relevance safe point. Check
			// there, not against intentionally dirty transforms at end-of-tick.
			detail::GameSessionTestAccess::RequestSpatialValidation(Server);
			Step();
			TickTimes.pop_back(); // diagnostic traversal is not production tick cost
			if (!detail::GameSessionTestAccess::VerifySpatialIndex(Server))
				throw std::runtime_error("scale 3K/3H consistency check failed");
			const double Seconds = static_cast<double>(PhaseTicks) / 60.0;
			std::cout << "[Content:Scale] packageVersion=" << Runtime.Content->GetManifest()->Package.PackageVersion
				<< " peers=" << Peers.size() << " objects=" << Objects << " phase=" << Phase
				<< " ticks=" << PhaseTicks << " wallMs=" << WallMilliseconds
				<< " convergeP50Ms=" << Percentile(Convergence, 0.50) << " convergeP95Ms=" << Percentile(Convergence, 0.95)
				<< " convergeP99Ms=" << Percentile(Convergence, 0.99) << " convergeMaxMs=" << Percentile(Convergence, 1.0)
				<< " convergeMaxTicks=" << Percentile(ConvergenceTicks, 1.0)
				<< " convergeP50Ticks=" << Percentile(ConvergenceTicks, 0.50)
				<< " convergeP95Ticks=" << Percentile(ConvergenceTicks, 0.95)
				<< " convergeP99Ticks=" << Percentile(ConvergenceTicks, 0.99)
				<< " tickMeanMs=" << std::accumulate(TickTimes.begin(), TickTimes.end(), 0.0) / TickTimes.size()
				<< " tickP50Ms=" << Percentile(TickTimes, 0.50) << " tickP95Ms=" << Percentile(TickTimes, 0.95)
				<< " tickP99Ms=" << Percentile(TickTimes, 0.99) << " tickMaxMs=" << Percentile(TickTimes, 1.0)
				<< " pendingHighWater=" << PendingHighWater << " selectedCap=" << After.StructuralMaximumTransitionsSelectedPerTick
				<< " journalLag=" << After.StructuralMaximumJournalLagRecords << " journalFailures=" << After.StructuralJournalLagFailures
				<< " desiredRelationships=" << After.RelevantObjects
				<< " selectedTransitions=" << After.StructuralTransitionsSelected - Before.StructuralTransitionsSelected
				<< " committedTransitions=" << After.StructuralTransitionsCommitted - Before.StructuralTransitionsCommitted
				<< " globalCapTicks=" << After.StructuralGlobalBudgetExhaustions - Before.StructuralGlobalBudgetExhaustions
				<< " characterStatesPerSecond=" << (After.CharacterPublicationStatesAccepted - Before.CharacterPublicationStatesAccepted) / Seconds
				<< " characterBytesPerSecond=" << (After.CharacterStateBytes - Before.CharacterStateBytes) / Seconds
				<< " characterStatesPerWallSecond=" << (After.CharacterPublicationStatesAccepted - Before.CharacterPublicationStatesAccepted) * 1000.0 / WallMilliseconds
				<< " maxCharacterGapTicks=" << MaximumGap
				<< " schedulerRejections=" << After.CharacterPublicationSchedulerRejections - Before.CharacterPublicationSchedulerRejections
				<< " ownerStateAgeTicks=" << After.CharacterMaximumOwnerStateAgeTicks
				<< " characterPublicationCpuNs=" << After.CharacterPublicationSelectionCpuNanoseconds - Before.CharacterPublicationSelectionCpuNanoseconds
				<< " ownerInputsAccepted=" << CharacterAfter.CommandsAccepted - CharacterBefore.CommandsAccepted
				<< " ownerInputCadenceTicks=" << InputPeriod
				<< " actionsAccepted=" << CharacterAfter.ActionRequestsAccepted - CharacterBefore.ActionRequestsAccepted
				<< " rootRequests=" << RootAfter.Requests - RootBefore.Requests
				<< " rootCommits=" << RootAfter.Accepted - RootBefore.Accepted
				<< " rootGchrStates=" << RootStates << " rootMaxGapTicks=" << RootMaximumGap
				<< " rootCellCrossings=" << PhaseRootCellCrossings
				<< " actionResolutions=" << Number(ClientControl, "ScaleActionResolutions") - ActionsBefore
				<< " actionEndings=" << Number(ClientControl, "ScaleActionEndings") - EndingsBefore
				<< " actionSubmissionFailures=" << Number(ClientControl, "ScaleActionSubmissionFailures") - SubmissionFailuresBefore
				<< " actionRejections=" << Number(ClientControl, "ScaleActionRejections") - RejectionsBefore
				<< " actionUnexpectedEndings=" << Number(ClientControl, "ScaleActionUnexpectedEndings")
				<< " actionMaxResultUs=" << Number(ClientControl, "ScaleActionMaxResultUs")
				<< " remoteEvents=" << Number(Runtime.CharacterControl, "ScaleEvents") - EventsBefore
				<< " remoteComplete=" << RemotesComplete << " remoteErrors=" << Number(ClientControl, "ScaleRemoteErrors")
				<< " remoteSamples=" << Number(ClientControl, "ScaleRemoteSamples")
				<< " actionEventStalled=" << ActionEventStalled
				<< " clientScriptStarted=" << (ClientControl->GetAttributeValue("ScaleClientStarted") == std::optional<WireValue>(WireValue(true)))
				<< " clientHasLocalPlayer=" << Peers.front().Runtime->Players->GetLocalPlayer().has_value()
				<< " acquisitions=" << Runtime.Content->GetMetrics().Acquisitions
				<< " admissions=" << Runtime.Content->GetMetrics().Admissions
				<< " providerPayloadBytes=" << Runtime.Content->GetManifest()->Entries.front().UncompressedBytes
				<< " decodedHighWaterBytes=" << Runtime.Content->GetMetrics().DecodedDocumentBytesHighWater;
			PrintScaleMemory();
			std::cout << '\n';
			std::cout.flush();
			if (!RemotesComplete) throw std::runtime_error("100-call scale RemoteFunction exceeded its bounded phase");
		};
		RunPhase("baseline", 0, false);
		if (!Runtime.Content->RequestContent(test::ScaleContentKey)) throw std::runtime_error("scale demand rejected");
		RunPhase("load", Objects, true);
		auto Root = Runtime.Workspace->FindFirstChild(std::string(test::ScaleRootName), false);
		if (!Root || Runtime.Content->GetMetrics().Acquisitions != 1 || Runtime.Content->GetMetrics().Admissions != 1)
			throw std::runtime_error("scale region acquisition/admission was not shared");
		const auto OldId = Root->GetObjectId();
		std::vector<ObjectId> OldParts;
		for (const auto &Part : Root->GetDescendants()) {
			OldParts.push_back(Part->GetObjectId());
			if (!detail::GameSessionTestAccess::GetSpatialCellAddress(Server, Part->GetObjectId()))
				throw std::runtime_error("Resident part has no 3K/3H projection");
		}
		if (!Runtime.Content->ReleaseContent(test::ScaleContentKey)) throw std::runtime_error("scale release rejected");
		RunPhase("evict", 0, true);
		if (!Root->GetDestroyed() || ObjectRegistry::Get().Lookup(OldId)) throw std::runtime_error("stale scale runtime survived eviction");
		for (const auto Id : OldParts) if (detail::GameSessionTestAccess::GetSpatialCellAddress(Server, Id))
			throw std::runtime_error("eviction retained a stale 3K/3H projection");
		if (!Runtime.Content->RequestContent(test::ScaleContentKey)) throw std::runtime_error("scale reload rejected");
		RunPhase("reload", Objects, true);
		Root = Runtime.Workspace->FindFirstChild(std::string(test::ScaleRootName), false);
		if (!Root || Root->GetObjectId() == OldId || Runtime.Content->GetMetrics().Acquisitions != 1 ||
			Runtime.Content->GetMetrics().Admissions != 2) throw std::runtime_error("scale reload did not reuse bytes with fresh identity");
		for (const auto &Peer : Peers) if (Peer.Content.Objects.contains(OldId) || !Peer.Content.Objects.contains(Root->GetObjectId()))
			throw std::runtime_error("stale peer materialization after scale reload");
		if (GameplayFailed) throw std::runtime_error("content converged but scale RemoteFunction calls failed; see phase metrics");
		std::cout << "[Content:Scale] CONTENT_SCALE_OK peers=" << Peers.size() << '\n';
	}

	void RunAdmission(std::size_t PeerCount, std::size_t SpatialObjectCount = 0,
		std::optional<ContentAvailabilityConfiguration> ContentConfiguration = std::nullopt, bool DenseContent = false,
		bool FullRateInput = false) {
		const bool GroupedContent = ContentConfiguration && PeerCount == 500 && !DenseContent;
		struct RawPeer {
			std::shared_ptr<SimulatedTransport> Transport;
			ConnectionId Connection;
			bool ReadySent = false;
			std::vector<std::byte> DuplicateReady;
			test::ContentScalePeer Content;
			std::unique_ptr<GameSession> Session;
			std::unique_ptr<HeadlessRenderer> Renderer;
			std::unique_ptr<Engine> Runtime;
		};

		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		TransportConfiguration.MaximumTransports = MaximumGameSessionPeers + 1;
		TransportConfiguration.MaximumConnections = MaximumGameSessionPeers;
		TransportConfiguration.MaximumPendingEventsPerTransport = MaximumGameSessionPeers * 4;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto World = std::make_shared<DataModel>();
		if (ContentConfiguration) test::AddScaleGameplay(World);
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(
			World,
			&Renderer,
			nullptr,
			EngineProviderConfiguration{.Content = ContentConfiguration, .AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		if (Runtime.Content) {
			// An active Character workload needs resident ground; otherwise ordinary
			// gravity legitimately carries peer focus away from the streamed region.
			for (std::size_t Group = 0; Group < (GroupedContent ? 20u : 1u); ++Group) {
			auto Ground = std::make_shared<Part>();
			Ground->SetName("ScaleResidentGround");
			Ground->SetAnchored(true);
			Ground->SetSize({1024.0f, 1.0f, 1024.0f});
			Ground->SetPosition({static_cast<float>(Group) * 2048.0f, 0.0f, 0.0f});
			Ground->SetParent(Runtime.Workspace);
			}
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Runtime.Content->IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
				Runtime.Content->Step();
				std::this_thread::yield();
			}
			if (!Runtime.Content->IsManifestAvailable()) throw std::runtime_error("scale manifest acquisition failed");
		}
		std::vector<std::shared_ptr<Part>> SpatialObjects;
		SpatialObjects.reserve(SpatialObjectCount);
		for (std::size_t Index = 0; Index < SpatialObjectCount; ++Index) {
			auto Object = std::make_shared<Part>();
			Object->SetCFrame(CFrame(10'000.0f + static_cast<float>(Index) * 512.0f, 0.0f, 0.0f));
			Object->SetParent(World);
			SpatialObjects.push_back(std::move(Object));
		}
		auto SpatialPlacement = Runtime.Players->PlayerAdded->Connect([Scale = ContentConfiguration.has_value(), GroupedContent](std::shared_ptr<Player> PlayerValue) {
			if (!PlayerValue || !PlayerValue->GetCharacter()) return;
			auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			if (CharacterValue) {
				const auto Index = PlayerValue->GetPlayerId() - 1;
				CharacterValue->SetPosition(Scale ? glm::vec3(static_cast<float>(Index % 32) * 3.0f, 6.0f,
					static_cast<float>(Index / 32) * 3.0f) : glm::vec3(static_cast<float>(Index) * 1024.0f, 6.0f, 0.0f));
				if (GroupedContent) CharacterValue->SetPosition({static_cast<float>(Index / 25) * 2048.0f +
					static_cast<float>(Index % 5) * 3.0f, 6.0f, static_cast<float>((Index % 25) / 5) * 3.0f});
			}
		});
		const auto ServerConfiguration = Configuration(GameSessionRole::Server);
		const auto ClientConfiguration = Configuration(GameSessionRole::Client);
		GameSession Server(ServerTransport, ServerConfiguration, &Runtime);
		if (!Server.Start().Succeeded()) throw std::runtime_error("session benchmark server did not start");
		if (ContentConfiguration) Runtime.Script->RunBootstrapScript(std::dynamic_pointer_cast<Script>(World->FindFirstChild("ScaleServerPolicy", false)));
		std::vector<RawPeer> Peers;
		Peers.reserve(PeerCount);
		const auto RuntimeAssets = Runtime.Assets->CaptureRuntimeAssets();
		auto PollRealPeer = [&](RawPeer &Peer, std::uint64_t CurrentTick) {
			(void)Peer.Session->Poll();
			if (!Peer.Runtime && Peer.Session->GetClientDataModel()) {
				const auto ClientWorld = Peer.Session->GetClientDataModel();
				std::dynamic_pointer_cast<AssetService>(ClientWorld->GetService("AssetService"))->LoadRuntimeAssetSnapshot(RuntimeAssets);
				const auto Hydrated = PackageBuilder::HydrateClientCode(World, ClientWorld);
				const auto ClientScript = std::dynamic_pointer_cast<Script>(ClientWorld->FindFirstChild("ScaleClientTraffic", false));
				std::cout << "[Content:Scale] hydratedCode=" << Hydrated << " clientScriptPresent=" << static_cast<bool>(ClientScript)
					<< " clientSourceBytes=" << (ClientScript ? ClientScript->GetSource().size() : 0) << '\n';
				if (!ClientScript || ClientScript->GetSource().empty())
					throw std::runtime_error("scale workload script was absent at trusted client-code hydration");
				Peer.Renderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
				Peer.Runtime = std::make_unique<Engine>(ClientWorld, Peer.Renderer.get(), nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
				Peer.Runtime->ProcessService->Alive = true;
				if (!Peer.Session->AttachClientRuntime(*Peer.Runtime)) throw std::runtime_error("scale client runtime attachment failed");
				(void)Peer.Runtime->ProcessEvent(KeyEvent{.Device = {1}, .Physical = PhysicalKey::W,
					.Logical = LogicalKey::W, .State = ButtonState::Pressed});
			}
			if (Peer.Runtime) Peer.Runtime->Step();
			Peer.Session->Step(CurrentTick);
		};
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		std::uint64_t Tick = 1;
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			auto Transport = Network->CreateTransport();
			if (ContentConfiguration && Index == 0) {
				Peers.push_back({Transport, {}});
				auto &Peer = Peers.back();
				Peer.Session = std::make_unique<GameSession>(std::make_shared<test::ObservedScaleTransport>(Transport, Peer.Content), ClientConfiguration);
				if (!Peer.Session->Start().Succeeded()) throw std::runtime_error("real scale client did not start");
				// Establish the gameplay client before the remaining admission burst.
				// Otherwise a critical-only bootstrap may precede this fixture's
				// ordinary Script publication, so one-time package hydration misses
				// the workload. All peers still run the unchanged bounded handshake.
				while (Tick <= 1200 && Server.GetMetrics().ReadyPeers == 0) {
					Network->Pump();
					(void)Server.Poll();
					Server.Step(Tick);
					(void)Network->Advance(std::chrono::milliseconds(1));
					Network->Pump();
					PollRealPeer(Peer, Tick++);
				}
				if (Server.GetMetrics().ReadyPeers != 1) throw std::runtime_error("scale gameplay client did not establish");
				continue;
			}
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
		std::size_t PreviouslyAcceptedPeers = 0;
		std::uint64_t StructuralPendingHighWater = 0;
		std::vector<double> CriticalReadyTicks;
		std::string FirstDisconnect;
		while (Tick <= 1200 && Server.GetMetrics().ReadyPeers != PeerCount) {
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick);
			const auto Progress = Server.GetMetrics();
			StructuralPendingHighWater = std::max(
				StructuralPendingHighWater, Progress.StructuralPendingEnters + Progress.StructuralPendingLeaves
			);
			while (PreviouslyAcceptedPeers < Progress.AcceptedPeers) {
				CriticalReadyTicks.push_back(static_cast<double>(Tick));
				++PreviouslyAcceptedPeers;
			}
			(void)Network->Advance(std::chrono::milliseconds(1));
			Network->Pump();
			for (auto &Peer : Peers) {
				if (Peer.Session) { PollRealPeer(Peer, Tick); continue; }
				std::array<TransportEvent, 64> Events;
				const auto Count = Peer.Transport->PollEvents(Events);
				for (std::size_t Index = 0; Index < Count; ++Index) {
					if (ContentConfiguration) if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Events[Index]))
						Peer.Content.Observe(Message->Payload);
					if (const auto *Disconnected = std::get_if<DisconnectedEvent>(&Events[Index])) {
						if (FirstDisconnect.empty()) FirstDisconnect = Disconnected->Information.Diagnostic;
						continue;
					}
					if (Peer.ReadySent) continue;
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
				std::to_string(Metrics.ProtocolRejects) + " timeouts=" + std::to_string(Metrics.HandshakeTimeouts) +
				" pending=" + std::to_string(Metrics.StructuralPendingEnters + Metrics.StructuralPendingLeaves) +
				" selected=" + std::to_string(Metrics.StructuralTransitionsSelected) +
				" committed=" + std::to_string(Metrics.StructuralTransitionsCommitted) +
				" backlogFailures=" + std::to_string(Metrics.StructuralBacklogLimitFailures) +
				" journalLagFailures=" + std::to_string(Metrics.StructuralJournalLagFailures) + " maxJournalLag=" +
				std::to_string(Metrics.StructuralMaximumJournalLagRecords) + " firstDisconnect=" + FirstDisconnect
			);
		auto PrintAdmission = [&](std::string_view Kind,
								  double Elapsed,
								  std::uint64_t ElapsedTicks,
								  std::uint64_t AllocationCount,
								  const GameSessionMetrics &Current) {
			std::cout << Kind << ',' << (SpatialObjectCount == 0 ? PeerCount : SpatialObjectCount) << ',' << Elapsed
					  << ',' << Elapsed / static_cast<double>(PeerCount) << ',' << ElapsedTicks << ','
					  << Current.PlayersCreated << ',' << Current.CharacterControlBindings << ',' << AllocationCount
					  << ',' << Current.SessionAcceptanceCpuNanoseconds << ',' << Current.PlayerCreationCpuNanoseconds
					  << ',' << Current.ServerGraphSynchronizationCpuNanoseconds << ','
					  << Current.BaselineSnapshotCpuNanoseconds << ',' << Current.BaselineDiscoveryCpuNanoseconds << ','
					  << Current.BaselineEncodeCpuNanoseconds << ',' << Current.GameplayRegistrationCpuNanoseconds
					  << ',' << Current.RelevantObjects << ',' << Current.RelevanceEnters << ','
					  << Current.RelevanceLeaves << ',' << Current.RelevanceQueries << ','
					  << Current.RelevanceCandidates << ',' << Current.RelevanceCpuNanoseconds << ','
					  << Current.MaterializedObjects << ',' << Current.MaterializedCharacters << ','
					  << Current.RelevanceInitializationCpuNanoseconds << ',' << Current.MaterializationBacklog << ','
					  << Current.MaterializationTransitions << ',' << Current.MaterializationCpuNanoseconds << ','
					  << Current.StructuralTemplateBuilds << ',' << Current.StructuralTemplateHits << ','
					  << Current.StructuralTemplateMisses << ',' << Current.StructuralTemplateInvalidations << ','
					  << Current.StructuralTemplateBytes << ',' << Current.PeerMaterializationPlans << ','
					  << Current.PeerPatchOperations << ',' << Current.ReferencePatchOperations << ','
					  << Current.StructuralBytesReused << ',' << Current.StructuralBytesEncoded << ','
					  << Current.ScratchHighWaterBytes << ',' << StructuralPendingHighWater << ','
					  << Current.StructuralMaximumTransitionsSelectedPerTick << ','
					  << Current.StructuralTransitionsSelected << ',' << Current.StructuralTransitionsCommitted << ','
					  << Current.StructuralTransitionsDeferredByBudget << ','
					  << Current.StructuralGlobalBudgetExhaustions << ',' << Percentile(CriticalReadyTicks, 0.50) << ','
					  << Percentile(CriticalReadyTicks, 0.95) << ',' << Percentile(CriticalReadyTicks, 0.99) << ','
					  << (CriticalReadyTicks.empty() ? 0.0 : *std::ranges::max_element(CriticalReadyTicks)) << ','
					  << Current.StructuralJournalLagFailures << ',' << Current.StructuralMaximumJournalLagRecords
					  << '\n';
		};
		PrintAdmission(
			SpatialObjectCount == 0 ? "AdmissionCriticalReady" : "WorldAdmissionCriticalReady",
			Duration,
			Tick - 1,
			Allocations,
			Metrics
		);
		if (ContentConfiguration) {
			// Keep the initial neighborhood and common content site relevant even
			// while the gameplay client moves during a long phase. The grouped
			// 500-peer case observes 25 local plus 25 shared Characters. No Desired
			// or Known set is injected; 3E computes both spatial selections normally.
			for (const auto Connection : detail::GameSessionTestAccess::GetConnections(Server)) {
				const auto PlayerValue = Server.GetAcceptedPlayer(Connection);
				if (!PlayerValue || !PlayerValue->GetCharacter()) throw std::runtime_error("scale peer has no Character");
				const std::array Focus{(*PlayerValue->GetCharacter())->GetCFrame().Position, glm::vec3(0.0f, 6.0f, 0.0f)};
				if (!Server.SetTrustedReplicationFocus(Connection, Focus)) throw std::runtime_error("scale trusted focus rejected");
			}
			std::cout << "[Content:Scale] relevanceProfile=" << (GroupedContent ? "25-local-plus-25-shared" : "initial-neighborhood-plus-content-site") << '\n';
		}
		while (Tick <= 1200 && (Server.GetMetrics().MaterializationBacklog != 0 || (GroupedContent && Tick < 16))) {
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick);
			const auto Progress = Server.GetMetrics();
			StructuralPendingHighWater = std::max(
				StructuralPendingHighWater, Progress.StructuralPendingEnters + Progress.StructuralPendingLeaves
			);
			(void)Network->Advance(std::chrono::milliseconds(1));
			Network->Pump();
			for (auto &Peer : Peers) {
				if (Peer.Session) { PollRealPeer(Peer, Tick); continue; }
				std::array<TransportEvent, 256> Events;
				const auto Count = Peer.Transport->PollEvents(Events);
				if (ContentConfiguration) for (std::size_t Index = 0; Index < Count; ++Index)
					if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Events[Index])) Peer.Content.Observe(Message->Payload);
			}
			++Tick;
		}
		const auto ConvergedMetrics = Server.GetMetrics();
		if (ConvergedMetrics.MaterializationBacklog != 0)
			throw std::runtime_error("session benchmark structural materialization did not converge");
		PrintAdmission(
			SpatialObjectCount == 0 ? "AdmissionConverged" : "WorldAdmissionConverged",
			Milliseconds(Started),
			Tick - 1,
			GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore,
			ConvergedMetrics
		);
		if (ContentConfiguration) RunContentScale(Runtime, Server, Network, Peers, Tick, FullRateInput);
		if (!ContentConfiguration && SpatialObjectCount == 0 && PeerCount <= 32) {
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
					  << After.StructuralTemplateBytes << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
		} else {
			for (auto &Peer : Peers)
				(void)Peer.Transport->Stop({DisconnectReason::LocalShutdown, "session benchmark complete"});
		}
		Server.Stop();
		for (auto &Peer : Peers) if (Peer.Session) {
			Peer.Session->Stop();
			if (Peer.Runtime) Peer.Runtime->Destroy();
		}
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
				  << Metrics.StructuralBytesEncoded << ',' << Metrics.ScratchHighWaterBytes
				  << ",0,0,0,0,0,0,0,0,0,0,0,0\n";
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
				  << Metrics.ScratchHighWaterBytes << ",0,0,0,0,0,0,0,0,0,0,0,0\n";
	}

	void RunStructuralScheduling(std::size_t ObjectCount, std::size_t Budget) {
		auto World = std::make_shared<DataModel>();
		std::vector<std::shared_ptr<Folder>> Objects;
		Objects.reserve(ObjectCount);
		PeerRelevanceSelection Selection{
			.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId()},
		};
		for (std::size_t Index = 0; Index < ObjectCount; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetName("ScheduledStructuralObject" + std::to_string(Index));
			Object->SetParent(World);
			Selection.DesiredObjects.push_back(Object->GetObjectId());
			Objects.push_back(std::move(Object));
		}
		std::ranges::sort(Selection.DesiredObjects);
		StructuralReplicationConfiguration StructuralConfiguration{
			.MaximumTransitionsPerPeerTick = Budget,
			.MaximumTransitionsPerTick = Budget,
			.PeerQuantum = Budget,
			.MaximumPendingTransitionsPerPeer = MaximumPeerDesiredObjects,
			.TransitionDeadlineTicks = 600,
		};
		const auto AllocationsBefore = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto TotalStarted = std::chrono::steady_clock::now();
		ReplicationCoordinator Coordinator(World, {}, true, StructuralConfiguration);
		const ConnectionId Connection{1, 1};
		const auto BootstrapStarted = std::chrono::steady_clock::now();
		auto Baseline = Coordinator.AddPeerBounded(Connection, ReplicationEpoch(1), Selection);
		const auto BootstrapMilliseconds = Milliseconds(BootstrapStarted);
		if (!Baseline.Succeeded()) throw std::runtime_error("bounded structural benchmark bootstrap failed");
		if (!Coordinator.CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded())
			throw std::runtime_error("bounded structural benchmark bootstrap commit failed");
		const auto PendingHighWater = Coordinator.GetMetrics().MaterializationBacklog;
		std::vector<double> TickMilliseconds;
		std::size_t MaximumSelected = Baseline.SelectedTransitions;
		std::uint64_t Tick = 1;
		while (Coordinator.HasPendingRelevance(Connection) && Tick <= ObjectCount + 2) {
			const auto TickStarted = std::chrono::steady_clock::now();
			auto Produced = Coordinator.ProducePendingRelevance(Connection, Budget, Tick);
			TickMilliseconds.push_back(Milliseconds(TickStarted));
			if (!Produced.Succeeded()) throw std::runtime_error("bounded structural benchmark did not make progress");
			MaximumSelected = std::max(MaximumSelected, Produced.SelectedTransitions);
			if (!Coordinator.CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence).Succeeded())
				throw std::runtime_error("bounded structural benchmark scheduler commit failed");
			++Tick;
		}
		if (Coordinator.HasPendingRelevance(Connection))
			throw std::runtime_error("bounded structural benchmark did not converge");
		const auto TotalMilliseconds = Milliseconds(TotalStarted);
		const auto Allocations = GameSessionBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto Metrics = Coordinator.GetMetrics();
		double MeanTickMilliseconds = 0.0;
		for (const auto Value : TickMilliseconds)
			MeanTickMilliseconds += Value;
		if (!TickMilliseconds.empty()) MeanTickMilliseconds /= static_cast<double>(TickMilliseconds.size());
		std::cout << "StructuralScheduling," << ObjectCount << ',' << Budget << ',' << Tick - 1 << ','
				  << TotalMilliseconds << ',' << BootstrapMilliseconds << ',' << MeanTickMilliseconds << ','
				  << Percentile(TickMilliseconds, 0.50) << ',' << Percentile(TickMilliseconds, 0.95) << ','
				  << Percentile(TickMilliseconds, 0.99) << ','
				  << (TickMilliseconds.empty() ? 0.0 : *std::ranges::max_element(TickMilliseconds)) << ','
				  << MaximumSelected << ',' << PendingHighWater << ',' << Metrics.StructuralTransitionsSelected << ','
				  << Metrics.StructuralTransitionsCommitted << ',' << Metrics.StructuralTransitionsDeferredByBudget
				  << ',' << Metrics.StructuralDeadlineMisses << ',' << Metrics.StructuralDependencyPlanOperations << ','
				  << Metrics.StructuralBacklogLimitFailures << ',' << Metrics.StructuralSelectionCpuNanoseconds << ','
				  << Metrics.StructuralBytesEncoded << ',' << Allocations << '\n';
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
				  << After.UpdateCpuNanoseconds - Before.UpdateCpuNanoseconds
				  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
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
				  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
		Runtime.Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		if (ArgumentCount == 4 && std::string_view(Arguments[1]) == "--write-content-scale") {
			test::WriteScalePackage(Arguments[2], std::stoull(Arguments[3]));
			return 0;
		}
		if ((ArgumentCount == 5 || ArgumentCount == 6) && (std::string_view(Arguments[1]) == "--content-scale" ||
			std::string_view(Arguments[1]) == "--content-scale-dense")) {
#if defined(GARGANTUAN_SCALE_ASAN) || defined(__SANITIZE_ADDRESS__)
			std::cout << "[Content:Scale] allocationInstrumentation=asan-native-count-unavailable\n";
#else
			std::cout << "[Content:Scale] allocationInstrumentation=benchmark-new-counter\n";
#endif
			if (ArgumentCount == 6 && std::string_view(Arguments[5]) != "--input-every-tick")
				throw std::invalid_argument("invalid scale input diagnostic option");
			const auto Count = std::stoull(Arguments[2]);
			if (Count != 32 && Count != 100 && Count != 500) throw std::invalid_argument("scale peers must be 32, 100, or 500");
			const std::string_view Provider = Arguments[4];
			if (Provider != "local" && Provider != "node") throw std::invalid_argument("invalid scale provider");
			RunAdmission(Count, 0, test::ScaleConfiguration(Arguments[3], Provider == "node"),
				std::string_view(Arguments[1]) == "--content-scale-dense", ArgumentCount == 6);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--structural-scheduling") {
			std::cout << "Kind,OfferedTransitions,Budget,ServiceTicks,TotalMs,BootstrapMs,MeanTickMs,P50TickMs,"
						 "P95TickMs,P99TickMs,MaxTickMs,MaximumSelected,PendingHighWater,Selected,Committed,"
						 "DeferredByBudget,DeadlineMisses,DependencyPlanOperations,BacklogLimitFailures,SelectionNs,"
						 "EncodedBytes,Allocations\n";
			for (const auto Offered : {1'000u, 2'000u, 5'000u, 10'000u})
				RunStructuralScheduling(Offered, 1'000);
			return 0;
		}
		std::cout << "Kind,Count,DurationMs,MillisecondsPerUnit,Ticks,ResultCount,ControlBindings,Allocations,"
					 "SessionAcceptanceNs,PlayerCreationNs,ServerGraphSynchronizationNs,BaselineSnapshotNs,"
					 "BaselineDiscoveryNs,BaselineEncodeNs,GameplayRegistrationNs,RelevantObjects,RelevanceEnters,"
					 "RelevanceLeaves,RelevanceQueries,RelevanceCandidates,RelevanceCpuNs,MaterializedObjects,"
					 "MaterializedCharacters,RelevanceInitializationNs,MaterializationBacklog,"
					 "MaterializationTransitions,MaterializationCpuNs,StructuralTemplateBuilds,"
					 "StructuralTemplateHits,StructuralTemplateMisses,StructuralTemplateInvalidations,"
					 "StructuralTemplateBytes,PeerMaterializationPlans,PeerPatchOperations,"
					 "ReferencePatchOperations,StructuralBytesReused,StructuralBytesEncoded,ScratchHighWaterBytes,"
					 "StructuralPendingHighWater,StructuralMaximumSelectedPerTick,StructuralSelected,"
					 "StructuralCommitted,StructuralDeferredByBudget,StructuralGlobalBudgetExhaustions,"
					 "CriticalReadyP50Tick,CriticalReadyP95Tick,CriticalReadyP99Tick,CriticalReadyMaxTick,"
					 "StructuralJournalLagFailures,StructuralMaximumJournalLagRecords\n";
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--admission-500") {
			RunAdmission(500);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--admission-normal") {
			for (const auto Count : {1u, 32u, 100u})
				RunAdmission(Count);
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
