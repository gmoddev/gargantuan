#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace GnsMixedTrafficFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;

inline bool Run() {
	constexpr std::size_t StructuralBytes = 64 * 1024;
	auto Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
	struct Cleanup {
		PairFixture &Pair;
		~Cleanup() { StopPair(Pair); }
	} Guard{Pair};
	try {
		if (!Pair.ServerConnection.IsValid()) throw std::runtime_error("mixed pair did not connect");
		const auto SendOrdinary = [&](TrafficClass Traffic, std::byte Marker) {
			auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
				Traffic, std::monostate{}, std::vector<std::byte>(64, Marker), Pair.Limits);
			if (!Intent || !Pair.Server->Send(*Intent).Succeeded())
				throw std::runtime_error("ordinary reliable send failed");
		};
		SendOrdinary(TrafficClass::Control, std::byte{0x11});
		auto Structural = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence(1)},
			std::vector<std::byte>(StructuralBytes - ReliableServiceEnvelopeBytes, std::byte{0x37}), Pair.Limits);
		if (!Structural || !detail::ReliableServiceFeedbackAccess::Attribute(*Structural, 1))
			throw std::runtime_error("mixed structural attribution failed");
		const auto Activated = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
		if (!detail::ReliableServiceFeedbackAccess::Activate(*Structural, 1, Activated) ||
			!Pair.Server->Send(*Structural).Succeeded())
			throw std::runtime_error("mixed structural send failed");
		SendOrdinary(TrafficClass::ReliableApplication, std::byte{0x55});
		std::this_thread::sleep_for(15ms);
		const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
		if (!Sample || !Sample->CountersValid || Sample->StructuralLastCompletedGrantToken != 1 ||
			Sample->StructuralLastCompletedGrantBytes != StructuralBytes ||
			Sample->StructuralLastCompletedGrantFailed ||
			Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
				FiniteGrantServiceCurve::RunningBoundByteMicroseconds)
			throw std::runtime_error("mixed structural finite grant failed F1");
		const auto Final = FeedbackFixture::Complete(Pair, StructuralBytes + 2 * (64 + ReliableServiceEnvelopeBytes));
		if (Final.StructuralPayloadBytesAcked != StructuralBytes ||
			Final.LastAttributedRetirementToken != 1 ||
			Final.LastAttributedRetiredPayloadBytes != StructuralBytes ||
			Final.ReliableStreamBytesRetransmitted != 0)
			throw std::runtime_error("mixed ACK/retirement attribution failed");
		// Native ACK/retirement can finish on GNS's service thread before the
		// application polls any receive events. Complete's ACK predicate can
		// therefore already be true at entry. Independently collect delivery;
		// this wait does not extend or resample the first-send F1 gate above.
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 2s, [&] {
			return Payloads(Pair.ClientEvents).size() >= 3;
		});
		const auto Received = Payloads(Pair.ClientEvents);
		if (Received.size() != 3 || Received[0] != std::vector<std::byte>(64, std::byte{0x11}) ||
			Received[1] != std::vector<std::byte>(StructuralBytes - ReliableServiceEnvelopeBytes, std::byte{0x37}) ||
			Received[2] != std::vector<std::byte>(64, std::byte{0x55}))
			throw std::runtime_error("ordinary control/structural/gameplay FIFO changed; received=" +
				std::to_string(Received.size()));
		std::cout << "[Network:GnsMixed] structural_bytes=" << StructuralBytes
			<< " max_deficit_byte_us=" << Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
			<< " ordinary_fifo=PASS ack_retirement=PASS\n";
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:GnsMixed] FAIL " << Error.what() << '\n';
		return false;
	}
}
}
