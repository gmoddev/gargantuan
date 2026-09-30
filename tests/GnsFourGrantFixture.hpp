#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// Four real pinned-GNS senders share the service thread. Snapshot only after
// the finite drain, so observing evidence cannot contend with packet service.
namespace GnsFourGrantFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;

struct Segment {
	std::uint64_t At = 0;
	std::uint64_t Bytes = 0;
};

inline bool Run() {
	constexpr std::uint64_t GrantBytes = 512 * 1024;
	constexpr std::uint64_t PoolRate = 64 * 1024 * 1024;
	constexpr std::uint64_t PoolBound = 4 * FiniteGrantServiceCurve::RunningBoundByteMicroseconds;
	std::array<PairFixture, 4> Pairs;
	struct Cleanup {
		std::array<PairFixture, 4> &Pairs;
		~Cleanup() { for (auto &Pair : Pairs) StopPair(Pair); }
	} CleanupPairs{Pairs};
	try {
		for (auto &Pair : Pairs) {
			Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
			if (!Pair.ServerConnection.IsValid()) throw std::runtime_error("four-peer connection failed");
		}
		std::array<std::uint64_t, 4> InitialPackets{};
		std::array<std::uint64_t, 4> InitialPacketBytes{};
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			const auto Initial = detail::ReliableServiceFeedbackAccess::Observe(
				*Pairs[Index].Server, Pairs[Index].ServerConnection);
			if (!Initial || !Initial->CountersValid) throw std::runtime_error("four-peer packet baseline unavailable");
			InitialPackets[Index] = Initial->NativePacketsSent;
			InitialPacketBytes[Index] = Initial->NativePacketBytesSent;
		}
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			auto &Pair = Pairs[Index];
			auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
				TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence(1)},
				std::vector<std::byte>(GrantBytes - ReliableServiceEnvelopeBytes, std::byte{0x37}), Pair.Limits);
			if (!Intent || !detail::ReliableServiceFeedbackAccess::Attribute(*Intent, 1))
				throw std::runtime_error("four-peer attribution failed");
			const auto Now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			if (!detail::ReliableServiceFeedbackAccess::Activate(*Intent, 1, Now) ||
				!Pair.Server->Send(*Intent).Succeeded())
				throw std::runtime_error("four-peer structural submission failed");
		}
		std::this_thread::sleep_for(55ms);
		std::vector<Segment> All;
		std::uint64_t CommonStart = 0;
		std::uint64_t CommonEnd = UINT64_MAX;
		std::uint64_t EarliestActivation = UINT64_MAX;
		std::uint64_t LatestCompletion = 0;
		std::uint64_t NativePackets = 0;
		std::uint64_t NativePacketBytes = 0;
		std::array<std::vector<Segment>, 4> PerPeer;
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(
				*Pairs[Index].Server, Pairs[Index].ServerConnection);
			if (!Sample)
				throw std::runtime_error("individual maximum grant feedback unavailable");
			const bool CountersInvalid = !Sample->CountersValid;
			const bool CompletionMissing = Sample->StructuralLastCompletedGrantToken != 1;
			const bool SizeMismatch = Sample->StructuralLastCompletedGrantBytes != GrantBytes;
			const bool CurveFailed = Sample->StructuralLastCompletedGrantFailed;
			const bool RunningBoundExceeded =
				Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
					FiniteGrantServiceCurve::RunningBoundByteMicroseconds;
			if (CountersInvalid || CompletionMissing || SizeMismatch || CurveFailed || RunningBoundExceeded) {
				std::cerr << "[Network:GnsFour] peer=" << Index
					<< " counters_invalid=" << CountersInvalid
					<< " completion_missing=" << CompletionMissing
					<< " size_mismatch=" << SizeMismatch
					<< " curve_failed=" << CurveFailed
					<< " running_bound_exceeded=" << RunningBoundExceeded
					<< " observed_us=" << Sample->ObservedAtMicroseconds
					<< " active_token=" << Sample->ActiveAttributedRetirementToken
					<< " active_bytes=" << Sample->StructuralActiveGrantBytes
					<< " active_first_sent=" << Sample->StructuralActiveGrantFirstSentBytes
					<< " active_started_us=" << Sample->StructuralActiveGrantStartedAtMicroseconds
					<< " completed_token=" << Sample->StructuralLastCompletedGrantToken
					<< " completed_bytes=" << Sample->StructuralLastCompletedGrantBytes
					<< " activated_us=" << Sample->StructuralLastCompletedGrantActivatedAtMicroseconds
					<< " first_send_us=" << Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds
					<< " completed_us=" << Sample->StructuralLastCompletedGrantCompletedAtMicroseconds
					<< " max_running_byte_us="
					<< Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
					<< " max_finite_shortfall_byte_us="
					<< Sample->StructuralMaximumFiniteShortfallByteMicroseconds
					<< " native_packets=" << Sample->NativePacketsSent - InitialPackets[Index]
					<< " native_packet_bytes=" << Sample->NativePacketBytesSent - InitialPacketBytes[Index]
					<< " pending=" << Sample->PendingReliableStreamBytes
					<< " sent_unacked=" << Sample->SentUnackedReliableStreamBytes
					<< " segment_events=" << Sample->LastCompletedStructuralSegmentEventCount << '\n';
				for (std::uint32_t Event = 0; Event < Sample->LastCompletedStructuralSegmentEventCount; ++Event) {
					const auto &Native = Sample->LastCompletedStructuralSegmentEvents[Event];
					std::cerr << "[Network:GnsFour] peer=" << Index << " segment=" << Event
						<< " at_us=" << Native.AtMicroseconds
						<< " bytes=" << Native.PayloadBytes << '\n';
				}
				throw std::runtime_error("individual maximum grant failed F1");
			}
			CommonStart = std::max(CommonStart, Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds);
			CommonEnd = std::min(CommonEnd, Sample->StructuralLastCompletedGrantCompletedAtMicroseconds);
			EarliestActivation = std::min(EarliestActivation,
				Sample->StructuralLastCompletedGrantActivatedAtMicroseconds);
			LatestCompletion = std::max(LatestCompletion,
				Sample->StructuralLastCompletedGrantCompletedAtMicroseconds);
			NativePackets += Sample->NativePacketsSent - InitialPackets[Index];
			NativePacketBytes += Sample->NativePacketBytesSent - InitialPacketBytes[Index];
			if (Sample->NativeMaximumPacketBytes > 1300)
				throw std::runtime_error("pinned maximum UDP packet exceeded");
			std::uint64_t Sum = 0;
			for (std::uint32_t Event = 0; Event < Sample->LastCompletedStructuralSegmentEventCount; ++Event) {
				const auto &Native = Sample->LastCompletedStructuralSegmentEvents[Event];
				PerPeer[Index].push_back({Native.AtMicroseconds, Native.PayloadBytes});
				Sum += Native.PayloadBytes;
			}
			if (Sum != GrantBytes) throw std::runtime_error("four-peer first-send timeline incomplete");
		}
		if (CommonEnd <= CommonStart + 1000)
			throw std::runtime_error("no genuine common four-grant drain interval");
		for (const auto &Peer : PerPeer) {
			std::uint64_t AtStart = 0;
			for (const auto &Event : Peer) {
				if (Event.At <= CommonStart) AtStart += Event.Bytes;
				else if (Event.At <= CommonEnd) All.push_back(Event);
			}
			if (AtStart >= GrantBytes) throw std::runtime_error("peer finished before common interval");
		}
		std::sort(All.begin(), All.end(), [](const Segment &Left, const Segment &Right) {
			return Left.At < Right.At;
		});
		std::int64_t Minimum = 0;
		std::uint64_t Served = 0;
		std::uint64_t MaximumDeficit = 0;
		auto Observe = [&](std::uint64_t At) {
			const auto Balance = static_cast<std::int64_t>(PoolRate * (At - CommonStart)) -
				static_cast<std::int64_t>(Served * 1000000);
			if (Balance < Minimum) Minimum = Balance;
			else MaximumDeficit = std::max(MaximumDeficit, static_cast<std::uint64_t>(Balance - Minimum));
		};
		for (const auto &Event : All) {
			Observe(Event.At); // Check the open interval before this first-send.
			Served += Event.Bytes;
			Observe(Event.At);
		}
		Observe(CommonEnd);
		const auto NativeWireBytesIpv4 = NativePacketBytes + 28 * NativePackets;
		if (NativeWireBytesIpv4 < 4 * GrantBytes)
			throw std::runtime_error("native wire accounting omitted structural bytes");
		const auto Overhead = NativeWireBytesIpv4 - 4 * GrantBytes;
		const auto Reserve = ReliableServiceProfile::PooledService().Pooled.RequiredTransportReserve;
		const auto ReserveBudget = Reserve * (LatestCompletion - EarliestActivation) / 1000000 + 4 * 1300;
		std::cout << "[Network:GnsFour] overlap_us=" << (CommonEnd - CommonStart)
			<< " pool_max_deficit_byte_us=" << MaximumDeficit << " bound=" << PoolBound
			<< " native_packets=" << NativePackets << " wire_bytes_ipv4=" << NativeWireBytesIpv4
			<< " overhead_bytes=" << Overhead << " reserve_budget_bytes=" << ReserveBudget << '\n';
		if (MaximumDeficit > PoolBound) throw std::runtime_error("four-grant pool F1 failed");
		if (Overhead > ReserveBudget)
			throw std::runtime_error("four-grant packet overhead exceeded transport reserve");
		for (auto &Pair : Pairs) {
			const auto Final = FeedbackFixture::Complete(Pair, GrantBytes);
			if (Final.StructuralPayloadBytesAcked != GrantBytes ||
				Final.LastAttributedRetirementToken != 1 ||
				Final.LastAttributedRetiredPayloadBytes != GrantBytes ||
				Final.ReliableStreamBytesRetransmitted != 0)
				throw std::runtime_error("four-peer ACK/retirement convergence failed");
		}
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:GnsFour] FAIL " << Error.what() << '\n';
		return false;
	}
}
}
