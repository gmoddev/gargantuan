#pragma once
#include "../src/runtime/PublicationLatencyDiagnostics.hpp"
#include "gargantuan/network/CharacterProtocol.hpp"
#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace gargantuan::test {
struct RecipientGapHistogram {
	std::array<std::uint64_t, 4097> Buckets{};
	std::uint64_t Count = 0;
	double Maximum = 0;
	void Add(double Milliseconds) noexcept {
		if (Count == std::numeric_limits<std::uint64_t>::max()) return;
		const auto Bin = static_cast<std::size_t>(std::min(4096.0, std::ceil(std::max(0.0, Milliseconds))));
		++Buckets[Bin]; ++Count; Maximum = std::max(Maximum, Milliseconds);
	}
	double Upper(double Fraction) const noexcept {
		if (!Count) return -1;
		const auto Rank = static_cast<std::uint64_t>((Count - 1) * Fraction);
		std::uint64_t Seen = 0;
		for (std::size_t Index = 0; Index < Buckets.size(); ++Index) {
			Seen += Buckets[Index];
			if (Seen > Rank) return Index == 4096 ? -1 : static_cast<double>(Index);
		}
		return -1;
	}
};
inline thread_local std::array<RecipientGapHistogram, 2> *ActiveRecipientGaps = nullptr;

