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
			.DurationTicks = 120,
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

	void RunServer(std::size_t Count, std::size_t ActiveCount, std::size_t Iterations) {
		MeasuringTransport Transport;
		NetworkScheduler Scheduler(Transport);
		const ConnectionId Connection{1, 1};
		const auto NetworkLimits = Limits();
		Scheduler.RegisterConnection(Connection, NetworkLimits);
		AuthoritativeCharacterNetwork Manager(Scheduler, NetworkLimits, Movement);
		Manager.AddPeer(Connection);
		Manager.RegisterAction(Action());
		WorldRoot World;
		std::vector<std::shared_ptr<KinematicCharacter>> Characters;
		std::vector<CharacterControlEpoch> Epochs;
		Characters.reserve(Count);
		Epochs.reserve(ActiveCount);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetPosition(
				{static_cast<float>(Index % 25) * 4.0f, 10.0f, static_cast<float>(Index / 25) * 4.0f}
			);
			Manager.RegisterCharacter(Character);
			if (Index < ActiveCount) {
				Manager.MarkMaterialized(Connection, Character->GetObjectId(), StateChannelId(Index + 1));
				auto Epoch = Manager.BindControl(Connection, Character->GetObjectId(), 1);
				Epochs.push_back(Epoch.value_or(CharacterControlEpoch{}));
				Manager.StartServerAction(Character->GetObjectId(), 1, 1);
			}
			Characters.push_back(std::move(Character));
		}
		(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
		const auto Before = Manager.GetMetrics();
		double AdmissionUs = 0.0;
		double StepUs = 0.0;
		std::vector<double> StepSamples;
		StepSamples.reserve(Iterations);
		std::uint64_t AdmissionAllocations = 0;
		std::uint64_t StepAllocations = 0;
		for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration) {
			const auto Tick = Iteration + 2;
			const auto AdmissionStart = std::chrono::steady_clock::now();
			for (std::size_t Index = 0; Index < ActiveCount; ++Index) {
				CharacterInputCommand Command{
					Characters[Index]->GetObjectId(),
					Epochs[Index],
					CharacterInputSequence(Iteration + 1),
					Tick,
					1.0f / 60.0f,
					{1.0f, 0.0f},
					0.0f,
					0
				};
				auto Bytes = EncodeCharacterMessage(CharacterMessage(Command));
				ReceivedMessageEvent Event{
					Connection,
					DeliveryMode::UnreliableSequenced,
					TrafficClass::RealtimeState,
					RealtimeStateOrder{StateChannelId(Index + 1), RealtimeStateSequence(Iteration + 1)},
					Bytes ? std::move(*Bytes) : std::vector<std::byte>{}
				};
				const auto AllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
				(void)Manager.HandleTransportEvent(TransportEvent(std::move(Event)));
				AdmissionAllocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) -
										AllocationsBefore;
			}
			const auto AdmissionEnd = std::chrono::steady_clock::now();
			const auto StepAllocationsBefore = CharacterBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto StepStart = std::chrono::steady_clock::now();
			Manager.Step(World, Tick);
			const auto StepEnd = std::chrono::steady_clock::now();
			StepAllocations += CharacterBenchmarkAllocations.load(std::memory_order_relaxed) - StepAllocationsBefore;
			(void)Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(NetworkLimits));
			AdmissionUs += Microseconds(AdmissionStart, AdmissionEnd);
			const auto Sample = Microseconds(StepStart, StepEnd);
			StepUs += Sample;
			StepSamples.push_back(Sample);
		}
		const auto After = Manager.GetMetrics();
		std::cout << "Server," << Count << ',' << ActiveCount << ',' << Iterations << ',' << AdmissionUs / Iterations
				  << ',' << StepUs / Iterations << ',' << Percentile(StepSamples, 0.95) << ','
				  << Percentile(StepSamples, 0.99) << ','
				  << (After.MovementCpuNanoseconds - Before.MovementCpuNanoseconds) / Iterations << ','
				  << (After.RootMotionCpuNanoseconds - Before.RootMotionCpuNanoseconds) / Iterations << ','
				  << (After.StateEncodeCpuNanoseconds - Before.StateEncodeCpuNanoseconds) / Iterations << ','
				  << (After.SchedulerSubmitCpuNanoseconds - Before.SchedulerSubmitCpuNanoseconds) / Iterations << ','
				  << Transport.Bytes << ',' << Transport.Messages << ',' << AdmissionAllocations << ','
				  << StepAllocations << '\n';
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
			Manager.RegisterAction(Action());
			WorldRoot World;
			auto Replica = std::make_shared<KinematicCharacter>();
			const ObjectId Source{500, 9};
			Manager.MarkMaterialized(Source, Replica);
			CharacterControlTransition Bind{Source, CharacterControlEpoch(3), StateChannelId(88), 1, true};
			auto BindBytes = EncodeCharacterMessage(CharacterMessage(Bind));
			ReceivedMessageEvent BindEvent{
				Connection,
				DeliveryMode::ReliableOrdered,
				TrafficClass::Control,
				{},
				BindBytes ? std::move(*BindBytes) : std::vector<std::byte>{}
			};
			(void)Manager.HandleTransportEvent(TransportEvent(std::move(BindEvent)));
			for (std::size_t Index = 0; Index < Pending; ++Index)
				(void)Manager.SubmitInput(Connection, World, Index + 2, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false);
			CharacterAuthoritativeState State{
				.Character = Source,
				.ControlEpoch = CharacterControlEpoch(3),
				.StateSequence = RealtimeStateSequence(1),
				.AuthoritativeTick = 1,
				.Transform = CFrame(0.0f, 6.0f, 0.0f)
			};
			auto StateBytes = EncodeCharacterMessage(CharacterMessage(State));
			ReceivedMessageEvent StateEvent{
				Connection,
				DeliveryMode::UnreliableSequenced,
				TrafficClass::RealtimeState,
				RealtimeStateOrder{StateChannelId(88), RealtimeStateSequence(1)},
				StateBytes ? std::move(*StateBytes) : std::vector<std::byte>{}
			};
			(void)Manager.HandleTransportEvent(TransportEvent(std::move(StateEvent)));
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
		std::cout << "Prediction," << Pending << ",0," << Iterations << ",0," << Mean << ','
				  << Percentile(Samples, 0.95) << ',' << Percentile(Samples, 0.99) << ",0,0,0,0," << ReplayCount << ','
				  << MaximumCharacterPredictionHistory << ",0," << ReconcileAllocations << '\n';
	}

	void RunBandwidth() {
		const ObjectId Character{1, 1};
		CharacterInputCommand Input{
			Character, CharacterControlEpoch(1), CharacterInputSequence(1), 1, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, 0
		};
		CharacterAuthoritativeState State{
			.Character = Character,
			.ControlEpoch = CharacterControlEpoch(1),
			.StateSequence = RealtimeStateSequence(1),
			.AuthoritativeTick = 1,
			.Transform = CFrame()
		};
		CharacterControlTransition Bind{Character, CharacterControlEpoch(1), StateChannelId(1), 1, true};
		CharacterActionRequest Request{Character, CharacterControlEpoch(1), CharacterActionSequence(1), {}, 1};
		const auto InputBytes = EncodeCharacterMessage(CharacterMessage(Input))->size();
		const auto StateBytes = EncodeCharacterMessage(CharacterMessage(State))->size();
		const auto ControlBytes = EncodeCharacterMessage(CharacterMessage(Bind))->size();
		const auto ActionBytes = EncodeCharacterMessage(CharacterMessage(Request))->size();
		for (const auto Count : {1u, 32u, 100u, 500u})
			std::cout << "Bandwidth," << Count << ",0,60," << InputBytes * 60ull << ',' << StateBytes * 60ull * Count
					  << ",0,0,0,0,0,0," << ControlBytes + ActionBytes << ',' << 60ull * (Count + 1) << ",0,0\n";
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		const auto Iterations = Full ? 120u : 3u;
		std::cout << "Scenario,Characters,Active,Iterations,AdmissionMeanUs,StepOrReconcileMeanUs,"
					 "StepOrReconcileP95Us,StepOrReconcileP99Us,MovementNs,RootNs,EncodeNs,SchedulerNs,"
					 "BytesOrReplay,MessagesOrBound,AdmissionAllocations,StepOrReconcileAllocations\n";
		for (const auto Count : {1u, 10u, 100u, 500u})
			RunServer(Count, Count, Iterations);
		for (const auto Pending : {0u, 2u, 4u, 8u, static_cast<unsigned>(MaximumCharacterPredictionHistory)})
			RunPrediction(Pending, Iterations);
		RunServer(500, 50, Iterations);
		RunBandwidth();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Character:NetworkBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
