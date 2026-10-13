#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <optional>
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

enum class Contract { LegacyFiniteGrant, HybridF1, SanitizerSafety };

inline const char *Identity(Contract Value) {
	return Value == Contract::HybridF1 ? "HYBRID_F1_V1" :
		Value == Contract::LegacyFiniteGrant ? "finite_grant_v1" : "SANITIZER_SAFETY_V1";
}

// Replay the bounded unique-positive-native timeline independently of the
// retained legacy curve. Completion never stands in for ACK or retirement.
inline bool NativeCompletionValid(const detail::ReliableServiceFeedback &Sample,
	std::uint64_t Token, std::uint64_t Bytes, std::uint64_t ExpectedFirstSentBytes = 0) {
	if (!Sample.CountersValid || Sample.StructuralCompletionEvidenceInvalid ||
		Sample.StructuralCompletionServiceFailed || Sample.StructuralLastCompletedCompletionFailed ||
		Sample.StructuralLastCompletedGrantToken != Token || Sample.StructuralLastCompletedGrantBytes != Bytes ||
		Sample.StructuralPayloadBytesFirstSent != (ExpectedFirstSentBytes ? ExpectedFirstSentBytes : Bytes) ||
		!Sample.StructuralCompletedGrantSequence ||
		!Sample.LastCompletedStructuralSegmentEventCount ||
		Sample.LastCompletedStructuralSegmentEventCount > Sample.LastCompletedStructuralSegmentEvents.size()) return false;
	NativeGrantCompletionCurve Curve;
	if (!Curve.Activate(Token, Bytes, Sample.StructuralLastCompletedGrantActivatedAtMicroseconds)) return false;
	for (std::uint32_t Index = 0; Index < Sample.LastCompletedStructuralSegmentEventCount; ++Index) {
		const auto &Event = Sample.LastCompletedStructuralSegmentEvents[Index];
		if (!Curve.FirstSend(Token, Event.PayloadBytes, Event.AtMicroseconds)) return false;
	}
	return !Curve.Invalid && !Curve.GrantFailed && Curve.FirstSentBytes == Bytes &&
		Curve.FirstSentAtMicroseconds == Sample.StructuralLastCompletedGrantFirstSendAtMicroseconds &&
		Curve.CompletedAtMicroseconds == Sample.StructuralLastCompletedGrantCompletedAtMicroseconds &&
		Curve.CompletedAtMicroseconds <= Sample.ObservedAtMicroseconds;
}

inline bool LegacyTimingFailed(const detail::ReliableServiceFeedback &Sample) {
	return Sample.StructuralLastCompletedGrantFailed || Sample.StructuralServiceFailed ||
		Sample.StructuralMaximumFiniteShortfallByteMicroseconds != 0 ||
		Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
			FiniteGrantServiceCurve::RunningBoundByteMicroseconds;
}

inline bool SelectedTimingValid(const detail::ReliableServiceFeedback &Sample,
	std::uint64_t Token, std::uint64_t Bytes, Contract Selected, std::uint64_t ExpectedFirstSentBytes = 0) {
	return Selected == Contract::SanitizerSafety || (NativeCompletionValid(Sample, Token, Bytes, ExpectedFirstSentBytes) &&
		(Selected == Contract::HybridF1 || !LegacyTimingFailed(Sample)));
}

