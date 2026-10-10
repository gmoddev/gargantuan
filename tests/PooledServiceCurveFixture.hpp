#pragma once

#include "../cmake/gns/ReliableServiceFeedback.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace ServiceCurveFixture {
using gargantuan::network::PooledReliableServiceProfile;
constexpr std::uint64_t Rate = 16ULL * 1024 * 1024;
constexpr std::uint64_t RunningBound = PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds;
static_assert(PooledReliableServiceProfile::ServiceQuantumBytes == 1248);
static_assert(PooledReliableServiceProfile::ServiceStartupMicroseconds == 5000);
static_assert(PooledReliableServiceProfile::ServiceSchedulingMicroseconds == 1000);
static_assert(PooledReliableServiceProfile::FiniteGrantInterceptByteMicroseconds == 101'911'296'000ULL);
static_assert(RunningBound == 18'025'216'000ULL);
static_assert(PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds == 72'100'864'000ULL);

inline void Require(bool Condition, const char *Message) {
	if (!Condition) throw std::runtime_error(Message);
}
inline GargantuanReliableServiceCounters Grant(std::uint64_t Token, int Bytes,
	std::uint64_t Start = 1'000'000) {
	GargantuanReliableServiceCounters Value;
	Value.AttributeMessage(Token, static_cast<std::int64_t>(Token), Bytes, Start);
	Require(!Value.Invalid, "finite grant attribution");
	return Value;
}
inline void Retire(GargantuanReliableServiceCounters &Value, std::uint64_t Token, int Bytes) {
	Value.AckSegment(Bytes, false);
	Value.AckMessage(static_cast<std::int64_t>(Token), Bytes, 0);
	Require(!Value.Invalid && Value.LastAttributedRetirementToken == Token, "exact ACK retirement");
}
inline bool Run() {
	std::size_t Passed = 0;
	auto Case = [&](const char *Name, auto Test) {
		try { Test(); ++Passed; std::cout << "[Network:ServiceCurve] " << Name << "=PASS\n"; }
		catch (const std::exception &Error) {
			std::cerr << "[Network:ServiceCurve] " << Name << "=FAIL " << Error.what() << '\n';
		}
	};
	Case("TinyFiniteLatencyAndPointwiseBoundary", [] {
		auto Prompt = Grant(1, 77);
		Prompt.FirstSend(77, 77, 1'006'000);
		Require(!Prompt.Invalid && !Prompt.StructuralServiceFailed &&
			Prompt.StructuralLastCompletedGrantBytes == 77, "prompt tiny grant");
		auto Late = Grant(1, 77);
		Late.FirstSend(77, 77, 1'006'075);
		Require(!Late.Invalid && Late.StructuralServiceFailed &&
			Late.StructuralPayloadBytesFirstSent == 77 && Late.StructuralLastCompletedGrantFailed,
			"late atomic first-send fails exact pointwise F1 but retains bytes");
	});
	Case("RepeatedTinyGrantsAndAckGaps", [] {
		GargantuanReliableServiceCounters Value;
		for (std::uint64_t Index = 1; Index <= 32; ++Index) {
			const auto Start = 1'000'000 + Index * 250'000;
			Value.AttributeMessage(Index, static_cast<std::int64_t>(Index), 77, Start);
			Value.FirstSend(77, 77, Start + 5'000);
			Retire(Value, Index, 77);
			Require(!Value.Invalid && !Value.StructuralServiceFailed &&
				!Value.StructuralCurrentDeficitByteMicroseconds, "gap cannot accrue grant deficit");
		}
		Require(Value.StructuralCompletedGrantSequence == 32 &&
			Value.StructuralPayloadBytesFirstSent == 32 * 77, "separate tiny obligations");
	});
	Case("MediumSweepAndOrdinaryIsolation", [] {
		for (int Bytes : {1248, 8192, 64 * 1024}) {
			auto Value = Grant(1, Bytes);
			Value.FirstSend(Bytes, Bytes, 1'005'000);
			Require(!Value.Invalid && !Value.StructuralServiceFailed &&
				!Value.StructuralPayloadBytesAcked &&
				Value.StructuralLastCompletedGrantBytes == static_cast<std::uint64_t>(Bytes),
				"finite grant complete before ACK");
			Value.FirstSend(700, 0, 1'005'100);
			Require(Value.StructuralPayloadBytesFirstSent == static_cast<std::uint64_t>(Bytes),
				"ordinary traffic cannot claim structural first-send");
			Retire(Value, 1, Bytes);
		}
	});
	Case("MaximumGrantHealthyDrain", [] {
		auto Value = Grant(1, 512 * 1024);
		int Remaining = 512 * 1024;
		std::uint64_t Time = 1'005'000;
		while (Remaining) {
			const auto Bytes = std::min(Remaining, 1248);
			Value.FirstSend(Bytes, Bytes, Time);
			Remaining -= Bytes; Time += 70;
		}
		Require(!Value.Invalid && !Value.StructuralServiceFailed &&
			Value.StructuralLastCompletedGrantBytes == 512 * 1024 &&
			Value.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds <= RunningBound &&
			Value.StructuralLastCompletedGrantCompletedAtMicroseconds <= 1'037'325,
			"maximum grant finite and ongoing bounds");
		Retire(Value, 1, 512 * 1024);
	});
	Case("SlowSenderFailsDuringLargeGrant", [] {
		auto Value = Grant(1, 512 * 1024);
		for (int Index = 0; Index < 90; ++Index)
			Value.FirstSend(1248, 1248, 1'005'000 + 100 * Index);
		Require(!Value.Invalid && Value.StructuralServiceFailed &&
			Value.StructuralMaximumDeficitByteMicroseconds > RunningBound &&
			Value.StructuralActiveGrantBytes == 512 * 1024, "sub-floor running sender fails");
	});
	Case("FourPeerCommonPool", [] {
		std::array<GargantuanReliableServiceCounters, 4> Peers{
			Grant(1, 512 * 1024), Grant(2, 512 * 1024),
			Grant(3, 512 * 1024), Grant(4, 512 * 1024)};
		int Remaining = 512 * 1024;
		std::uint64_t Time = 1'005'000;
		while (Remaining) {
			const auto Bytes = std::min(Remaining, 1248);
			for (auto &Peer : Peers) Peer.FirstSend(Bytes, Bytes, Time);
			Remaining -= Bytes; Time += 70;
		}
		std::uint64_t Begin = 0, End = std::numeric_limits<std::uint64_t>::max();
		for (const auto &Peer : Peers) {
			Require(!Peer.Invalid && !Peer.StructuralServiceFailed &&
				Peer.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds <= RunningBound,
				"all peers pass own all-subinterval bound");
			Begin = std::max(Begin, Peer.StructuralLastCompletedGrantFirstSendAtMicroseconds);
			End = std::min(End, Peer.StructuralLastCompletedGrantCompletedAtMicroseconds);
		}
		Require(End > Begin && End - Begin >= 25'000 &&
			4 * Rate == 64ULL * 1024 * 1024 &&
			4 * RunningBound == PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds,
			"common active interval inherits sum of four peer all-subinterval proofs");
	});
	Case("FourAsymmetricGrantsStopChargingCompletedPeer", [] {
		const std::array<int, 4> Sizes{512 * 1024, 256 * 1024, 128 * 1024, 64 * 1024};
		std::array<GargantuanReliableServiceCounters, 4> Peers{
			Grant(1, Sizes[0]), Grant(2, Sizes[1]), Grant(3, Sizes[2]), Grant(4, Sizes[3])};
		std::array<int, 4> Remaining = Sizes;
		std::uint64_t At = 1'005'000;
		while (std::any_of(Remaining.begin(), Remaining.end(), [](int Bytes) { return Bytes > 0; })) {
			for (std::size_t Index = 0; Index < Peers.size(); ++Index) {
				if (!Remaining[Index]) continue;
				const int Bytes = std::min(Remaining[Index], 1248);
				Peers[Index].FirstSend(Bytes, Bytes, At);
				Remaining[Index] -= Bytes;
			}
			At += 70;
		}
		for (std::size_t Index = 0; Index < Peers.size(); ++Index) {
			Peers[Index].ObserveActiveService(At + 10'000);
			Require(!Peers[Index].Invalid && !Peers[Index].StructuralServiceFailed &&
				Peers[Index].StructuralLastCompletedGrantBytes == static_cast<std::uint64_t>(Sizes[Index]) &&
				!Peers[Index].StructuralActiveGrantBytes,
				"completed asymmetric peer accrues no later pool demand");
		}
		Require(Peers[3].StructuralLastCompletedGrantCompletedAtMicroseconds <
			Peers[0].StructuralLastCompletedGrantCompletedAtMicroseconds,
			"smaller grant exits the common running interval first");
	});
	std::cout << "[Network:ServiceCurve] passed=" << Passed << " total=7\n";
	return Passed == 7;
}
}
