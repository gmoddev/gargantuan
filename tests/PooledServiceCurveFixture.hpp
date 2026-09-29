#pragma once

#include "../cmake/gns/ReliableServiceFeedback.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace ServiceCurveFixture {
using gargantuan::network::PooledReliableServiceProfile;
constexpr std::uint64_t Rate = 16ULL * 1024 * 1024;
constexpr std::uint64_t Bound = PooledReliableServiceProfile::ServiceDeficitBoundByteMicroseconds;
static_assert(PooledReliableServiceProfile::ServiceQuantumBytes == 1248);
static_assert(PooledReliableServiceProfile::MaximumQualifiedServiceHandoffMicroseconds == 6000);
static_assert(Bound == 101'911'296'000ULL);
static_assert(4 * Bound == 407'645'184'000ULL);

inline void Require(bool Condition, const char *Message) {
	if (!Condition) throw std::runtime_error(Message);
}

inline GargantuanReliableServiceCounters Grant(std::uint64_t Token, int Bytes,
	std::uint64_t Start = 1'000'000) {
	GargantuanReliableServiceCounters Value;
	Value.AttributeMessage(Token, static_cast<std::int64_t>(Token), Bytes, Start);
	Require(!Value.Invalid, "grant attribution is valid");
	return Value;
}

inline bool Run() {
	std::size_t Passed = 0;
	auto Case = [&](const char *Name, auto Test) {
		try { Test(); ++Passed; std::cout << "[Network:ServiceCurve] " << Name << "=PASS\n"; }
		catch (const std::exception &Error) {
			std::cerr << "[Network:ServiceCurve] " << Name << "=FAIL " << Error.what() << '\n';
		}
	};
	Case("SubpacketAndExactBoundary", [] {
		auto Value = Grant(1, 100);
		Value.ObserveActiveService(1'006'000);
		Require(!Value.StructuralServiceFailed && Value.StructuralMaximumDeficitByteMicroseconds == Rate * 6000,
			"6 ms requested Nagle/wake handoff conforms");
		Value.FirstSend(102, 100, 1'006'000);
		Require(!Value.Invalid && !Value.StructuralServiceFailed && Value.StructuralPayloadBytesFirstSent == 100,
			"subpacket first send remains within B_i");
		auto Beyond = Grant(1, 100);
		Beyond.ObserveActiveService(1'006'075);
		Require(Beyond.StructuralServiceFailed && Beyond.StructuralMaximumDeficitByteMicroseconds > Bound,
			"failure follows the exact curve beyond G_i, not a delay > 6 ms shortcut");
	});
	Case("FullPacketPacingAndLateWake", [] {
		auto Value = Grant(1, 4 * 1248);
		for (int Index = 0; Index < 4; ++Index)
			Value.FirstSend(1248, 1248, 1'000'074 + 74 * Index);
		Require(!Value.Invalid && !Value.StructuralServiceFailed &&
			Value.StructuralPayloadBytesFirstSent == 4 * 1248 &&
			Value.StructuralQualifiedActiveMicroseconds == 296,
			"full native packets and ordinary pacing count as service");
		auto Late = Grant(1, 1248);
		Late.ObserveActiveService(1'008'000);
		Require(Late.StructuralServiceFailed, "OS/thread oversleep beyond requested wake is charged");
	});
	Case("GameSessionBudgetDefer", [] {
		auto Value = Grant(1, 1248);
		Value.ObserveActiveService(1'016'667);
		Require(Value.StructuralServiceFailed && Value.StructuralQualifiedActiveMicroseconds == 16'667,
			"one server step of queued active work is not exempt handoff");
	});
	Case("AckBurstAndGameplayIsolation", [] {
		auto Value = Grant(1, 1000);
		Value.FirstSend(1002, 1000, 1'001'000);
		Require(!Value.StructuralServiceFailed && !Value.StructuralPayloadBytesAcked &&
			Value.StructuralPayloadBytesFirstSent == 1000,
			"zero ACK in a fresh first-send observation remains healthy");
		Value.FirstSend(700, 0, 1'001'100);
		Require(Value.StructuralPayloadBytesFirstSent == 1000,
			"ordinary reliable gameplay cannot satisfy structural S_i");
		Value.AckSegment(1002, false); Value.AckMessage(1, 1000, 0);
		Require(!Value.Invalid && Value.StructuralPayloadBytesAcked == 1000 &&
			Value.StructuralPayloadBytesFirstSent - Value.StructuralPayloadBytesAcked == 0,
			"later structural ACK converges without gameplay credit");
	});
	Case("TurnoverAndRequalification", [] {
		auto Value = Grant(1, 1000);
		Value.FirstSend(1000, 1000, 1'005'000);
		Value.AckSegment(1000, false); Value.AckMessage(1, 1000, 0);
		const auto FirstDeficit = Value.StructuralCurrentDeficitByteMicroseconds;
		const auto QualifiedTime = Value.StructuralQualifiedActiveMicroseconds;
		Require(FirstDeficit > 0 && QualifiedTime == 5000 && !Value.StructuralServiceFailed,
			"first grant leaves nonzero history");
		Value.AttributeMessage(2, 2, 1000, 2'005'000);
		Require(!Value.Invalid && Value.StructuralQualifiedActiveMicroseconds == QualifiedTime &&
			Value.StructuralCurrentDeficitByteMicroseconds == FirstDeficit,
			"credit/fairness and one-second requalification wait pause time without reset");
		Value.ObserveActiveService(2'006'200);
		Require(Value.StructuralServiceFailed && Value.StructuralMaximumDeficitByteMicroseconds > Bound,
			"next active grant resumes against historical deficit");
	});
	Case("FourPeerComposition", [] {
		std::array<GargantuanReliableServiceCounters, 4> Peers{
			Grant(1, 60'000), Grant(2, 60'000), Grant(3, 60'000), Grant(4, 60'000)};
		for (int Tick = 1; Tick <= 6; ++Tick)
			for (int Peer = 1; Peer < 4; ++Peer)
				Peers[Peer].FirstSend(8000, 8000, 1'000'000 + 1000 * Tick);
		std::uint64_t TotalFirstSent = 0;
		for (auto &Peer : Peers) {
			Peer.ObserveActiveService(1'007'000);
			TotalFirstSent += Peer.StructuralPayloadBytesFirstSent;
		}
		Require(TotalFirstSent * 1'000'000 + 4 * Bound >= 4 * Rate * 7000,
			"aggregate 64 MiB/s curve with B_pool can pass");
		Require(Peers[0].StructuralServiceFailed &&
			!Peers[1].StructuralServiceFailed && !Peers[2].StructuralServiceFailed && !Peers[3].StructuralServiceFailed,
			"pool success cannot hide one peer's B_i violation");
	});
	std::cout << "[Network:ServiceCurve] passed=" << Passed << " total=6\n";
	return Passed == 6;
}
}
