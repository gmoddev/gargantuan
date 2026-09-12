#pragma once
#include "../../network/GnsServiceDiagnostics.hpp"
#include "../../runtime/PublicationLatencyDiagnostics.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace gargantuan::host {
// Explicit trusted session-smoke opt-in only. Fixed header reads, no payload
// copies or graph decode. One bounded line/write per observation survives a
// harness terminating a failed host. Logging cost remains visible in host timing.
class TransportServiceSmoke final {
	using Publication = runtime_detail::PublicationLatencyRecord;
	using Backend = network::detail::GnsServiceRecord;
	static constexpr std::uint64_t MaximumRecords = 131072;
	std::uint64_t Records = 0, Dropped = 0;
	const char *Host;
	bool Enabled = false;
	runtime_detail::PublicationLatencySink PublicationSink{this, Selected, Append, Packet};
	network::detail::GnsServiceSink BackendSink{this, Observe};
	runtime_detail::PublicationLatencySink *PreviousPublication = nullptr;
	network::detail::GnsServiceSink *PreviousBackend = nullptr;
	static bool Selected(void *, network::ConnectionId) noexcept { return true; }
	static Publication Header(const char *Stage, network::ConnectionId Connection,
		std::span<const std::byte> Payload, std::uint64_t Time) noexcept {
		Publication Value{.Stage = Stage, .Connection = Connection, .Nanoseconds = Time,
			.Bytes = static_cast<std::uint32_t>(Payload.size())};
		auto Read = [&](std::size_t Offset, std::size_t Count) {
			std::uint64_t Result = 0;
			for (std::size_t I = 0; I < Count; ++I)
				Result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(Payload[Offset + I])) << (8 * I);
			return Result;
		};
		if (Payload.size() >= 52 && std::memcmp(Payload.data(), "GRMT", 4) == 0) {
			Value.Kind = 100 + static_cast<std::uint32_t>(Read(6, 1));
			Value.Object = {static_cast<std::uint32_t>(Read(8, 4)), static_cast<std::uint32_t>(Read(12, 4))};
			Value.Epoch = Read(16, 8); Value.Sequence = Read(24, 8);
			if (!Value.Sequence) Value.Sequence = Read(32, 8);
		} else if (Payload.size() >= 36 && std::memcmp(Payload.data(), "GRPL", 4) == 0) {
			Value.Kind = 200; Value.Epoch = Read(8, 8); Value.Sequence = Read(16, 8);
			Value.Operations = static_cast<std::uint32_t>(Read(28, 4));
		} else if (Payload.size() >= 8 && std::memcmp(Payload.data(), "GCHR", 4) == 0) {
			Value.Kind = static_cast<std::uint32_t>(Read(6, 1)) + 1;
		} else if (Payload.size() >= 4 && std::memcmp(Payload.data(), "GSES", 4) == 0) Value.Kind = 300;
		return Value;
	}
	void Write(Publication P, Backend B = {}) noexcept {
		if (Records >= MaximumRecords) {
			if (Dropped != std::numeric_limits<std::uint64_t>::max()) ++Dropped;
			return;
		}
		++Records;
		char Line[1024];
		const int Size = std::snprintf(Line, sizeof(Line),
			"[Network:Service] host=%s stage=%s ns=%llu peer=%u gen=%u kind=%u object=%u objectGen=%u id=%llu epoch=%llu bytes=%u ops=%u backendId=%lld receiveAgeUs=%lld queueUs=%lld pending=%d unacked=%d pendingUnreliable=%d rate=%d rateMin=%d rateMax=%d sendBuffer=%d ping=%d outBps=%.3f inBps=%.3f delivery=%d traffic=%d result=%d\n",
			Host, P.Stage, static_cast<unsigned long long>(P.Nanoseconds), P.Connection.Slot, P.Connection.Generation,
			P.Kind, P.Object.Slot, P.Object.Generation, static_cast<unsigned long long>(P.Sequence),
			static_cast<unsigned long long>(P.Epoch), P.Bytes, P.Operations,
			static_cast<long long>(B.MessageNumber), static_cast<long long>(B.ReceiveAgeUs), static_cast<long long>(B.QueueUs),
			B.PendingReliable, B.UnackedReliable, B.PendingUnreliable, B.Rate, B.RateMin, B.RateMax, B.SendBuffer,
			B.Ping, B.OutBytesPerSecond, B.InBytesPerSecond, B.Delivery, B.Traffic, B.Result);
		if (Size > 0 && static_cast<std::size_t>(Size) < sizeof(Line))
			(void)std::fwrite(Line, 1, static_cast<std::size_t>(Size), stdout);
	}
	static void Append(void *Context, Publication Value) noexcept {
		static_cast<TransportServiceSmoke *>(Context)->Write(Value);
	}
	static void Packet(void *Context, const char *Stage, network::ConnectionId Connection,
		std::span<const std::byte> Payload, std::uint64_t Time) noexcept {
		static_cast<TransportServiceSmoke *>(Context)->Write(Header(Stage, Connection, Payload, Time));
	}
	static void Observe(void *Context, Backend Value, std::span<const std::byte> Payload) noexcept {
		static_cast<TransportServiceSmoke *>(Context)->Write(
			Header(Value.Stage, Value.Connection, Payload, Value.Nanoseconds), Value);
	}
public:
	TransportServiceSmoke(bool SessionSmoke, const char *Host) noexcept : Host(Host) {
		const auto *Setting = SessionSmoke ? std::getenv("GARGANTUAN_TRANSPORT_SERVICE_TRACE") : nullptr;
		Enabled = Setting && std::strcmp(Setting, "1") == 0;
		if (!Enabled) return;
		PreviousPublication = runtime_detail::ActivePublicationLatency;
		PreviousBackend = network::detail::ActiveGnsService;
		runtime_detail::ActivePublicationLatency = &PublicationSink;
		network::detail::ActiveGnsService = &BackendSink;
		std::printf("[Network:ServiceLimit] host=%s cap=%llu format=1 unavailable=-1\n", Host,
			static_cast<unsigned long long>(MaximumRecords));
	}
	~TransportServiceSmoke() {
		if (!Enabled) return;
		runtime_detail::ActivePublicationLatency = PreviousPublication;
		network::detail::ActiveGnsService = PreviousBackend;
		std::printf("[Network:ServiceEnd] host=%s records=%llu dropped=%llu\n", Host,
			static_cast<unsigned long long>(Records), static_cast<unsigned long long>(Dropped));
	}
	TransportServiceSmoke(const TransportServiceSmoke &) = delete;
	TransportServiceSmoke &operator=(const TransportServiceSmoke &) = delete;
};
}
