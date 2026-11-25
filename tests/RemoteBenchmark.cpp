#include "gargantuan/network/RemoteManager.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
	thread_local bool TrackCodecAllocations = false;
	thread_local std::uint64_t CodecAllocations = 0;
	thread_local std::uint64_t CodecAllocationBytes = 0;
}

void *operator new(std::size_t Bytes) {
	if (TrackCodecAllocations) {
		++CodecAllocations;
		CodecAllocationBytes += Bytes;
	}
	if (auto *Value = std::malloc(Bytes == 0 ? 1 : Bytes)) return Value;
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept { std::free(Value); }
void operator delete(void *Value, std::size_t) noexcept { std::free(Value); }

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	struct MeasuringScheduler final : INetworkScheduler {
		std::uint64_t Messages = 0;
		std::uint64_t Bytes = 0;

		bool RegisterConnection(ConnectionId, const NetworkLimits &) override {
			return true;
		}
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			++Messages;
			Bytes += Intent.Payload().size();
			return {SchedulerSubmitStatus::Accepted};
		}
		SchedulerFlushResult Flush(ConnectionId, SchedulerTickBudget) override {
			return {SchedulerFlushStatus::Drained};
		}
		bool CancelConnection(ConnectionId) override {
			return true;
		}
		std::optional<SchedulerStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
	};

	NetworkLimits Limits() {
		return {
			.MaximumReliableMessageBytes = MaximumRemoteFrameBytes,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 4 * 1024 * 1024,
			.MaximumInFlightRemoteRequests = 64,
			.MaximumDecodedMessageBytes = MaximumRemoteFrameBytes,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 4096,
		};
	}

	template <typename Work> double Measure(std::size_t Iterations, Work &&Callback) {
		const auto Start = std::chrono::steady_clock::now();
		for (std::size_t Index = 0; Index < Iterations; ++Index)
			Callback(Index);
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	constexpr std::size_t EventIterations = 50'000;
	constexpr std::size_t RequestIterations = 10'000;
	const ObjectId Reliable{1, 1};
	const ObjectId Unreliable{2, 1};
	const ObjectId Sequenced{3, 1};
	const ObjectId Function{4, 1};
	const ConnectionId Connection{1, 1};
	RemoteMessage CodecMessage{
		.Kind = RemoteMessageKind::ReliableEvent, .Remote = Reliable, .Arguments = {42, 3.5, std::string("small-event")}
	};
	std::vector<std::byte> Encoded;
	const double EncodeUs = Measure(EventIterations, [&](std::size_t) {
		auto Result = EncodeRemoteMessage(CodecMessage);
		if (!Result) std::abort();
		Encoded = std::move(*Result);
	});
	const double DecodeUs = Measure(EventIterations, [&](std::size_t) {
		if (!DecodeRemoteMessage(Encoded)) std::abort();
	});

	MeasuringScheduler Scheduler;
	auto Time = std::chrono::steady_clock::time_point{};
	RemoteManager Manager(
		RemoteManagerRole::Client,
		Scheduler,
		[](ConnectionId, ObjectId) { return true; },
		[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; },
		[&] { return Time; }
	);
	if (!Manager.AddPeer(Connection, ReplicationEpoch(1), Limits())) return 1;
	for (const auto [Remote, Kind] : std::array{
			 std::pair{Reliable, RemoteInstanceKind::ReliableEvent},
			 std::pair{Unreliable, RemoteInstanceKind::UnreliableEvent},
			 std::pair{Sequenced, RemoteInstanceKind::UnreliableSequencedEvent},
			 std::pair{Function, RemoteInstanceKind::Function}
		 }) {
		if (!Manager.RegisterRemote(Remote, Kind) || !Manager.PublishRemote(Connection, Remote)) return 1;
	}
	auto EventRun = [&](ObjectId Remote) {
		return Measure(EventIterations, [&](std::size_t Index) {
			if (Index != 0 && Index % MaximumRemoteCallsPerRemotePerSecond == 0) Time += 1s;
			if (!Manager.SendEvent(Connection, Remote, {static_cast<int>(Index), std::string("payload")}).Accepted())
				std::abort();
		});
	};
	const double ReliableUs = EventRun(Reliable);
	const double UnreliableUs = EventRun(Unreliable);
	const double SequencedUs = EventRun(Sequenced);

	std::size_t Completed = 0;
	const double RequestUs = Measure(RequestIterations, [&](std::size_t Index) {
		if (Index != 0 && Index % MaximumRemoteCallsPerRemotePerSecond == 0) Time += 1s;
		auto Started = Manager.StartRequest(
			Connection,
			Function,
			{static_cast<int>(Index)},
			[&](RemoteRequestResult Result) {
				if (Result.Outcome.Status == RemoteRequestTerminalStatus::Success) ++Completed;
			},
			1s
		);
		if (!Started.Accepted() || !Started.Request) std::abort();
		RemoteMessage Response{
			.Kind = RemoteMessageKind::Response,
			.Remote = Function,
			.Request = *Started.Request,
			.Arguments = {static_cast<int>(Index)}
		};
		auto Bytes = EncodeRemoteMessage(Response);
		if (!Bytes ||
			!Manager.HandleTransportEvent(
				ReceivedMessageEvent{
					Connection, DeliveryMode::ReliableOrdered, TrafficClass::ReliableApplication, {}, std::move(*Bytes)
				}
			))
			std::abort();
		Manager.Pump(1);
	});
	if (Completed != RequestIterations) return 1;

	RemoteArray Items;
	for (int Index = 0; Index < 24; ++Index)
		Items.Values.emplace_back(RemoteDictionary{{
			{"count", Index + 1}, {"item", std::string("inventory-item-") + std::to_string(Index)},
			{"position", WireVector3{1.25f, 2.5f, 5.0f}},
		}});
	const RemoteMessage Structured{
		.Kind = RemoteMessageKind::ReliableEvent,
		.Remote = Reliable,
		.Arguments = {RemoteDictionary{{
			{"items", std::move(Items)}, {"snapshot", RemoteBuffer{std::vector<std::byte>(2048, std::byte{0x80})}},
		}}},
	};
	constexpr std::size_t StructuredIterations = 10'000;
	std::vector<std::byte> StructuredFrame;
	const auto RunStructured = [&] {
		return Measure(StructuredIterations, [&](std::size_t) {
			auto EncodedFrame = EncodeRemoteMessage(Structured);
			if (!EncodedFrame) std::abort();
			StructuredFrame = std::move(*EncodedFrame);
			if (!DecodeRemoteMessage(StructuredFrame)) std::abort();
		});
	};
	const double StructuredUs = RunStructured();
	runtime_detail::WorkSample Work{};
	double StructuredCapturedUs;
	{
		runtime_detail::WorkCapture Capture(&Work, []() noexcept { return CodecAllocations; },
			[]() noexcept { return CodecAllocationBytes; });
		TrackCodecAllocations = true;
		StructuredCapturedUs = RunStructured();
		TrackCodecAllocations = false;
	}
	const auto &StructuredEncode = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::RemoteEncode)];
	const auto &StructuredDecode = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::RemoteDecode)];
	if (StructuredEncode.Calls != StructuredIterations || StructuredDecode.Calls != StructuredIterations) return 1;

	std::cout << "Remote benchmark\n"
			  << "codec encode: " << EncodeUs / EventIterations << " us/message\n"
			  << "codec decode: " << DecodeUs / EventIterations << " us/message\n"
			  << "reliable admission: " << ReliableUs / EventIterations << " us/message\n"
			  << "unreliable admission: " << UnreliableUs / EventIterations << " us/message\n"
			  << "sequenced admission: " << SequencedUs / EventIterations << " us/message\n"
			  << "request start/response/dispatch: " << RequestUs / RequestIterations << " us/request\n"
			  << "scheduler submissions: " << Scheduler.Messages << "\n"
			  << "output bytes: " << Scheduler.Bytes << "\n"
			  << "structured round trip: " << StructuredUs / StructuredIterations << " us/message\n"
			  << "structured round trip with capture: " << StructuredCapturedUs / StructuredIterations << " us/message\n"
			  << "structured encode: " << StructuredEncode.Nanoseconds / (1000.0 * StructuredIterations) << " us/message\n"
			  << "structured decode: " << StructuredDecode.Nanoseconds / (1000.0 * StructuredIterations) << " us/message\n"
			  << "structured wire bytes: " << StructuredEncode.Units / StructuredIterations << " bytes/message\n"
			  << "structured decoded logical bytes: " << StructuredDecode.Units / StructuredIterations << " bytes/message\n"
			  << "structured decode allocations: " << StructuredDecode.Allocations / StructuredIterations << " allocations/message\n"
			  << "structured decode requested bytes: " << StructuredDecode.AllocatedBytes / StructuredIterations << " bytes/message\n";
	return 0;
}
