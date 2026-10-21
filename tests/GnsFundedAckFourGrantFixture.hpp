#pragma once

#include "GnsFundedAckCompatibilityFixture.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

// Same real production-adapter pair and pinned native service thread as the
// existing strict four-grant fixture. This adds funded ACK policy and complete
// bidirectional wire observations. It does not claim GameSession admission or
// 32-peer fairness; the production farm remains responsible for those gates.
namespace GnsFundedAckFourGrantFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;
namespace Compatibility = GnsFundedAckCompatibilityFixture;

struct Segment { std::uint64_t At = 0, Bytes = 0; };

inline void DrainAll(std::array<PairFixture, 4> &Pairs) {
	for (auto &Pair : Pairs) {
		auto Server = Drain(*Pair.Server);
		auto Client = Drain(*Pair.Client);
		Pair.ServerEvents.insert(Pair.ServerEvents.end(), std::make_move_iterator(Server.begin()),
			std::make_move_iterator(Server.end()));
		Pair.ClientEvents.insert(Pair.ClientEvents.end(), std::make_move_iterator(Client.begin()),
			std::make_move_iterator(Client.end()));
	}
}

inline bool Run() {
	constexpr std::uint64_t GrantBytes = MaximumReliableServiceGroupBytes;
	constexpr std::uint64_t Token = 1;
	const auto Profile = ReliableServiceProfile::PooledService().Pooled;
	std::array<PairFixture, 4> Pairs;
	struct Cleanup {
		std::array<PairFixture, 4> &Pairs;
		~Cleanup() { for (auto &Pair : Pairs) StopPair(Pair); }
	} Guard{Pairs};
	try {
		std::array<Compatibility::WireSnapshot, 4> Before, AtAck;
		std::array<std::optional<NetworkMessageIntent>, 4> Intents;
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			auto &Pair = Pairs[Index];
			Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
			Compatibility::StartObserver(Pair);
			Before[Index] = Compatibility::ReadWire(Pair);
			Intents[Index].emplace(Compatibility::Intent(Pair, TrafficClass::StructuralReplication,
				GrantBytes, std::byte{0x37}, Token));
			Compatibility::Require(Compatibility::Access::Attribute(*Intents[Index], Token),
				"funded four-grant attribution failed");
		}
		// Payload construction and observer setup precede activation. Activation
		// remains before actual submission; it is never moved to first packet.
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			Compatibility::Require(Compatibility::Access::Activate(*Intents[Index], Token, Compatibility::Now()),
				"funded four-grant activation failed");
			Compatibility::Send(Pairs[Index], *Intents[Index]);
		}
		std::this_thread::sleep_for(55ms);
		std::array<std::vector<Segment>, 4> PerPeer;
		std::uint64_t CommonStart = 0, CommonEnd = std::numeric_limits<std::uint64_t>::max();
		std::uint64_t EarliestActivation = CommonEnd, LatestCompletion = 0, MaximumPeerDeficit = 0;
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			const auto Snapshot = Compatibility::ReadWire(Pairs[Index]);
			const auto &Sample = Snapshot.Sender;
			if (Sample.StructuralLastCompletedGrantToken != Token || Sample.StructuralLastCompletedGrantBytes != GrantBytes ||
				Sample.StructuralPayloadBytesFirstSent != GrantBytes || Sample.StructuralLastCompletedGrantFailed ||
				Sample.StructuralServiceFailed || Sample.StructuralMaximumFiniteShortfallByteMicroseconds != 0 ||
				Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
					FiniteGrantServiceCurve::RunningBoundByteMicroseconds) {
				std::cerr << "[Network:FundedAckFour] peer=" << Index << " token="
					<< Sample.StructuralLastCompletedGrantToken << " first_sent=" << Sample.StructuralPayloadBytesFirstSent
					<< " active_bytes=" << Sample.StructuralActiveGrantBytes << " failed=" << Sample.StructuralLastCompletedGrantFailed
					<< " finite_shortfall=" << Sample.StructuralMaximumFiniteShortfallByteMicroseconds
					<< " running_deficit=" << Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds << '\n';
				throw std::runtime_error("individual funded maximum grant failed strict native F1");
			}
			const auto Activation = Sample.StructuralLastCompletedGrantActivatedAtMicroseconds;
			const auto First = Sample.StructuralLastCompletedGrantFirstSendAtMicroseconds;
			const auto End = Sample.StructuralLastCompletedGrantCompletedAtMicroseconds;
			Compatibility::Require(Activation > 0 && First >= Activation && End >= First,
				"four-grant native activation/first-send/completion chronology invalid");
			CommonStart = std::max(CommonStart, First); CommonEnd = std::min(CommonEnd, End);
			EarliestActivation = std::min(EarliestActivation, Activation); LatestCompletion = std::max(LatestCompletion, End);
			MaximumPeerDeficit = std::max(MaximumPeerDeficit,
				Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds);
			std::uint64_t Sum = 0;
			for (std::uint32_t Event = 0; Event < Sample.LastCompletedStructuralSegmentEventCount; ++Event) {
				const auto &Native = Sample.LastCompletedStructuralSegmentEvents[Event];
				Compatibility::Require(Native.PayloadBytes > 0 && Native.AtMicroseconds >= First && Native.AtMicroseconds <= End &&
					(PerPeer[Index].empty() || Native.AtMicroseconds >= PerPeer[Index].back().At),
					"four-grant native segment chronology invalid");
				PerPeer[Index].push_back({Native.AtMicroseconds, Native.PayloadBytes}); Sum += Native.PayloadBytes;
			}
			Compatibility::Require(Sum == GrantBytes, "four-grant native timeline must conserve every exact first-send byte");
		}
		Compatibility::Require(CommonEnd > CommonStart + 1000, "no genuine common four-grant native drain interval");
		std::vector<Segment> CommonEvents;
		for (const auto &Peer : PerPeer) {
			std::uint64_t AtStart = 0;
			for (const auto &Event : Peer) {
				if (Event.At <= CommonStart) AtStart += Event.Bytes;
				else if (Event.At <= CommonEnd) CommonEvents.push_back(Event);
			}
			Compatibility::Require(AtStart > 0 && AtStart < GrantBytes, "common pool contains an unstarted or completed grant");
		}
		std::sort(CommonEvents.begin(), CommonEvents.end(), [](const Segment &Left, const Segment &Right) {
			return Left.At < Right.At;
		});
		std::int64_t Minimum = 0;
		std::uint64_t Served = 0, MaximumPoolDeficit = 0;
		auto Observe = [&](std::uint64_t At) {
			const auto Balance = static_cast<std::int64_t>(Profile.StructuralPool * (At - CommonStart)) -
				static_cast<std::int64_t>(Served * 1000000);
			if (Balance < Minimum) Minimum = Balance;
			else MaximumPoolDeficit = std::max(MaximumPoolDeficit, static_cast<std::uint64_t>(Balance - Minimum));
		};
		for (const auto &Event : CommonEvents) {
			Observe(Event.At); // Charge the open interval before the native service event.
			Served += Event.Bytes; Observe(Event.At);
		}
		Observe(CommonEnd);
		Compatibility::Require(MaximumPoolDeficit <= PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds,
			"funded ACK prototype violated the unchanged common four-grant running curve");

		bool Retired = false;
		const auto Deadline = std::chrono::steady_clock::now() + 2s;
		while (std::chrono::steady_clock::now() < Deadline) {
			DrainAll(Pairs); Retired = true;
			for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
				AtAck[Index] = Compatibility::ReadWire(Pairs[Index]);
				const auto &Sample = AtAck[Index].Sender;
				Retired &= Sample.LastAttributedRetirementToken == Token && Sample.ReliablePayloadBytesAcked == GrantBytes &&
					Sample.PendingReliableStreamBytes == 0 && Sample.SentUnackedReliableStreamBytes == 0;
			}
			if (Retired) break;
			std::this_thread::sleep_for(1ms);
		}
		Compatibility::Require(Retired, "four-grant ACK retirement did not converge");
		// Shared post-retirement observation horizon for every endpoint, including
		// confirmations emitted after the attributed token has gone away.
		const auto TailDeadline = std::chrono::steady_clock::now() + 100ms;
		while (std::chrono::steady_clock::now() < TailDeadline) { DrainAll(Pairs); std::this_thread::sleep_for(1ms); }
		DrainAll(Pairs);
		std::uint64_t WholeIpv4 = 0, WholeIpv6 = 0, TailIpv6 = 0, FundedCeilings = 0, PromptCount = 0;
		for (std::size_t Index = 0; Index < Pairs.size(); ++Index) {
			auto &Pair = Pairs[Index];
			const auto Final = Compatibility::ReadWire(Pair);
			const auto &Sample = Final.Sender;
			Compatibility::Require(Sample.StructuralPayloadBytesFirstSent == GrantBytes &&
				Sample.StructuralPayloadBytesAcked == GrantBytes && Sample.ReliablePayloadBytesAcked == GrantBytes &&
				Sample.LastAttributedRetirementToken == Token && Sample.LastAttributedRetiredPayloadBytes == GrantBytes &&
				Sample.ActiveAttributedRetirementToken == 0 && Sample.PendingReliableStreamBytes == 0 &&
				Sample.SentUnackedReliableStreamBytes == 0 && Sample.ReliableStreamBytesRetransmitted == 0 &&
				Sample.UniqueReliableStreamBytesAcked == Sample.UniqueReliableStreamBytesFirstSent,
				"four-grant terminal exact ACK/retirement/conservation failed");
			Compatibility::ExactPayloads(Pair, {Compatibility::Payload(GrantBytes, std::byte{0x37})});
			GargantuanAckDiagnostics Sender, Receiver;
			Compatibility::Require(Compatibility::AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) &&
				Compatibility::AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver), "four-grant ACK observers missing");
			Compatibility::Require(Compatibility::Requests(Sender, Token) == 1 && Compatibility::Requests(Receiver, 0) == 0 &&
				Sender.PromptFinalWireAllowed && !Sender.GrantWireInvalid, "each funded maximum grant must prompt exactly once");
			++PromptCount;
			const auto Wire = Compatibility::Cost(Before[Index], Final);
			const auto Tail = Compatibility::Cost(AtAck[Index], Final);
			Compatibility::Require(Wire.SenderPackets > 0 && Wire.ReceiverPackets > 0 &&
				Wire.ConservativeIpv6Bytes <= Sender.GrantWholeWireCeiling,
				"funded maximum grant exceeded its independent whole bidirectional wire ceiling");
			WholeIpv4 += Wire.Ipv4Bytes; WholeIpv6 += Wire.ConservativeIpv6Bytes;
			TailIpv6 += Tail.ConservativeIpv6Bytes; FundedCeilings += Sender.GrantWholeWireCeiling;
			std::cout << "[Network:FundedAckFour] peer=" << Index << " complete_us="
				<< Sample.StructuralLastCompletedGrantCompletedAtMicroseconds - Sample.StructuralLastCompletedGrantActivatedAtMicroseconds
				<< " running_deficit=" << Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
				<< " whole_wire_ipv4=" << Wire.Ipv4Bytes << " whole_wire_ipv6_bound=" << Wire.ConservativeIpv6Bytes
				<< " wire_ceiling=" << Sender.GrantWholeWireCeiling << " requests=1\n";
		}
		Compatibility::Require(WholeIpv4 >= 4 * GrantBytes && PromptCount == 4,
			"pool wire accounting or exact prompt count missing");
		// Preserve the existing strict four-grant reserve window and startup packet
		// term, but now charge reverse ACK and post-retirement packets as well.
		const auto ReserveBudget = Profile.RequiredTransportReserve * (LatestCompletion - EarliestActivation) / 1000000 + 4 * 1300;
		Compatibility::Require(WholeIpv4 - 4 * GrantBytes <= ReserveBudget && WholeIpv6 <= FundedCeilings,
			"whole four-grant wire overhead exceeded unchanged reserve or funded ceilings");
		std::cout << "[Network:FundedAckFour] overlap_us=" << CommonEnd - CommonStart
			<< " peer_max_running_byte_us=" << MaximumPeerDeficit << " pool_max_running_byte_us=" << MaximumPoolDeficit
			<< " pool_bound_byte_us=" << PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds
			<< " whole_wire_ipv4=" << WholeIpv4 << " whole_wire_ipv6_bound=" << WholeIpv6
			<< " reserve_budget=" << ReserveBudget << " post_retirement_us=100000 tail_wire_ipv6_bound=" << TailIpv6
			<< " requests=" << PromptCount << " result=PASS\n";
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:FundedAckFour] FAIL " << Error.what() << '\n';
		return false;
	}
}
}