inline bool RunContractTests() {
	detail::ReliableServiceFeedback Sample;
	Sample.ObservedAtMicroseconds = 1'055'000;
	Sample.StructuralCompletedGrantSequence = Sample.StructuralLastCompletedGrantToken = 1;
	Sample.StructuralLastCompletedGrantBytes = Sample.StructuralPayloadBytesFirstSent = 65'536;
	Sample.StructuralLastCompletedGrantActivatedAtMicroseconds = 1'000'000;
	Sample.StructuralLastCompletedGrantFirstSendAtMicroseconds = 1'000'100;
	Sample.StructuralLastCompletedGrantCompletedAtMicroseconds = 1'005'000;
	Sample.LastCompletedStructuralSegmentEventCount = 2;
	Sample.LastCompletedStructuralSegmentEvents[0] = {1'000'100, 100};
	Sample.LastCompletedStructuralSegmentEvents[1] = {1'005'000, 65'436};
	FiniteGrantServiceCurve Legacy;
	if (!Legacy.Activate(1, 65'536, 1'000'000) || !Legacy.FirstSend(1, 100, 1'000'100)) return false;
	(void)Legacy.FirstSend(1, 65'436, 1'005'000);
	Sample.StructuralLastCompletedGrantFailed = Sample.StructuralServiceFailed = Legacy.GrantFailed;
	Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds = Legacy.MaximumRunningDeficitByteMicroseconds;
	if (!LegacyTimingFailed(Sample) || !SelectedTimingValid(Sample, 1, 65'536, Contract::HybridF1) ||
		SelectedTimingValid(Sample, 1, 65'536, Contract::LegacyFiniteGrant)) return false;
	auto Repeated = Sample;
	Repeated.StructuralPayloadBytesFirstSent = 2 * 65'536;
	if (!SelectedTimingValid(Repeated, 1, 65'536, Contract::HybridF1, 2 * 65'536) ||
		SelectedTimingValid(Repeated, 1, 65'536, Contract::HybridF1)) return false;
	auto Late = Sample;
	Late.StructuralLastCompletedGrantCompletedAtMicroseconds = 1'037'325;
	Late.LastCompletedStructuralSegmentEvents[1].AtMicroseconds = 1'037'325;
	if (SelectedTimingValid(Late, 1, 65'536, Contract::HybridF1)) return false;
	for (int Fault = 0; Fault < 8; ++Fault) {
		auto Invalid = Sample;
		switch (Fault) {
		case 0: Invalid.CountersValid = false; break;
		case 1: Invalid.StructuralCompletionEvidenceInvalid = true; break;
		case 2: Invalid.StructuralLastCompletedCompletionFailed = true; break;
		case 3: Invalid.LastCompletedStructuralSegmentEventCount = 513; break;
		case 4: --Invalid.LastCompletedStructuralSegmentEvents[1].PayloadBytes; break;
		case 5: Invalid.LastCompletedStructuralSegmentEvents[1].AtMicroseconds = 999'999; break;
		case 6: Invalid.StructuralLastCompletedGrantToken = 2; break;
		case 7: Invalid.ObservedAtMicroseconds = 1'004'999; break;
		}
		if (SelectedTimingValid(Invalid, 1, 65'536, Contract::HybridF1)) return false;
	}
	std::cout << "[Network:GnsFour] contract_policy=PASS hybrid_legacy_only_failure=diagnostic hard_faults=9\n";
	return true;
}

inline bool Run(Contract Selected = Contract::LegacyFiniteGrant) {
	const bool RequireWallClockF1 = Selected == Contract::LegacyFiniteGrant;
	const bool RequireNativeCompletion = Selected != Contract::SanitizerSafety;
	std::cout << "[Network:GnsFour] contract=" << Identity(Selected) << '\n';
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
		if (Selected == Contract::SanitizerSafety) {
			// Sanitizer instrumentation and shared CI scheduling are not an F1
			// throughput environment. Let the same real-GNS grants finish before
			// checking their byte, packet, ACK and retirement semantics.
			const auto Deadline = std::chrono::steady_clock::now() + 3s;
			bool Complete = false;
			while (std::chrono::steady_clock::now() < Deadline) {
				Complete = true;
				for (auto &Pair : Pairs) {
					const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(
						*Pair.Server, Pair.ServerConnection);
					if (!Sample || !Sample->CountersValid)
						throw std::runtime_error("sanitizer grant feedback unavailable");
					Complete &= Sample->StructuralLastCompletedGrantToken == 1;
				}
				if (Complete) break;
				std::this_thread::sleep_for(5ms);
			}
			if (!Complete) throw std::runtime_error("sanitizer grants did not first-send within bounded wait");
		}
		std::vector<Segment> All;
		std::uint64_t CommonStart = 0;
		std::uint64_t CommonEnd = UINT64_MAX;
		std::uint64_t EarliestActivation = UINT64_MAX;
		std::uint64_t LatestCompletion = 0;
		std::uint64_t NativePackets = 0;
		std::uint64_t NativePacketBytes = 0;
		std::uint64_t MaximumPeerRunningDeficit = 0;
		std::uint32_t WallClockFailedPeers = 0;
		std::array<std::vector<Segment>, 4> PerPeer;
		// Freeze every peer before validation/output. One failed peer must not
		// suppress the concurrent native traces needed to attribute the failure.
		std::array<std::optional<detail::ReliableServiceFeedback>, 4> Samples;
		bool AnyIndividualFailure = false;
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			Samples[Index] = detail::ReliableServiceFeedbackAccess::Observe(
				*Pairs[Index].Server, Pairs[Index].ServerConnection);
			const auto &Sample = Samples[Index];
			AnyIndividualFailure |= !Sample || !Sample->CountersValid ||
				Sample->NativePacketsSent < InitialPackets[Index] ||
				Sample->NativePacketBytesSent < InitialPacketBytes[Index] ||
				Sample->StructuralLastCompletedGrantToken != 1 ||
				Sample->StructuralLastCompletedGrantBytes != GrantBytes ||
				Sample->StructuralPayloadBytesFirstSent != GrantBytes ||
				Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds == 0 ||
				Sample->StructuralLastCompletedGrantCompletedAtMicroseconds < Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds ||
				Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds < Sample->StructuralLastCompletedGrantActivatedAtMicroseconds ||
				Sample->LastCompletedStructuralSegmentEventCount > Sample->LastCompletedStructuralSegmentEvents.size() ||
				!SelectedTimingValid(*Sample, 1, GrantBytes, Selected);
		}
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			const auto &Sample = Samples[Index];
			if (!Sample) {
				std::cerr << "[Network:GnsFour] peer=" << Index << " feedback_available=0\n";
				continue;
			}
			const bool CountersInvalid = !Sample->CountersValid;
			const bool CompletionMissing = Sample->StructuralLastCompletedGrantToken != 1;
			const bool SizeMismatch = Sample->StructuralLastCompletedGrantBytes != GrantBytes;
			const bool FirstSendMismatch = Sample->StructuralPayloadBytesFirstSent != GrantBytes ||
				Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds == 0 ||
				Sample->StructuralLastCompletedGrantCompletedAtMicroseconds <
					Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds ||
				Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds <
					Sample->StructuralLastCompletedGrantActivatedAtMicroseconds;
			const bool CurveFailed = Sample->StructuralLastCompletedGrantFailed;
			const bool RunningBoundExceeded =
				Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
					FiniteGrantServiceCurve::RunningBoundByteMicroseconds;
			MaximumPeerRunningDeficit = std::max(MaximumPeerRunningDeficit,
				Sample->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds);
			WallClockFailedPeers += CurveFailed || RunningBoundExceeded;
			if (AnyIndividualFailure || CurveFailed || RunningBoundExceeded) {
				std::cerr << "[Network:GnsFour] peer=" << Index
					<< " contract=" << Identity(Selected)
					<< " counters_invalid=" << CountersInvalid
					<< " completion_missing=" << CompletionMissing
					<< " size_mismatch=" << SizeMismatch
					<< " first_send_mismatch=" << FirstSendMismatch
					<< " native_completion_invalid=" << (RequireNativeCompletion && !NativeCompletionValid(*Sample, 1, GrantBytes))
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
					<< " acked=" << Sample->StructuralPayloadBytesAcked
					<< " retired_token=" << Sample->LastAttributedRetirementToken
					<< " retired_bytes=" << Sample->LastAttributedRetiredPayloadBytes
					<< " retransmitted=" << Sample->ReliableStreamBytesRetransmitted
					<< " segment_events=" << Sample->LastCompletedStructuralSegmentEventCount << '\n';
				for (std::size_t Event = 0; Event < std::min<std::size_t>(Sample->LastCompletedStructuralSegmentEventCount,
					std::size(Sample->LastCompletedStructuralSegmentEvents)); ++Event) {
					const auto &Native = Sample->LastCompletedStructuralSegmentEvents[Event];
					std::cerr << "[Network:GnsFour] peer=" << Index << " segment=" << Event
						<< " at_us=" << Native.AtMicroseconds
						<< " bytes=" << Native.PayloadBytes << '\n';
				}
				if (AnyIndividualFailure) continue;
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
				if (!Native.PayloadBytes ||
					Native.AtMicroseconds < Sample->StructuralLastCompletedGrantFirstSendAtMicroseconds ||
					Native.AtMicroseconds > Sample->StructuralLastCompletedGrantCompletedAtMicroseconds ||
					(!PerPeer[Index].empty() && Native.AtMicroseconds < PerPeer[Index].back().At))
					throw std::runtime_error("four-peer first-send event chronology invalid");
				PerPeer[Index].push_back({Native.AtMicroseconds, Native.PayloadBytes});
				Sum += Native.PayloadBytes;
			}
			if (Sum != GrantBytes) throw std::runtime_error("four-peer first-send timeline incomplete");
		}
		if (AnyIndividualFailure)
			throw std::runtime_error(Selected == Contract::SanitizerSafety ?
				"sanitizer grant attribution failed" : "individual maximum grant failed selected contract");
		if (CommonEnd <= CommonStart + (RequireNativeCompletion ? 1000 : 0))
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
		const auto FiniteEnvelopeByteMicroseconds =
			FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds + GrantBytes * 1000000;
		const auto FiniteEnvelopeMicroseconds =
			(FiniteEnvelopeByteMicroseconds + FiniteGrantServiceCurve::PeerRateBytesPerSecond - 1) /
			FiniteGrantServiceCurve::PeerRateBytesPerSecond;
		const auto ReserveWindowMicroseconds = RequireNativeCompletion ?
			LatestCompletion - EarliestActivation : FiniteEnvelopeMicroseconds;
		const auto ReserveBudget = Reserve * ReserveWindowMicroseconds / 1000000 + 4 * 1300;
		std::cout << "[Network:GnsFour] mode=" <<
			(RequireWallClockF1 ? "strict-f1" : Selected == Contract::HybridF1 ?
				"hybrid-native-completion" : "sanitizer-safety-f1-not-qualified")
			<< " contract=" << Identity(Selected)
			<< " overlap_us=" << (CommonEnd - CommonStart)
			<< " wall_clock_failed_peers=" << WallClockFailedPeers
			<< " peer_max_running_byte_us=" << MaximumPeerRunningDeficit
			<< " pool_max_deficit_byte_us=" << MaximumDeficit << " bound=" << PoolBound
			<< " native_packets=" << NativePackets << " wire_bytes_ipv4=" << NativeWireBytesIpv4
			<< " overhead_bytes=" << Overhead << " reserve_budget_bytes=" << ReserveBudget << '\n';
		if (RequireWallClockF1 && MaximumDeficit > PoolBound)
			throw std::runtime_error("four-grant pool F1 failed");
		if (Overhead > ReserveBudget)
			throw std::runtime_error("four-grant packet overhead exceeded transport reserve");
		for (auto &Pair : Pairs) {
			const auto Final = FeedbackFixture::Complete(Pair, GrantBytes);
			if (Final.StructuralPayloadBytesAcked != GrantBytes ||
				Final.LastAttributedRetirementToken != 1 ||
				Final.LastAttributedRetiredPayloadBytes != GrantBytes ||
				Final.ReliableStreamBytesRetransmitted != 0)
				throw std::runtime_error("four-peer ACK/retirement convergence failed");
			// ACK is transport receipt, not adapter delivery. Drain the existing
			// bounded event queue and require exactly one unchanged ordered payload.
			auto Received = Drain(*Pair.Client);
			Pair.ClientEvents.insert(Pair.ClientEvents.end(), std::make_move_iterator(Received.begin()),
				std::make_move_iterator(Received.end()));
			const auto Delivered = Payloads(Pair.ClientEvents);
			if (Delivered != std::vector<std::vector<std::byte>>{
				std::vector<std::byte>(GrantBytes - ReliableServiceEnvelopeBytes, std::byte{0x37})})
				throw std::runtime_error("four-peer exact ordered delivery failed");
		}
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:GnsFour] contract=" << Identity(Selected) << " FAIL " << Error.what() << '\n';
		return false;
	}
}
// Retain the existing diagnostic call convention, including sanitizer mode.
inline bool Run(bool RequireWallClockF1) {
	return Run(RequireWallClockF1 ? Contract::LegacyFiniteGrant : Contract::SanitizerSafety);
}
}