class PublicationLatencyFixture final {
	using Record = runtime_detail::PublicationLatencyRecord;
	struct Sample { Record Value; std::uint32_t Phase; };
	static constexpr std::size_t MaximumRecords = 131072;
	std::vector<Sample> Samples;
	std::array<network::ConnectionId, 2> Connections;
	runtime_detail::PublicationLatencySink Sink;
	runtime_detail::PublicationLatencySink *Previous = nullptr;
	std::array<RecipientGapHistogram, 2> *PreviousGaps = nullptr;
	std::array<RecipientGapHistogram, 2> Gaps{};
	std::array<std::array<RecipientGapHistogram, 2>, 4> CompletedGaps{};
	std::uint32_t Phase = 4;
	std::uint64_t Dropped = 0, Failures = 0;
	std::uint64_t Origin = 0;
	std::array<ObjectId, 10> Roots{};
	std::size_t RootCount = 0;
	bool Enabled = false, Printed = false;
	static bool Selected(void *Context, network::ConnectionId Connection) noexcept {
		const auto &Self = *static_cast<PublicationLatencyFixture *>(Context);
		return Self.Phase < 4 && (Connection == Self.Connections[0] || Connection == Self.Connections[1]);
	}
	static void Append(void *Context, Record Value) noexcept {
		auto &Self = *static_cast<PublicationLatencyFixture *>(Context);
		if (Self.Samples.size() < MaximumRecords) Self.Samples.push_back({Value, Self.Phase});
		else if (Self.Dropped != std::numeric_limits<std::uint64_t>::max()) ++Self.Dropped;
	}
	static void Packet(void *Context, const char *Stage, network::ConnectionId Connection,
		std::span<const std::byte> Payload, std::uint64_t Nanoseconds) noexcept {
		try {
			if (Payload.size() < 4) return;
			Record Value{.Stage = Stage, .Connection = Connection, .Nanoseconds = Nanoseconds,
				.Bytes = static_cast<std::uint32_t>(Payload.size())};
			if (std::memcmp(Payload.data(), "GCHR", 4) == 0) {
				auto Message = network::DecodeCharacterMessage(Payload);
				if (!Message) return;
				if (const auto *Frame = std::get_if<network::CharacterStateFrame>(&*Message)) {
					Value.Kind = 5; Value.Epoch = Frame->MaterializationEpoch.Value();
					for (const auto &State : Frame->GetStates()) {
						Value.Object = State.Character; Value.Tick = State.AuthoritativeTick;
						Value.Sequence = State.StateSequence.Value(); Append(Context, Value);
					}
				}
			} else if (std::memcmp(Payload.data(), "GRMT", 4) == 0) {
				auto Message = network::DecodeRemoteMessage(Payload);
				if (!Message) return;
				Value.Kind = 100 + static_cast<std::uint32_t>(Message->Kind);
				Value.Object = Message->Remote;
				Value.Sequence = Message->Request.IsValid() ? Message->Request.Value() : Message->Sequence.Value();
				Value.Epoch = Message->Publication.Value(); Append(Context, Value);
			} else if (std::memcmp(Payload.data(), "GRPL", 4) == 0) {
				// Read only the existing fixed diagnostic header; decoding the
				// structural graph again at each tap would contaminate attribution.
				if (Payload.size() < 36) return;
				auto Read = [&](std::size_t Offset, std::size_t Count) {
					std::uint64_t Result = 0;
					for (std::size_t I = 0; I < Count; ++I) Result |=
						static_cast<std::uint64_t>(std::to_integer<unsigned char>(Payload[Offset + I])) << (8 * I);
					return Result;
				};
				Value.Kind = 200; Value.Sequence = Read(16, 8); Value.Epoch = Read(8, 8);
				Value.Operations = static_cast<std::uint32_t>(Read(28, 4)); Append(Context, Value);
			}
		} catch (...) {
			auto &Self = *static_cast<PublicationLatencyFixture *>(Context);
			if (Self.Failures != std::numeric_limits<std::uint64_t>::max()) ++Self.Failures;
		}
	}
public:
	std::size_t GetCount() const noexcept { return Samples.size(); }
	std::uint64_t GetDropped() const noexcept { return Dropped; }
	void AddRoot(ObjectId Id) noexcept { if (RootCount < Roots.size()) Roots[RootCount++] = Id; }
	void SetOrigin(std::chrono::steady_clock::time_point Value) noexcept {
		Origin = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Value.time_since_epoch()).count());
	}
	PublicationLatencyFixture(bool Enabled, network::ConnectionId First, network::ConnectionId Last)
		: Connections{First, Last}, Sink{this, Selected, Append, Packet}, Enabled(Enabled) {
		if (!Enabled) return;
		Samples.reserve(MaximumRecords);
		Previous = runtime_detail::ActivePublicationLatency;
		PreviousGaps = ActiveRecipientGaps;
		runtime_detail::ActivePublicationLatency = &Sink;
	}
	~PublicationLatencyFixture() {
		if (!Enabled) return;
		runtime_detail::ActivePublicationLatency = Previous;
		ActiveRecipientGaps = PreviousGaps;
	}
	PublicationLatencyFixture(const PublicationLatencyFixture &) = delete;
	PublicationLatencyFixture &operator=(const PublicationLatencyFixture &) = delete;
	void Begin(std::string_view Name) {
		if (!Enabled) return;
		Phase = Name == "baseline" ? 0 : Name == "load" ? 1 : Name == "evict" ? 2 : 3;
		Gaps = {}; ActiveRecipientGaps = &Gaps;
	}
	void End() {
		if (!Enabled || Phase >= 4) return;
		CompletedGaps[Phase] = Gaps; ActiveRecipientGaps = nullptr; Phase = 4;
	}
	void Print() {
		if (!Enabled || Printed) return;
		Printed = true;
		constexpr std::array Names{"baseline", "load", "evict", "reload"};
		std::cout << "[Content:LatencyLimit] records=" << Samples.size() << " cap=" << MaximumRecords
			<< " reservedBytes=" << Samples.capacity() * sizeof(Sample) << " dropped=" << Dropped << " failures=" << Failures
			<< " firstPeer=" << Connections[0].Slot << " lastPeer=" << Connections[1].Slot << " originNs=" << Origin << '\n';
		for (std::size_t I = 0; I < RootCount; ++I)
			std::cout << "[Content:LatencyRoot] object=" << Roots[I].Slot << " generation=" << Roots[I].Generation << '\n';
		for (const auto &Sample : Samples) {
			const auto &V = Sample.Value;
			std::cout << "[Content:Latency] phase=" << Names[Sample.Phase] << " stage=" << V.Stage
				<< " peer=" << V.Connection.Slot << " peerGen=" << V.Connection.Generation
				<< " object=" << V.Object.Slot << " objectGen=" << V.Object.Generation << " tick=" << V.Tick
				<< " sequence=" << V.Sequence << " due=" << V.Due << " epoch=" << V.Epoch << " ns=" << V.Nanoseconds
				<< " kind=" << V.Kind << " tier=" << V.Tier << " bytes=" << V.Bytes << " operations=" << V.Operations << '\n';
		}
		for (std::size_t P = 0; P < Names.size(); ++P) for (std::size_t Kind = 0; Kind < 2; ++Kind) {
			const auto &H = CompletedGaps[P][Kind];
			std::cout << "[Content:RecipientHistogram] phase=" << Names[P] << " root=" << Kind << " samples=" << H.Count
				<< " p50UpperMs=" << H.Upper(.50) << " p95UpperMs=" << H.Upper(.95) << " p99UpperMs=" << H.Upper(.99)
				<< " maxMs=" << H.Maximum << " overflow=" << H.Buckets.back() << '\n';
		}
	}
};
}
