#pragma once

#include "../src/runtime/PublicationLatencyDiagnostics.hpp"
#include "gargantuan/network/CharacterProtocol.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gargantuan::test {
// Load-phase attribution only. Records borrow static stage labels and contain
// full identities, never pointers or payloads. No publication/acceptance policy
// reads this sink. Overflow invalidates the trace instead of hiding lost events.
class JoinedCharacterFixture final {
	using Record = runtime_detail::PublicationLatencyRecord;
	static constexpr std::size_t MaximumRecords = 1'048'576;
	std::vector<Record> Records;
	std::array<network::ConnectionId, 500> Connections{};
	std::size_t ConnectionCount = 0;
	runtime_detail::PublicationLatencySink Sink{this, Selected, Append, Packet};
	runtime_detail::PublicationLatencySink *Previous = nullptr;
	bool Enabled = false, Active = false;
	std::uint64_t Dropped = 0, DecodeFailures = 0;
	static bool Selected(void *Context, network::ConnectionId Connection) noexcept {
		const auto &Self = *static_cast<JoinedCharacterFixture *>(Context);
		return Self.Active && (!Connection.IsValid() || std::binary_search(Self.Connections.begin(),
			Self.Connections.begin() + Self.ConnectionCount, Connection));
	}
	static void Append(void *Context, Record Value) noexcept {
		auto &Self = *static_cast<JoinedCharacterFixture *>(Context);
		if (Self.Records.size() < MaximumRecords) Self.Records.push_back(Value);
		else ++Self.Dropped;
	}
	static void Packet(void *Context, const char *Stage, network::ConnectionId Connection,
		std::span<const std::byte> Payload, std::uint64_t Nanoseconds) noexcept {
		const std::string_view Name(Stage);
		// Endpoint role is explicit at the simulated submit/delivery taps. The
		// older Handoff/SimulatedDelivered taps cannot disambiguate local IDs.
		if (Name == "Handoff" || Name == "SimulatedDelivered") return;
		auto &Self = *static_cast<JoinedCharacterFixture *>(Context);
		try {
			Record Value{.Stage = Stage, .Connection = Connection, .Nanoseconds = Nanoseconds,
				.Bytes = static_cast<std::uint32_t>(Payload.size())};
			if (Payload.size() < 4) { ++Self.DecodeFailures; return; }
			if (std::memcmp(Payload.data(), "GCHR", 4) == 0) {
				auto Message = network::DecodeCharacterMessage(Payload);
				if (!Message) { ++Self.DecodeFailures; return; }
				Value.Kind = 400 + static_cast<std::uint32_t>(network::GetCharacterMessageKind(*Message));
				if (const auto *Frame = std::get_if<network::CharacterStateFrame>(&*Message)) {
					Value.Tick = Frame->ServerTick; Value.Sequence = Frame->FrameSequence.Value();
					Value.Epoch = Frame->MaterializationEpoch.Value(); Value.Operations = Frame->StateCount;
					Append(Context, Value); // packet bytes counted once, not per state
					for (const auto &State : Frame->GetStates()) {
						Value.Kind = 5; Value.Object = State.Character; Value.Tick = State.AuthoritativeTick;
						Value.Sequence = State.StateSequence.Value(); Value.Due = State.ControlEpoch.Value();
						Value.Bytes = static_cast<std::uint32_t>(network::GetCompactCharacterStateEncodedBytes(State));
						Value.Operations = 1; Append(Context, Value);
					}
					return;
				}
			} else if (std::memcmp(Payload.data(), "GRMT", 4) == 0) {
				auto Message = network::DecodeRemoteMessage(Payload);
				if (!Message) { ++Self.DecodeFailures; return; }
				Value.Kind = 100 + static_cast<std::uint32_t>(Message->Kind);
				Value.Object = Message->Remote; Value.Epoch = Message->Publication.Value();
				Value.Sequence = Message->Request.IsValid() ? Message->Request.Value() : Message->Sequence.Value();
			} else if (std::memcmp(Payload.data(), "GRPL", 4) == 0) {
				if (Payload.size() < 36) { ++Self.DecodeFailures; return; }
				auto Read = [&](std::size_t Offset, std::size_t Count) {
					std::uint64_t Result = 0;
					for (std::size_t I = 0; I < Count; ++I)
						Result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(Payload[Offset + I])) << (8 * I);
					return Result;
				};
				Value.Kind = 200; Value.Epoch = Read(8, 8); Value.Sequence = Read(16, 8);
				Value.Operations = static_cast<std::uint32_t>(Read(28, 4));
			} else Value.Kind = 999; // counted control/unknown, never silently omitted
			Append(Context, Value);
		} catch (...) { ++Self.DecodeFailures; }
	}
public:
	JoinedCharacterFixture(bool Enable, std::span<const network::ConnectionId> Peers) : Enabled(Enable) {
		if (!Enabled) return;
		if (Peers.size() > Connections.size()) throw std::runtime_error("joined peer bound");
		ConnectionCount = Peers.size(); std::copy(Peers.begin(), Peers.end(), Connections.begin());
		std::sort(Connections.begin(), Connections.begin() + ConnectionCount);
		Records.reserve(MaximumRecords);
	}
	~JoinedCharacterFixture() { End(); }
	JoinedCharacterFixture(const JoinedCharacterFixture &) = delete;
	JoinedCharacterFixture &operator=(const JoinedCharacterFixture &) = delete;
	void Begin(std::string_view Phase) {
		if (!Enabled || Phase != "load") return;
		Previous = runtime_detail::ActivePublicationLatency;
		Active = true; runtime_detail::ActivePublicationLatency = &Sink;
		Mark("PhaseBegin", 0);
	}
	void End() noexcept {
		if (!Active) return;
		Mark("PhaseEnd", 0);
		runtime_detail::ActivePublicationLatency = Previous; Active = false;
	}
	void Mark(const char *Stage, std::uint64_t Tick, network::ConnectionId Peer = {}) const noexcept {
		if (Active) runtime_detail::RecordPublicationLatency({.Stage = Stage, .Connection = Peer, .Tick = Tick, .Kind = 302});
	}
	std::size_t GetCount() const noexcept { return Records.size(); }
	std::span<const Record> GetRecords() const noexcept { return Records; }
	std::uint64_t GetDropped() const noexcept { return Dropped; }
	void Print() const {
		if (!Enabled) return;
		std::cout << "[Content:JoinedLimit] records=" << Records.size() << " cap=" << MaximumRecords
			<< " reservedBytes=" << Records.capacity() * sizeof(Record) << " dropped=" << Dropped
			<< " decodeFailures=" << DecodeFailures << '\n';
		for (const auto &V : Records)
			std::cout << "[Content:Joined] stage=" << V.Stage << " peer=" << V.Connection.Slot
				<< " peerGen=" << V.Connection.Generation << " object=" << V.Object.Slot << " objectGen=" << V.Object.Generation
				<< " tick=" << V.Tick << " seq=" << V.Sequence << " due=" << V.Due << " epoch=" << V.Epoch
				<< " ns=" << V.Nanoseconds << " kind=" << V.Kind << " tier=" << V.Tier
				<< " bytes=" << V.Bytes << " operations=" << V.Operations << '\n';
		if (Dropped || DecodeFailures) throw std::runtime_error("joined trace incomplete");
	}
};
}
