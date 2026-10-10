#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace FiniteGrantServiceCurveFixture {
using gargantuan::network::FiniteGrantServiceCurve;

inline void Require(bool Condition, const char *Message) {
	if (!Condition) throw std::runtime_error(Message);
}

struct Step {
	std::uint64_t Time;
	std::uint64_t Bytes;
};

struct ReferencePoint {
	std::uint64_t Time;
	std::uint64_t Cumulative;
	bool Running;
};

// Intentionally straightforward reference: enumerate both sides of every
// atomic first-send and compare each running point with every earlier point.
// Between points, demand only rises, so the pre-event endpoint is the worst
// case. This is independent of the online running-min implementation.
inline bool ReferenceFailed(std::uint64_t GrantBytes, const std::vector<Step> &Steps,
	std::uint64_t Horizon) {
	std::vector<ReferencePoint> Points;
	Points.push_back({0, 0, false});
	std::uint64_t Cumulative = 0;
	bool Running = false;
	std::uint64_t FirstAt = 0, FirstBytes = 0;
	for (const Step &Event : Steps) {
		Points.push_back({Event.Time, Cumulative, Running});
		Cumulative += Event.Bytes;
		if (!Running) { Running = true; FirstAt = Event.Time; FirstBytes = Cumulative; }
		Points.push_back({Event.Time, Cumulative, Running});
	}
	Points.push_back({Horizon, Cumulative, Running});
	std::vector<std::int64_t> PreviousBalances;
	for (const ReferencePoint &Point : Points) {
		if (Point.Cumulative < GrantBytes) {
			const std::uint64_t Due = Point.Time * FiniteGrantServiceCurve::PeerRateBytesPerSecond;
			const std::uint64_t Demand = Due > FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds
				? std::min(GrantBytes * 1000000,
					Due - FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds) : 0;
			if (Point.Cumulative * 1000000 < Demand) return true;
		}
		if (!Point.Running || Point.Cumulative == GrantBytes) continue;
		const std::int64_t Balance =
			static_cast<std::int64_t>((Point.Time - FirstAt) *
				FiniteGrantServiceCurve::PeerRateBytesPerSecond) -
			static_cast<std::int64_t>((Point.Cumulative - FirstBytes) * 1000000);
		for (const std::int64_t Earlier : PreviousBalances)
			if (Balance > Earlier &&
				static_cast<std::uint64_t>(Balance) - static_cast<std::uint64_t>(Earlier) >
					FiniteGrantServiceCurve::RunningBoundByteMicroseconds) return true;
		PreviousBalances.push_back(Balance);
	}
	return false;
}

