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

	enum class Workload : std::uint8_t { Stationary, Moving, Mixed, Actions };

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
		for (std::size_t Index = 0; Index < CharacterCount; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition(
				{static_cast<float>(Index % 25) * 4.0f, 10.0f, static_cast<float>(Index / 25) * 4.0f}
			);
			Manager.RegisterCharacter(Character);
			if (Index < RelevantCount)
				Manager.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(Index + 1));
			if (WorkloadValue == Workload::Actions && Index < RelevantCount)
				Manager.StartServerAction(Character->GetObjectId(), 1, 1);
			Characters.push_back(std::move(Character));
		}

		std::vector<double> StepSamples;
		StepSamples.reserve(SimulationTicks);
		std::uint64_t Allocations = 0;
		for (std::size_t Tick = 1; Tick <= SimulationTicks; ++Tick) {
			std::size_t MovingCount = 0;
			if (WorkloadValue == Workload::Moving)
				MovingCount = RelevantCount;
			else if (WorkloadValue == Workload::Mixed)
				MovingCount = (RelevantCount + 4) / 5;
			else if (WorkloadValue == Workload::Actions)
				MovingCount = RelevantCount;
			for (std::size_t Index = 0; Index < MovingCount; ++Index) {
				auto Transform = Characters[Index]->GetCFrame();
				Transform.Position.x += 0.1f;
				Characters[Index]->ApplyRuntimeTransform(Transform);
				Characters[Index]->ApplyRuntimeControllerFacts({6.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, true);
			}
			const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			Manager.Step(World, Tick);
			const auto Ended = std::chrono::steady_clock::now();
			(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
			Allocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			StepSamples.push_back(Microseconds(Started, Ended));
		}

		double StepMean = 0.0;
		for (const auto Sample : StepSamples) StepMean += Sample;
		StepMean /= static_cast<double>(StepSamples.size());
		const auto Metrics = Manager.GetMetrics();
		const auto DurationSeconds = static_cast<double>(SimulationTicks) /
			static_cast<double>(DefaultCharacterSimulationTicksPerSecond);
		const auto BytesPerSecond = static_cast<double>(Transport.Bytes) / DurationSeconds;
		const auto MessagesPerSecond = static_cast<double>(Transport.Messages) / DurationSeconds;
		const auto StatesPerSecond = static_cast<double>(Metrics.AbsoluteStatesSent) / DurationSeconds;
		const auto AverageStatesPerBatch = Metrics.StateFramesEmitted == 0 ? 0.0
			: static_cast<double>(Metrics.StatesInFrames) / static_cast<double>(Metrics.StateFramesEmitted);
		const auto Reduction = Metrics.SemanticStateBytes == 0 ? 0.0
			: 100.0 * (1.0 - static_cast<double>(Metrics.CompactStateBytes) /
				static_cast<double>(Metrics.SemanticStateBytes));
		const auto PublicationTicks = Cadence * DurationSeconds;
		std::cout << "Transport," << WorkloadName(WorkloadValue) << ',' << CharacterCount << ',' << RelevantCount << ','
				  << Cadence << ',' << SimulationTicks << ',' << Transport.Bytes << ',' << BytesPerSecond << ','
				  << Transport.Messages << ',' << MessagesPerSecond << ',' << BytesPerSecond + 3600.0 << ','
				  << MessagesPerSecond + 60.0 << ',' << Metrics.AbsoluteStatesSent << ',' << StatesPerSecond << ','
				  << Metrics.StatesConsidered << ',' << Metrics.StatesSuppressedUnchanged << ','
				  << Metrics.StateFramesEmitted << ',' << AverageStatesPerBatch << ',' << Metrics.BatchSplits << ','
				  << Metrics.SemanticStateBytes << ',' << Metrics.CompactStateBytes << ',' << Reduction << ',' << StepMean
				  << ',' << Percentile(StepSamples, 0.95) << ',' << Percentile(StepSamples, 0.99) << ','
				  << Metrics.StateChangeDetectionCpuNanoseconds << ',' << Metrics.StateFrameAssemblyCpuNanoseconds << ','
				  << Metrics.StateEncodeCpuNanoseconds << ',' << Metrics.SchedulerSubmitCpuNanoseconds << ','
				  << Allocations << ',' << (PublicationTicks == 0.0 ? 0.0 : Allocations / PublicationTicks) << '\n';
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
			(void)Manager.HandleTransportEvent(TransportEvent(ReceivedMessageEvent{
				Connection,
				DeliveryMode::ReliableOrdered,
				TrafficClass::Control,
				{},
				BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{},
			}));
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
			(void)Manager.HandleTransportEvent(TransportEvent(ReceivedMessageEvent{
				Connection,
				DeliveryMode::UnreliableSequenced,
				TrafficClass::RealtimeState,
				RealtimeStateOrder{StateChannelId(88), RealtimeStateSequence(1)},
				StateBytes ? std::move(*StateBytes) : std::vector<std::byte>{},
			}));
			const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			Manager.Reconcile(World);
			const auto Ended = std::chrono::steady_clock::now();
			ReconcileAllocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			Samples.push_back(Microseconds(Started, Ended));
			ReplayCount += Manager.GetMetrics().PredictedCommandsReplayed;
		}
		double Mean = 0.0;
		for (const auto Sample : Samples) Mean += Sample;
		Mean /= static_cast<double>(Samples.size());
		std::cout << "Prediction,Pending" << Pending << ",0,0,0," << Iterations << ",0,0,0,0,0,0,"
				  << ReplayCount << ",0,0,0,0,0,0,0,0,0," << Mean << ',' << Percentile(Samples, 0.95) << ','
				  << Percentile(Samples, 0.99) << ",0,0,0,0," << ReconcileAllocations << ",0\n";
	}

	void RunBaselines() {
		for (const auto Count : {1u, 32u, 100u, 500u})
			std::cout << "Baseline3B,Moving," << Count << ',' << Count << ",60,60," << 112ull * 60ull * Count
					  << ',' << 112ull * 60ull * Count << ',' << 60ull * Count << ',' << 60ull * Count << ','
					  << 112ull * 60ull * Count + 3600ull << ',' << 60ull * Count + 60ull
					  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n";
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		std::cout << "Kind,Workload,Characters,Relevant,CadenceHz,SimulationTicks,PayloadBytes,PayloadBytesPerSecond,"
					 "SchedulerMessages,SchedulerMessagesPerSecond,BytesPerSecondIncludingInput,"
					 "MessagesPerSecondIncludingInput,States,StatesPerSecond,Considered,Suppressed,Batches,"
					 "AverageStatesPerBatch,BatchSplits,SemanticBytes,CompactBytes,CompactReductionPercent,"
					 "StepOrReconcileMeanUs,P95Us,P99Us,ChangeDetectionNs,AssemblyNs,EncodeNs,SchedulerNs,"
					 "Allocations,AllocationsPerPublicationTick\n";
		RunBaselines();
		if (Full) {
			for (const auto Cadence : {15u, 20u, 30u, 60u})
				for (const auto Count : {1u, 32u, 100u, 500u})
					for (const auto WorkloadValue :
						 {Workload::Stationary, Workload::Moving, Workload::Mixed, Workload::Actions})
						RunTransport(Count, Count, Cadence, WorkloadValue, 120);
			RunTransport(500, 50, 20, Workload::Moving, 120);
			for (const auto Pending : {0u, 2u, 4u, 8u, static_cast<unsigned>(MaximumCharacterPredictionHistory)})
				RunPrediction(Pending, 120);
		} else {
			for (const auto Count : {1u, 32u})
				for (const auto WorkloadValue : {Workload::Stationary, Workload::Moving, Workload::Actions})
					RunTransport(Count, Count, 20, WorkloadValue, 12);
			RunPrediction(4, 3);
		}
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Character:NetworkBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
