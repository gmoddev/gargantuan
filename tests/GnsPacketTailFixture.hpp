#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include <steam/isteamnetworkingutils.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// Uses the production adapter, its exact 32-byte frame, the pinned native GNS
// sender, and native unique-first-send counters.  No simulated transport clock.
namespace GnsPacketTailFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;

struct TailResult {
	std::uint64_t FirstBytes = 0;
	std::uint64_t FirstAt = 0;
	std::uint64_t CompletedAt = 0;
	std::uint64_t MaximumRunningDeficit = 0;
	std::uint64_t NativePackets = 0;
	std::uint64_t NativePacketBytes = 0;
	std::uint64_t NativeMaximumPacketBytes = 0;
	bool ServiceFailed = false;
	std::array<detail::ReliableServiceFeedback::StructuralSegmentEvent, 512> Segments{};
	std::uint32_t SegmentCount = 0;
};

inline TailResult Observe(std::size_t CompleteBytes, std::uint64_t Token) {
	if (CompleteBytes <= ReliableServiceEnvelopeBytes) throw std::runtime_error("invalid complete size");
	PairFixture Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
	if (!Pair.ServerConnection.IsValid()) throw std::runtime_error("real GNS pair did not connect");
	struct PairCleanup {
		PairFixture &Value;
		~PairCleanup() { StopPair(Value); }
	} Cleanup{Pair};
	auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
		TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence(Token)},
		std::vector<std::byte>(CompleteBytes - ReliableServiceEnvelopeBytes, std::byte{0x37}), Pair.Limits);
	if (!Intent || !detail::ReliableServiceFeedbackAccess::Attribute(*Intent, Token))
		throw std::runtime_error("structural attribution failed");
	const auto Before = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
	if (!Before || !Before->CountersValid) throw std::runtime_error("initial native packet counters unavailable");
	const auto Started = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	if (!detail::ReliableServiceFeedbackAccess::Activate(*Intent, Token, Started) ||
		!Pair.Server->Send(*Intent).Succeeded()) throw std::runtime_error("structural submission failed");
	TailResult Result;
	// Do not take the global GNS feedback lock during the finite drain itself.
	std::this_thread::sleep_for(50ms);
	const auto Deadline = std::chrono::steady_clock::now() + 2s;
	while (std::chrono::steady_clock::now() < Deadline) {
		const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
		if (!Sample) throw std::runtime_error("native feedback unavailable");
		if (!Sample->CountersValid) {
			std::cerr << "[Network:GnsTail] invalid counters first=" << Sample->StructuralPayloadBytesFirstSent
				<< " active=" << Sample->StructuralActiveGrantBytes << " grant_first="
				<< Sample->StructuralActiveGrantFirstSentBytes << " pending="
				<< Sample->PendingReliableStreamBytes << " unacked="
				<< Sample->SentUnackedReliableStreamBytes << '\n';
			throw std::runtime_error("native feedback invalid");
		}
		if (Sample->StructuralLastCompletedGrantToken == Token) {
			Result.FirstAt = Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds;
			Result.CompletedAt = Sample->StructuralLastCompletedGrantCompletedAtMicroseconds;
			Result.MaximumRunningDeficit =
				Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds;
			Result.ServiceFailed = Sample->StructuralLastCompletedGrantFailed;
			Result.NativePackets = Sample->NativePacketsSent - Before->NativePacketsSent;
			Result.NativePacketBytes = Sample->NativePacketBytesSent - Before->NativePacketBytesSent;
			Result.NativeMaximumPacketBytes = Sample->NativeMaximumPacketBytes;
			Result.Segments = Sample->LastCompletedStructuralSegmentEvents;
			Result.SegmentCount = Sample->LastCompletedStructuralSegmentEventCount;
			if (Result.SegmentCount)
				Result.FirstBytes = Result.Segments[0].PayloadBytes;
			break;
		}
		std::this_thread::sleep_for(2ms);
	}
	if (!Result.CompletedAt) throw std::runtime_error("native first-send did not complete");
	const auto Final = FeedbackFixture::Complete(Pair, CompleteBytes);
	if (Final.StructuralPayloadBytesAcked != CompleteBytes ||
		Final.LastAttributedRetirementToken != Token ||
		Final.LastAttributedRetiredPayloadBytes != CompleteBytes ||
		Final.ReliableStreamBytesRetransmitted != 0)
		throw std::runtime_error("native ACK/retirement conservation failed");
	std::cout << "[Network:GnsTail] bytes=" << CompleteBytes << " first_bytes=" << Result.FirstBytes
		<< " tail_bytes=" << (CompleteBytes - Result.FirstBytes) << " first_us=" << Result.FirstAt
		<< " complete_us=" << Result.CompletedAt << " gap_us=" << (Result.CompletedAt - Result.FirstAt)
		<< " max_deficit_byte_us=" << Result.MaximumRunningDeficit
		<< " native_packets=" << Result.NativePackets
		<< " native_wire_bytes_ipv4=" << (Result.NativePacketBytes + 28 * Result.NativePackets)
		<< " failed=" << Result.ServiceFailed << '\n';
	for (std::uint32_t Index = 0; Index < Result.SegmentCount; ++Index)
		std::cout << "[Network:GnsTail:Segment] index=" << Index << " bytes="
			<< Result.Segments[Index].PayloadBytes << " at_us=" << Result.Segments[Index].AtMicroseconds
			<< " since_first_us=" << (Result.Segments[Index].AtMicroseconds - Result.FirstAt) << '\n';
	return Result;
}

inline bool Run() {
	try {
		std::uint64_t Token = 1;
		for (const std::size_t Size : std::array<std::size_t, 14>{77, 1134, 1135, 1136, 1137,
			1138, 1248, 1257, 1258, 1259, 1260, 8192, 65536, 524288}) {
			const auto Result = Observe(Size, Token++);
			if (Result.ServiceFailed || Result.MaximumRunningDeficit >
				FiniteGrantServiceCurve::RunningBoundByteMicroseconds)
				throw std::runtime_error("scoped NoNagle failed a legal finite grant");
			if (Result.NativePackets == 0 || Result.NativeMaximumPacketBytes > 1300 ||
				Result.NativePacketBytes + 28 * Result.NativePackets >
					(Size * 9 / 8 + 2 * 1300))
				throw std::runtime_error("structural packet overhead exceeded the 18 MiB/s reserve envelope");
		}
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:GnsTail] corrected=FAIL " << Error.what() << '\n';
		return false;
	}
}
}