inline bool Run() {
	std::size_t Passed = 0;
	auto Case = [&](const char *Name, const auto &Test) {
		try { Test(); ++Passed; std::cout << "[Network:FiniteGrant] " << Name << "=PASS\n"; }
		catch (const std::exception &Error) {
			std::cerr << "[Network:FiniteGrant] " << Name << "=FAIL " << Error.what() << '\n';
		}
	};
	static_assert(FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds == 101911296000ULL,
		"F1 finite intercept must remain exact");
	static_assert(FiniteGrantServiceCurve::RunningBoundByteMicroseconds == 18025216000ULL,
		"F1 recurring bound must remain exact");
	Case("IsolatedTinyAndAckGap", [] {
		FiniteGrantServiceCurve Curve;
		Require(Curve.Activate(1, 77, 1000000), "activate tiny grant");
		Require(Curve.FirstSend(1, 77, 1000020), "prompt first-send");
		Require(Curve.FirstSentBytes == 77 && Curve.CompletedAtMicroseconds == 1000020,
			"exact first-send conservation");
		Require(Curve.Observe(2100000) && !Curve.ServiceFailed &&
			Curve.QualifiedRunningMicroseconds == 0,
			"ACK wait does not accrue drain time");
	});
	Case("RepeatedTinyCreditAndRequalificationGaps", [] {
		FiniteGrantServiceCurve Curve;
		for (std::uint64_t Index = 1; Index <= 100; ++Index) {
			const std::uint64_t Start = Index * 1000000;
			Require(Curve.Activate(Index, 77, Start), "activate next finite grant");
			Require(Curve.FirstSend(Index, 77, Start + 20), "prompt tiny first-send");
			Require(Curve.Observe(Start + 800000), "credit/ACK gap remains uncharged");
		}
		Require(!Curve.ServiceFailed && !Curve.Invalid, "no cross-grant deficit");
	});
	Case("MediumAndMaximumGrantSweep", [] {
		const std::uint64_t Sizes[] = {1248, 8192, 65536, 262144, 524288};
		for (std::uint64_t Size : Sizes) {
			FiniteGrantServiceCurve Curve;
			Require(Curve.Activate(1, Size, 1000000), "activate legal grant");
			std::uint64_t Sent = 0, Time = 1000100;
			while (Sent < Size) {
				const std::uint64_t Bytes = std::min<std::uint64_t>(16384, Size - Sent);
				Require(Curve.FirstSend(1, Bytes, Time), "healthy packetized drain");
				Sent += Bytes;
				Time += 800;
			}
			Require(Curve.FirstSentBytes == Size && !Curve.ServiceFailed,
				"complete finite grant passes");
			Require(Curve.MaximumRunningDeficitByteMicroseconds <=
				FiniteGrantServiceCurve::RunningBoundByteMicroseconds,
				"healthy recurring drain passes");
		}
	});
	Case("SlowMaximumGrantFailsRecurringBeforeFinite", [] {
		FiniteGrantServiceCurve Curve;
		Require(Curve.Activate(1, 524288, 1000000), "activate maximum grant");
		Require(Curve.FirstSend(1, 16384, 1000100), "startup first-send");
		Require(!Curve.Observe(1001200) && Curve.GrantFailed && Curve.ServiceFailed,
			"sustained slow sender fails recurring bound");
		Require(Curve.MaximumFiniteShortfallByteMicroseconds == 0,
			"recurring failure precedes finite-envelope failure");
	});
	Case("FiniteBoundaryAndAtomicEvent", [] {
		FiniteGrantServiceCurve Passing;
		Require(Passing.Activate(1, 77, 0), "activate boundary grant");
		Require(Passing.Observe(6074), "no positive finite demand before boundary");
		Require(Passing.FirstSend(1, 77, 6074), "atomic first-send before boundary");
		FiniteGrantServiceCurve Late;
		Require(Late.Activate(1, 77, 0), "activate late grant");
		Require(!Late.FirstSend(1, 77, 6075) && Late.ServiceFailed,
			"exact pointwise envelope detects pre-event shortfall");
		Require(Late.FirstSentBytes == 77 && Late.CompletedAtMicroseconds == 6075,
			"late first-send still counts for conservation");
	});
	Case("TimestampTiesAndGrantLocalReset", [] {
		FiniteGrantServiceCurve Curve;
		Require(Curve.Activate(1, 8192, 1000000), "activate split grant");
		Require(Curve.FirstSend(1, 4096, 1000100), "first segment");
		Require(Curve.FirstSend(1, 4096, 1000100), "same-timestamp segment");
		Require(Curve.CompletedAtMicroseconds == 1000100 &&
			Curve.QualifiedRunningMicroseconds == 0,
			"timestamp tie creates no artificial elapsed service time");
		Require(Curve.Observe(2000000), "ACK and credit gap");
		Require(Curve.Activate(2, 77, 2000000) && Curve.FirstSend(2, 77, 2000020),
			"new grant starts a new contract");
		Require(!Curve.GrantFailed && !Curve.ServiceFailed &&
			Curve.MaximumRunningDeficitByteMicroseconds == 0,
			"grant-local running history reset");
		FiniteGrantServiceCurve Failed;
		Require(Failed.Activate(1, 77, 0), "activate failing grant");
		Require(!Failed.FirstSend(1, 77, 6075), "late grant fails");
		Require(Failed.Observe(900000) == false && Failed.ServiceFailed,
			"failure remains latched throughout ACK and credit gap");
		Require(Failed.Activate(2, 77, 1000000) && !Failed.GrantFailed &&
			!Failed.ServiceFailed && Failed.LastGrantFailed && Failed.EverFailed,
			"new independently admitted grant can requalify after recorded failure");
		Require(Failed.FirstSend(2, 77, 1000020) && !Failed.ServiceFailed,
			"healthy requalification grant restores service health");
	});
	Case("IdentityAndOverflow", [] {
		FiniteGrantServiceCurve Wrong;
		Require(Wrong.Activate(7, 100, 0), "activate identity test");
		Require(!Wrong.FirstSend(8, 100, 1) && Wrong.Invalid, "wrong token rejected");
		FiniteGrantServiceCurve TooLarge;
		Require(!TooLarge.Activate(1, 524289, 0) && TooLarge.Invalid,
			"oversize grant rejected");
		FiniteGrantServiceCurve Overflow;
		Require(Overflow.Activate(1, 100, 0), "activate overflow test");
		Require(!Overflow.Observe(UINT64_MAX) && Overflow.Invalid,
			"timestamp multiplication cannot wrap");
	});
	Case("ReferenceDifferential", [] {
		std::uint64_t Seed = 0x9e3779b97f4a7c15ULL;
		for (int Trial = 0; Trial < 400; ++Trial) {
			auto Next = [&]() -> std::uint64_t {
				Seed = Seed * 6364136223846793005ULL + 1442695040888963407ULL;
				return Seed >> 32;
			};
			const std::uint64_t Size = 1 + Next() % 50000;
			const int Events = 1 + static_cast<int>(Next() % 7);
			std::vector<Step> Steps;
			std::uint64_t Remaining = Size, Time = Next() % 7000;
			for (int Index = 0; Index < Events && Remaining; ++Index) {
				const std::uint64_t Bytes = Index + 1 == Events
					? Remaining : 1 + Next() % Remaining;
				Steps.push_back({Time, Bytes});
				Remaining -= Bytes;
				Time += Next() % 1500;
			}
			const std::uint64_t Horizon = Time + Next() % 2000;
			FiniteGrantServiceCurve Curve;
			Require(Curve.Activate(1, Size, 0), "activate generated grant");
			for (const Step &Event : Steps) Curve.FirstSend(1, Event.Bytes, Event.Time);
			Curve.Observe(Horizon);
			Require(!Curve.Invalid && Curve.ServiceFailed == ReferenceFailed(Size, Steps, Horizon),
				"optimized verdict differs from independent pointwise reference");
		}
	});
	std::cout << "[Network:FiniteGrant] passed=" << Passed << " total=8\n";
	return Passed == 8;
}
} // namespace FiniteGrantServiceCurveFixture
