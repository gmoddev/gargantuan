#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
	std::atomic<std::uint64_t> CharacterBenchmarkAllocations = 0;
}

void *operator new(std::size_t Size) {
	CharacterBenchmarkAllocations.fetch_add(1, std::memory_order_relaxed);
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

	enum class Workload : std::uint8_t {
		Stationary,
		Moving,
		Mixed,
		Actions,
		ImportanceMixed,
		ImportanceReduced,
		ImportanceLow,
		ImportanceSemanticPromotion,
	};

	std::string_view WorkloadName(Workload Value) {
		switch (Value) {
		case Workload::Stationary:
			return "Stationary";
		case Workload::Moving:
			return "Moving";
		case Workload::Mixed:
			return "Mixed20Moving";
		case Workload::Actions:
			return "RootAction";
		case Workload::ImportanceMixed:
			return "ImportanceMixed10Full30Reduced60Low";
		case Workload::ImportanceReduced:
			return "ImportanceAllReduced";
		case Workload::ImportanceLow:
			return "ImportanceAllLow";
		case Workload::ImportanceSemanticPromotion:
			return "ImportanceDenseSemanticPromotion";
		}
		return "Unknown";
	}

	class MeasuringTransport final : public IGameTransport {
	  public:
		TransportOperationResult Start(const TransportStartConfiguration &) override {
			return {TransportOperationStatus::Succeeded};
		}
		TransportOperationResult Stop(DisconnectInfo Information) override {
			return {TransportOperationStatus::Succeeded, std::move(Information)};
		}
		TransportOperationResult Disconnect(ConnectionId, DisconnectInfo Information) override {
			return {TransportOperationStatus::Succeeded, std::move(Information)};
		}
		TransportOperationResult Send(const NetworkMessageIntent &Message) override {
			Bytes += Message.Payload().size();
			++Messages;
			return {TransportOperationStatus::Succeeded};
		}
		std::size_t PollEvents(std::span<TransportEvent>) override {
			return 0;
		}
		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId) const override {
			return 1200;
		}
		std::optional<NetworkStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
		std::uint64_t Bytes = 0;
		std::uint64_t Messages = 0;

		void ResetMeasurements() {
			Bytes = 0;
			Messages = 0;
		}
	};

	NetworkLimits Limits() {
		return {
			.MaximumReliableMessageBytes = 4096,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 1024 * 1024,
			.MaximumInFlightRemoteRequests = 16,
			.MaximumDecodedMessageBytes = 4096,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
		};
	}

	AssetContentId Content() {
		std::array<std::uint8_t, 4> Bytes{3, 11, 29, 47};
		return AssetContentId::Hash(Bytes);
	}

	CharacterActionDefinition Action() {
		return {
			.Token = 1,
			.Animation = AssetId::FromBuiltInName("CharacterNetworkBenchmark"),
			.ContentRevision = Content(),
			.DurationTicks = 10000,
			.EvaluateRootMotion = [](std::uint64_t From, std::uint64_t To) -> std::optional<RootMotionDelta> {
				if (To <= From) return std::nullopt;
				return RootMotionDelta{.Translation = {0.01f * static_cast<float>(To - From), 0.0f, 0.0f}};
			},
		};
	}

	CharacterMotionRequest Movement(const CharacterInputCommand &Command, const KinematicCharacter &) {
		return {
			.Translation =
				{Command.MoveIntent.x * 6.0f * Command.DeltaSeconds,
				 0.0f,
				 Command.MoveIntent.y * 6.0f * Command.DeltaSeconds},
			.Velocity = {Command.MoveIntent.x * 6.0f, 0.0f, Command.MoveIntent.y * 6.0f},
		};
	}

	double Microseconds(auto Start, auto End) {
		return std::chrono::duration<double, std::micro>(End - Start).count();
	}

	double Percentile(std::vector<double> Samples, double Fraction) {
		if (Samples.empty()) return 0.0;
		std::ranges::sort(Samples);
		const auto Index = std::min(
			Samples.size() - 1,
			static_cast<std::size_t>(std::ceil(Fraction * static_cast<double>(Samples.size())) - 1.0)
		);
		return Samples[Index];
	}

	void RunTransport(
		std::size_t CharacterCount,
		std::size_t RelevantCount,
		std::uint32_t Cadence,
		Workload WorkloadValue,
		std::size_t SimulationTicks
	) {
		MeasuringTransport Transport;
		NetworkScheduler Scheduler(Transport);
		const ConnectionId Connection{1, 1};
		const auto NetworkLimits = Limits();
		Scheduler.RegisterConnection(Connection, NetworkLimits);
		CharacterNetworkConfiguration Configuration;
		Configuration.StateUpdatesPerSecond = Cadence;
		AuthoritativeCharacterNetwork Manager(Scheduler, NetworkLimits, Movement, {}, Configuration);
		Manager.AddPeer(Connection);
		Manager.RegisterAction(Action());
		WorldRoot World;
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		Characters.reserve(CharacterCount);
		const auto FullCount = std::max<std::size_t>(1, (RelevantCount + 9) / 10);
		const auto ReducedCount = (RelevantCount * 3 + 9) / 10;
		const bool ImportanceWorkload = WorkloadValue == Workload::ImportanceMixed ||
										WorkloadValue == Workload::ImportanceReduced ||
										WorkloadValue == Workload::ImportanceLow ||
										WorkloadValue == Workload::ImportanceSemanticPromotion;
		for (std::size_t Index = 0; Index < CharacterCount; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			if (ImportanceWorkload && Index < RelevantCount) {
				float X = 220.0f;
				if (WorkloadValue == Workload::ImportanceReduced)
					X = 100.0f;
				else if (WorkloadValue == Workload::ImportanceMixed)
					X = Index < FullCount ? 24.0f : Index < FullCount + ReducedCount ? 100.0f : 220.0f;
				Character->SetPosition({X, 10.0f, static_cast<float>(Index % 16) * 0.01f});
			} else {
				Character->SetPosition(
					{static_cast<float>(Index % 25) * 4.0f, 10.0f, static_cast<float>(Index / 25) * 4.0f}
				);
			}
			Manager.RegisterCharacter(Character);
			if (Index < RelevantCount)
				Manager.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(Index + 1));
			if (WorkloadValue == Workload::Actions && Index < RelevantCount)
				Manager.StartServerAction(Character->GetObjectId(), 1, 1);
			Characters.push_back(std::move(Character));
		}
		std::uint64_t FirstMeasurementTick = 1;
		if (ImportanceWorkload) {
			Manager.SetPeerPublicationFocus(Connection, std::array{glm::vec3{0.0f, 10.0f, 0.0f}});
			if (WorkloadValue == Workload::ImportanceMixed && RelevantCount != 0)
				(void)Manager.BindControl(Connection, Characters.front()->GetObjectId(), 1);
			(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
		}
		if (ImportanceWorkload || WorkloadValue == Workload::Moving) {
			for (std::uint64_t Tick = 1; Tick <= 18; ++Tick) {
				for (std::size_t Index = 0; Index < RelevantCount; ++Index) {
					auto Transform = Characters[Index]->GetCFrame();
					Transform.Position.x += 0.1f;
					Characters[Index]->ApplyRuntimeTransform(Transform);
					Characters[Index]->ApplyRuntimeControllerFacts({6.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, true);
				}
				Manager.Step(World, Tick);
				(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
			}
			FirstMeasurementTick = 19;
			Transport.ResetMeasurements();
		}
		const auto MetricsBefore = Manager.GetMetrics();

		std::vector<double> StepSamples;
		StepSamples.reserve(SimulationTicks);
		std::uint64_t Allocations = 0;
		for (std::size_t Tick = 1; Tick <= SimulationTicks; ++Tick) {
			const auto SimulationTick = FirstMeasurementTick + Tick - 1;
			std::size_t MovingCount = 0;
			if (WorkloadValue == Workload::Moving || ImportanceWorkload)
				MovingCount = RelevantCount;
			else if (WorkloadValue == Workload::Mixed)
				MovingCount = (RelevantCount + 4) / 5;
			else if (WorkloadValue == Workload::Actions)
				MovingCount = RelevantCount;
			if (WorkloadValue == Workload::ImportanceSemanticPromotion && Tick == 1)
				for (std::size_t Index = 0; Index < RelevantCount; ++Index)
					(void)Manager.StartServerAction(Characters[Index]->GetObjectId(), 1, SimulationTick);
			for (std::size_t Index = 0; Index < MovingCount; ++Index) {
				auto Transform = Characters[Index]->GetCFrame();
				Transform.Position.x += 0.1f;
				Characters[Index]->ApplyRuntimeTransform(Transform);
				Characters[Index]->ApplyRuntimeControllerFacts({6.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, true);
			}
			const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			Manager.Step(World, SimulationTick);
			const auto Ended = std::chrono::steady_clock::now();
			(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
			Allocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			StepSamples.push_back(Microseconds(Started, Ended));
		}

		double StepMean = 0.0;
		for (const auto Sample : StepSamples)
			StepMean += Sample;
		StepMean /= static_cast<double>(StepSamples.size());
		const auto Metrics = Manager.GetMetrics();
		const auto Delta = [&Metrics, &MetricsBefore](auto Member) { return Metrics.*Member - MetricsBefore.*Member; };
		const auto DurationSeconds = static_cast<double>(SimulationTicks) /
									 static_cast<double>(DefaultCharacterSimulationTicksPerSecond);
		const auto BytesPerSecond = static_cast<double>(Transport.Bytes) / DurationSeconds;
		const auto MessagesPerSecond = static_cast<double>(Transport.Messages) / DurationSeconds;
		const auto States = Delta(&CharacterNetworkMetrics::AbsoluteStatesSent);
		const auto StateFrames = Delta(&CharacterNetworkMetrics::StateFramesEmitted);
		const auto SemanticBytes = Delta(&CharacterNetworkMetrics::SemanticStateBytes);
		const auto CompactBytes = Delta(&CharacterNetworkMetrics::CompactStateBytes);
		const auto StateAgeSamples = Delta(&CharacterNetworkMetrics::StateAgeSamples);
		const auto FullRateStates = Delta(&CharacterNetworkMetrics::FullRateStatesSent);
		const auto ReducedRateStates = Delta(&CharacterNetworkMetrics::ReducedRateStatesSent);
		const auto LowRateStates = Delta(&CharacterNetworkMetrics::LowRateStatesSent);
		const auto StatesPerSecond = static_cast<double>(States) / DurationSeconds;
		const auto AverageStatesPerBatch = StateFrames == 0
											   ? 0.0
											   : static_cast<double>(Delta(&CharacterNetworkMetrics::StatesInFrames)) /
													 static_cast<double>(StateFrames);
		const auto Reduction = SemanticBytes == 0 ? 0.0
												  : 100.0 * (1.0 - static_cast<double>(CompactBytes) /
																	   static_cast<double>(SemanticBytes));
		const auto PublicationTicks = Cadence * DurationSeconds;
		std::cout << "Transport," << WorkloadName(WorkloadValue) << ',' << CharacterCount << ',' << RelevantCount << ','
				  << Cadence << ',' << SimulationTicks << ',' << Transport.Bytes << ',' << BytesPerSecond << ','
				  << Transport.Messages << ',' << MessagesPerSecond << ',' << BytesPerSecond + 3600.0 << ','
				  << MessagesPerSecond + 60.0 << ',' << States << ',' << StatesPerSecond << ','
				  << Delta(&CharacterNetworkMetrics::StatesConsidered) << ','
				  << Delta(&CharacterNetworkMetrics::StatesSuppressedUnchanged) << ',' << StateFrames << ','
				  << AverageStatesPerBatch << ',' << Delta(&CharacterNetworkMetrics::BatchSplits) << ','
				  << SemanticBytes << ',' << CompactBytes << ',' << Reduction << ',' << StepMean << ','
				  << Percentile(StepSamples, 0.95) << ',' << Percentile(StepSamples, 0.99) << ','
				  << Delta(&CharacterNetworkMetrics::StateChangeDetectionCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::StateFrameAssemblyCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::StateEncodeCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::SchedulerSubmitCpuNanoseconds) << ',' << Allocations << ','
				  << (PublicationTicks == 0.0 ? 0.0 : Allocations / PublicationTicks) << ','
				  << Delta(&CharacterNetworkMetrics::ImportanceEvaluations) << ','
				  << Delta(&CharacterNetworkMetrics::ImportanceTierTransitions) << ','
				  << Delta(&CharacterNetworkMetrics::TemporaryPromotions) << ','
				  << Delta(&CharacterNetworkMetrics::ForcedSemanticPublications) << ',' << FullRateStates << ','
				  << ReducedRateStates << ',' << LowRateStates << ','
				  << Delta(&CharacterNetworkMetrics::FullRateStateBytes) << ','
				  << Delta(&CharacterNetworkMetrics::ReducedRateStateBytes) << ','
				  << Delta(&CharacterNetworkMetrics::LowRateStateBytes) << ','
				  << (StateAgeSamples == 0 ? 0.0
										   : static_cast<double>(Delta(&CharacterNetworkMetrics::StateAgeTicks)) /
												 static_cast<double>(StateAgeSamples))
				  << ',' << Metrics.MaximumStateAgeTicks << ',' << Metrics.FullRateRelationships << ','
				  << Metrics.ReducedRateRelationships << ',' << Metrics.LowRateRelationships << ','
				  << Delta(&CharacterNetworkMetrics::ImportanceEvaluationCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::DueSetCpuNanoseconds) << ','
				  << (FullRateStates == 0
						  ? 0.0
						  : static_cast<double>(Delta(&CharacterNetworkMetrics::FullRateStateAgeTicks)) /
								FullRateStates)
				  << ','
				  << (ReducedRateStates == 0
						  ? 0.0
						  : static_cast<double>(Delta(&CharacterNetworkMetrics::ReducedRateStateAgeTicks)) /
								ReducedRateStates)
				  << ','
				  << (LowRateStates == 0
						  ? 0.0
						  : static_cast<double>(Delta(&CharacterNetworkMetrics::LowRateStateAgeTicks)) / LowRateStates)
				  << ',' << Metrics.MaximumFullRateStateAgeTicks << ',' << Metrics.MaximumReducedRateStateAgeTicks
				  << ',' << Metrics.MaximumLowRateStateAgeTicks << '\n';
	}

	void RunPeerMatrix(
		std::size_t PeerCount, std::size_t CharacterCount, std::size_t RelevantPerPeer, std::size_t SimulationTicks
	) {
		MeasuringTransport Transport;
		NetworkScheduler Scheduler(Transport);
		const auto NetworkLimits = Limits();
		AuthoritativeCharacterNetwork Manager(Scheduler, NetworkLimits, Movement);
		WorldRoot World;
		std::vector<ConnectionId> Connections;
		Connections.reserve(PeerCount);
		for (std::size_t Index = 0; Index < PeerCount; ++Index) {
			const ConnectionId Connection{static_cast<std::uint32_t>(Index + 1), 1};
			Scheduler.RegisterConnection(Connection, NetworkLimits);
			Manager.AddPeer(Connection);
			Connections.push_back(Connection);
		}

		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		Characters.reserve(CharacterCount);
		constexpr float CircleRadius = 800.0f;
		constexpr float Tau = 6.28318530717958647692f;
		for (std::size_t Index = 0; Index < CharacterCount; ++Index) {
			const auto Angle = Tau * static_cast<float>(Index) / static_cast<float>(CharacterCount);
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition({CircleRadius * std::cos(Angle), 10.0f, CircleRadius * std::sin(Angle)});
			Manager.RegisterCharacter(Character);
			Characters.push_back(std::move(Character));
		}

		std::vector<std::int32_t> RelativeCharacters{0};
		for (std::int32_t Offset = 1; Offset <= 3; ++Offset) {
			RelativeCharacters.push_back(Offset);
			RelativeCharacters.push_back(-Offset);
		}
		for (std::int32_t Offset = 7; Offset <= 14; ++Offset) {
			RelativeCharacters.push_back(Offset);
			RelativeCharacters.push_back(-Offset);
		}
		for (std::int32_t Offset = 20; Offset <= 32; ++Offset) {
			RelativeCharacters.push_back(Offset);
			RelativeCharacters.push_back(-Offset);
		}
		RelativeCharacters.push_back(33);
		RelevantPerPeer = std::min(RelevantPerPeer, RelativeCharacters.size());
		for (std::size_t PeerIndex = 0; PeerIndex < PeerCount; ++PeerIndex) {
			const auto Connection = Connections[PeerIndex];
			const auto OwnerIndex = PeerIndex % CharacterCount;
			Manager.SetPeerPublicationFocus(Connection, std::array{Characters[OwnerIndex]->GetPosition()});
			for (std::size_t Relation = 0; Relation < RelevantPerPeer; ++Relation) {
				const auto Offset = RelativeCharacters[Relation];
				const auto CharacterIndex = static_cast<std::size_t>(
					(static_cast<std::int64_t>(OwnerIndex) + Offset + static_cast<std::int64_t>(CharacterCount)) %
					static_cast<std::int64_t>(CharacterCount)
				);
				Manager.MarkMaterialized(
					Connection, Characters[CharacterIndex]->GetObjectId(), StateChannelId(Relation + 1)
				);
			}
			(void)Manager.BindControl(Connection, Characters[OwnerIndex]->GetObjectId(), 1);
			(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
		}

		for (std::uint64_t Tick = 1; Tick <= 18; ++Tick) {
			for (const auto &Character : Characters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.y += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
				Character->ApplyRuntimeControllerFacts({0.0f, 0.6f, 0.0f}, {0.0f, 1.0f, 0.0f}, true);
			}
			Manager.Step(World, Tick);
			for (const auto Connection : Connections)
				(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
		}
		Transport.ResetMeasurements();
		const auto MetricsBefore = Manager.GetMetrics();
		std::vector<double> StepSamples;
		std::vector<double> StatesPerTick;
		StepSamples.reserve(SimulationTicks);
		StatesPerTick.reserve(SimulationTicks);
		std::uint64_t Allocations = 0;
		for (std::uint64_t Offset = 0; Offset < SimulationTicks; ++Offset) {
			const auto Tick = 19 + Offset;
			for (const auto &Character : Characters) {
				auto Transform = Character->GetCFrame();
				Transform.Position.y += 0.01f;
				Character->ApplyRuntimeTransform(Transform);
			}
			const auto Before = Manager.GetMetrics().AbsoluteStatesSent;
			const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			Manager.Step(World, Tick);
			const auto Ended = std::chrono::steady_clock::now();
			Allocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			const auto After = Manager.GetMetrics().AbsoluteStatesSent;
			StepSamples.push_back(Microseconds(Started, Ended));
			StatesPerTick.push_back(static_cast<double>(After - Before));
			for (const auto Connection : Connections)
				(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
		}
		const auto Metrics = Manager.GetMetrics();
		const auto Delta = [&Metrics, &MetricsBefore](auto Member) { return Metrics.*Member - MetricsBefore.*Member; };
		double MeanStep = 0.0;
		double MeanStates = 0.0;
		for (const auto Sample : StepSamples)
			MeanStep += Sample;
		for (const auto Sample : StatesPerTick)
			MeanStates += Sample;
		MeanStep /= static_cast<double>(StepSamples.size());
		MeanStates /= static_cast<double>(StatesPerTick.size());
		double StateVariance = 0.0;
		for (const auto Sample : StatesPerTick)
			StateVariance += (Sample - MeanStates) * (Sample - MeanStates);
		StateVariance /= static_cast<double>(StatesPerTick.size());
		const auto DurationSeconds = static_cast<double>(SimulationTicks) /
									 static_cast<double>(DefaultCharacterSimulationTicksPerSecond);
		std::cout << "PeerScale," << PeerCount << ',' << CharacterCount << ',' << RelevantPerPeer << ','
				  << PeerCount * RelevantPerPeer << ',' << SimulationTicks << ',' << Transport.Bytes / DurationSeconds
				  << ',' << Transport.Messages / DurationSeconds << ','
				  << Delta(&CharacterNetworkMetrics::AbsoluteStatesSent) / DurationSeconds << ','
				  << Delta(&CharacterNetworkMetrics::StateSnapshotsBuilt) / DurationSeconds << ','
				  << Delta(&CharacterNetworkMetrics::StateSnapshotRelationshipUses) / DurationSeconds << ',' << MeanStep
				  << ','
				  << Percentile(StepSamples, 0.95) << ',' << Percentile(StepSamples, 0.99) << ',' << MeanStates << ','
				  << *std::ranges::max_element(StatesPerTick) << ',' << Percentile(StatesPerTick, 0.95) << ','
				  << StateVariance << ',' << Delta(&CharacterNetworkMetrics::ImportanceEvaluations) / DurationSeconds
				  << ',' << Delta(&CharacterNetworkMetrics::ImportanceEvaluationCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::DueSetCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::StateChangeDetectionCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::StateFrameAssemblyCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::StateEncodeCpuNanoseconds) << ','
				  << Delta(&CharacterNetworkMetrics::SchedulerSubmitCpuNanoseconds) << ',' << Allocations << ','
				  << Delta(&CharacterNetworkMetrics::ImportanceTierTransitions) << ',' << Metrics.FullRateRelationships
				  << ',' << Metrics.ReducedRateRelationships << ',' << Metrics.LowRateRelationships << '\n';
	}

	void RunPresentationQuality(std::string_view Profile, auto StateAt, bool DropEveryFifthSample = false) {
		MeasuringTransport Transport;
		NetworkScheduler Scheduler(Transport);
		const auto NetworkLimits = Limits();
		const ConnectionId Connection{700, 1};
		Scheduler.RegisterConnection(Connection, NetworkLimits);
		PredictedCharacterNetwork Client(Scheduler, NetworkLimits, Movement);
		Client.AddPeer(Connection);
		const ObjectId Source{700, 1};
		auto Replica = std::make_shared<KinematicCharacter>();
		Client.MarkMaterialized(Source, Replica);
		WorldRoot World;
		std::uint64_t FrameSequence = 1;
		std::uint64_t StateSequence = 1;
		std::uint64_t LastSampleTick = 0;
		std::size_t SampleIndex = 0;
		std::vector<double> Errors;
		double SampleAgeTicks = 0.0;
		for (std::uint64_t Tick = 1; Tick <= 180; ++Tick) {
			if ((Tick - 1) % 12 == 0) {
				const bool Drop = DropEveryFifthSample && SampleIndex % 5 == 4;
				++SampleIndex;
				if (!Drop) {
					const auto [Position, Velocity] = StateAt(Tick);
					CharacterStateFrame Frame{
						.ServerTick = Tick,
						.FrameSequence = CharacterStateFrameSequence(FrameSequence),
						.MaterializationEpoch = CharacterMaterializationEpoch(1),
						.StateCount = 1,
					};
					Frame.States[0] = {
						.Character = Source,
						.ControlEpoch = CharacterControlEpoch(1),
						.StateSequence = RealtimeStateSequence(StateSequence),
						.AuthoritativeTick = Tick,
						.Transform = CFrame(Position),
						.Velocity = Velocity,
					};
					auto Encoded = EncodeCharacterMessage(CharacterMessage(Frame));
					if (!Encoded) throw std::runtime_error("[Character:NetworkBenchmark] quality state encode failed");
					if (!Client.HandleTransportEvent(TransportEvent(
							ReceivedMessageEvent{
								Connection,
								DeliveryMode::UnreliableSequenced,
								TrafficClass::RealtimeState,
								RealtimeStateOrder{StateChannelId(91), RealtimeStateSequence(FrameSequence)},
								std::move(*Encoded),
							}
						)))
						throw std::runtime_error("[Character:NetworkBenchmark] quality state was rejected");
					LastSampleTick = Tick;
					++FrameSequence;
					++StateSequence;
				}
			}
			Client.Reconcile(World);
			Client.UpdatePresentation(Tick);
			if (Tick < 25) continue;
			const auto TargetTick = Tick - DefaultRemoteInterpolationDelayTicks;
			const auto [ExpectedPosition, ExpectedVelocity] = StateAt(TargetTick);
			(void)ExpectedVelocity;
			Errors.push_back(glm::distance(Replica->GetPresentationCFrame().Position, ExpectedPosition));
			SampleAgeTicks += static_cast<double>(Tick - LastSampleTick);
		}
		double MeanError = 0.0;
		for (const auto Error : Errors)
			MeanError += Error;
		MeanError /= static_cast<double>(Errors.size());
		const auto Metrics = Client.GetMetrics();
		std::cout << "Quality," << Profile << ",5," << Errors.size() << ',' << MeanError << ','
				  << Percentile(Errors, 0.95) << ',' << *std::ranges::max_element(Errors) << ','
				  << SampleAgeTicks / static_cast<double>(Errors.size()) << ',' << Metrics.RemoteExtrapolations << ','
				  << Metrics.RemoteExtrapolationHolds << ',' << Metrics.InterpolationResets << ','
				  << Metrics.HardPresentationResets << '\n';
	}

	void RunPrediction(std::size_t Pending, std::size_t Iterations) {
		std::vector<double> Samples;
		Samples.reserve(Iterations);
		std::uint64_t ReplayCount = 0;
		std::uint64_t ReconcileAllocations = 0;
		for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration) {
			MeasuringTransport Transport;
			NetworkScheduler Scheduler(Transport);
			const ConnectionId Connection{2, 1};
			const auto NetworkLimits = Limits();
			Scheduler.RegisterConnection(Connection, NetworkLimits);
			PredictedCharacterNetwork Manager(Scheduler, NetworkLimits, Movement);
			Manager.AddPeer(Connection);
			WorldRoot World;
			auto Replica = std::make_shared<KinematicCharacter>();
			const ObjectId Source{500, 9};
			Manager.MarkMaterialized(Source, Replica);
			CharacterControlTransition Bind{Source, CharacterControlEpoch(3), StateChannelId(88), 1, true};
			auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
			(void)Manager.HandleTransportEvent(TransportEvent(
				ReceivedMessageEvent{
					Connection,
					DeliveryMode::ReliableOrdered,
					TrafficClass::Control,
					{},
					BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{},
				}
			));
			for (std::size_t Index = 0; Index < Pending; ++Index)
				(void)Manager.SubmitInput(Connection, World, Index + 2, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false);
			CharacterStateFrame Frame{
				.ServerTick = 1,
				.FrameSequence = CharacterStateFrameSequence(1),
				.StateCount = 1,
			};
			Frame.States[0] = {
				.Character = Source,
				.ControlEpoch = CharacterControlEpoch(3),
				.StateSequence = RealtimeStateSequence(1),
				.AuthoritativeTick = 1,
				.Transform = CFrame(0.0f, 6.0f, 0.0f),
			};
			auto StateBytes = EncodeCharacterMessage(CharacterMessage(Frame));
			(void)Manager.HandleTransportEvent(TransportEvent(
				ReceivedMessageEvent{
					Connection,
					DeliveryMode::UnreliableSequenced,
					TrafficClass::RealtimeState,
					RealtimeStateOrder{StateChannelId(88), RealtimeStateSequence(1)},
					StateBytes ? std::move(*StateBytes) : std::vector<std::byte>{},
				}
			));
			const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			Manager.Reconcile(World);
			const auto Ended = std::chrono::steady_clock::now();
			ReconcileAllocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			Samples.push_back(Microseconds(Started, Ended));
			ReplayCount += Manager.GetMetrics().PredictedCommandsReplayed;
		}
		double Mean = 0.0;
		for (const auto Sample : Samples)
			Mean += Sample;
		Mean /= static_cast<double>(Samples.size());
		std::cout << "Prediction,Pending" << Pending << ",0,0,0," << Iterations << ",0,0,0,0,0,0," << ReplayCount
				  << ",0,0,0,0,0,0,0,0,0," << Mean << ',' << Percentile(Samples, 0.95) << ','
				  << Percentile(Samples, 0.99) << ",0,0,0,0," << ReconcileAllocations
				  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
	}

	void RunBaselines() {
		for (const auto Count : {1u, 32u, 100u, 500u})
			std::cout << "Baseline3B,Moving," << Count << ',' << Count << ",60,60," << 112ull * 60ull * Count << ','
					  << 112ull * 60ull * Count << ',' << 60ull * Count << ',' << 60ull * Count << ','
					  << 112ull * 60ull * Count + 3600ull << ',' << 60ull * Count + 60ull
					  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		std::cout
			<< "Kind,Workload,Characters,Relevant,CadenceHz,SimulationTicks,PayloadBytes,PayloadBytesPerSecond,"
			   "SchedulerMessages,SchedulerMessagesPerSecond,BytesPerSecondIncludingInput,"
			   "MessagesPerSecondIncludingInput,States,StatesPerSecond,Considered,Suppressed,Batches,"
			   "AverageStatesPerBatch,BatchSplits,SemanticBytes,CompactBytes,CompactReductionPercent,"
			   "StepOrReconcileMeanUs,P95Us,P99Us,ChangeDetectionNs,AssemblyNs,EncodeNs,SchedulerNs,"
			   "Allocations,AllocationsPerPublicationTick,ImportanceEvaluations,TierTransitions,"
			   "TemporaryPromotions,ForcedSemanticPublications,FullRateStates,ReducedRateStates,LowRateStates,"
			   "FullRateStateBytes,ReducedRateStateBytes,LowRateStateBytes,MeanStateAgeTicks,MaximumStateAgeTicks,"
			   "FullRateRelationships,ReducedRateRelationships,LowRateRelationships,ImportanceEvaluationNs,DueSetNs,"
			   "FullRateMeanAgeTicks,ReducedRateMeanAgeTicks,LowRateMeanAgeTicks,MaximumFullRateAgeTicks,"
			   "MaximumReducedRateAgeTicks,MaximumLowRateAgeTicks\n";
		RunBaselines();
		if (Full) {
			for (const auto Cadence : {15u, 20u, 30u, 60u})
				for (const auto Count : {1u, 32u, 100u, 500u})
					for (const auto WorkloadValue :
						 {Workload::Stationary, Workload::Moving, Workload::Mixed, Workload::Actions})
						RunTransport(Count, Count, Cadence, WorkloadValue, 120);
			RunTransport(500, 50, 20, Workload::Moving, 120);
			RunTransport(32, 32, 20, Workload::ImportanceMixed, 120);
			RunTransport(500, 500, 20, Workload::ImportanceMixed, 120);
			RunTransport(500, 50, 20, Workload::ImportanceMixed, 120);
			RunTransport(500, 500, 20, Workload::ImportanceReduced, 120);
			RunTransport(500, 500, 20, Workload::ImportanceLow, 120);
			RunTransport(500, 500, 20, Workload::ImportanceSemanticPromotion, 120);
			std::cout
				<< "PeerScale,Peers,Characters,RelevantPerPeer,Relationships,SimulationTicks,PayloadBytesPerSecond,"
				   "MessagesPerSecond,StatesPerSecond,SnapshotsBuiltPerSecond,SnapshotRelationshipUsesPerSecond,"
				   "StepMeanUs,P95Us,P99Us,MeanStatesPerTick,MaxStatesPerTick,"
				   "P95StatesPerTick,StateCountVariance,ImportanceEvaluationsPerSecond,ImportanceNs,DueSetNs,"
				   "ChangeDetectionNs,AssemblyNs,EncodeNs,SchedulerNs,Allocations,TierTransitions,"
				   "FullRateRelationships,ReducedRateRelationships,LowRateRelationships\n";
			RunPeerMatrix(0, 500, 0, 60);
			RunPeerMatrix(1, 500, 50, 60);
			RunPeerMatrix(32, 500, 50, 60);
			RunPeerMatrix(100, 500, 50, 60);
			RunPeerMatrix(500, 500, 50, 60);
			std::cout << "Quality,Profile,CadenceHz,MeasuredTicks,MeanPositionError,P95PositionError,"
						 "MaximumPositionError,MeanArrivalSampleAgeTicks,Extrapolations,Holds,InterpolationResets,"
						 "HardPresentationResets\n";
			const auto ConstantMotion = [](std::uint64_t Tick) {
				const auto Seconds = static_cast<float>(Tick) / 60.0f;
				return std::pair{glm::vec3{6.0f * Seconds, 6.0f, 0.0f}, glm::vec3{6.0f, 0.0f, 0.0f}};
			};
			RunPresentationQuality("Constant6", ConstantMotion);
			RunPresentationQuality("AccelerationStopReverse", [](std::uint64_t Tick) {
				const auto Time = static_cast<float>(Tick) / 60.0f;
				float X = 0.0f;
				float Velocity = 0.0f;
				if (Time <= 1.0f) {
					X = 3.0f * Time * Time;
					Velocity = 6.0f * Time;
				} else if (Time <= 1.5f) {
					X = 3.0f + 6.0f * (Time - 1.0f);
					Velocity = 6.0f;
				} else if (Time <= 2.0f) {
					const auto Delta = Time - 1.5f;
					X = 6.0f + 6.0f * Delta - 6.0f * Delta * Delta;
					Velocity = 6.0f - 12.0f * Delta;
				} else {
					X = 7.5f - 6.0f * (Time - 2.0f);
					Velocity = -6.0f;
				}
				return std::pair{glm::vec3{X, 6.0f, 0.0f}, glm::vec3{Velocity, 0.0f, 0.0f}};
			});
			RunPresentationQuality("JumpGravity", [](std::uint64_t Tick) {
				const auto Time = static_cast<float>(Tick) / 60.0f;
				const auto AirTime = 16.0f / 9.8f;
				const auto Height = Time < AirTime ? 6.0f + 8.0f * Time - 4.9f * Time * Time : 6.0f;
				const auto VerticalVelocity = Time < AirTime ? 8.0f - 9.8f * Time : 0.0f;
				return std::pair{glm::vec3{2.0f * Time, Height, 0.0f}, glm::vec3{2.0f, VerticalVelocity, 0.0f}};
			});
			RunPresentationQuality("Constant120", [](std::uint64_t Tick) {
				const auto Seconds = static_cast<float>(Tick) / 60.0f;
				return std::pair{glm::vec3{120.0f * Seconds, 6.0f, 0.0f}, glm::vec3{120.0f, 0.0f, 0.0f}};
			});
			RunPresentationQuality("Constant6Loss20Percent", ConstantMotion, true);
			for (const auto Pending : {0u, 2u, 4u, 8u, static_cast<unsigned>(MaximumCharacterPredictionHistory)})
				RunPrediction(Pending, 120);
		} else {
			for (const auto Count : {1u, 32u})
				for (const auto WorkloadValue : {Workload::Stationary, Workload::Moving, Workload::Actions})
					RunTransport(Count, Count, 20, WorkloadValue, 12);
			RunTransport(32, 32, 20, Workload::ImportanceMixed, 12);
			RunPrediction(4, 3);
		}
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Character:NetworkBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
