#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace FiniteGrantServiceCurveFixture {
using gargantuan::network::FiniteGrantServiceCurve;
using gargantuan::network::NativeGrantCompletionCurve;

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

// D02 candidate arithmetic only. This fixture reference consumes synthetic
// unique-native events and an explicitly resolved evidence frontier. It does
// not replace the production F1 evaluator or qualify a host execution model.
enum class CandidateH1Verdict { CompletedTimely, ActiveWithinBudget, DeadlineMiss, IncompleteFeedback, Invalid };

inline CandidateH1Verdict CandidateH1(std::uint64_t GrantBytes, std::uint64_t Activated,
	const std::vector<Step> &NativeEvents, std::uint64_t ResolvedThrough, bool FeedbackComplete = true) {
	constexpr std::uint64_t Rate = 16'777'216;
	constexpr std::uint64_t BudgetByteMicroseconds = Rate * 6'000 + (524'288 + 1'248) * 1'000'000ULL;
	if (!GrantBytes || GrantBytes > 524'288 || !Activated || Activated == UINT64_MAX ||
		ResolvedThrough < Activated || NativeEvents.size() > 512)
		return CandidateH1Verdict::Invalid;
	std::uint64_t Last = Activated, Sent = 0, Completed = 0;
	bool CompletionPresent = false;
	for (const auto &Event : NativeEvents) {
		if (!Event.Bytes || Event.Bytes > GrantBytes - Sent || Event.Time < Last || Event.Time > ResolvedThrough)
			return CandidateH1Verdict::Invalid;
		Sent += Event.Bytes; Last = Event.Time;
		if (Sent == GrantBytes) { Completed = Event.Time; CompletionPresent = true; }
	}
	// An unresolved result cannot prove either timely service or an exact
	// deadline miss. A genuinely resolved late observation may carry an earlier
	// positive native completion, which retains its actual event timestamp.
	if (!FeedbackComplete) return CandidateH1Verdict::IncompleteFeedback;
	const auto End = CompletionPresent ? Completed : ResolvedThrough;
	const auto Elapsed = End - Activated;
	if (Elapsed > UINT64_MAX / Rate) return CandidateH1Verdict::Invalid;
	if (Rate * Elapsed > BudgetByteMicroseconds) return CandidateH1Verdict::DeadlineMiss;
	return CompletionPresent ? CandidateH1Verdict::CompletedTimely : CandidateH1Verdict::ActiveWithinBudget;
}

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
	Case("CandidateUniformCompletionBoundary", [] {
		constexpr std::uint64_t Activated = 1'000'000;
		for (const auto Bytes : {77ULL, 1'057ULL, 65'536ULL, 524'288ULL}) {
			Require(CandidateH1(Bytes, Activated, {{Activated + 37'324, Bytes}}, Activated + 37'324) ==
				CandidateH1Verdict::CompletedTimely, "candidate uniform exact lower boundary");
			Require(CandidateH1(Bytes, Activated, {{Activated + 37'325, Bytes}}, Activated + 37'325) ==
				CandidateH1Verdict::DeadlineMiss, "candidate uniform exact upper boundary");
		}
		Require(CandidateH1(1'057, Activated, {{Activated + 7'070, 1'057}}, Activated + 7'070) ==
			CandidateH1Verdict::CompletedTimely, "candidate explicitly permits historical tiny completion timing");
		FiniteGrantServiceCurve Existing;
		Require(Existing.Activate(1, 1'057, Activated) && !Existing.FirstSend(1, 1'057, Activated + 7'070) &&
			Existing.ServiceFailed, "unchanged production F1 continues to fail the same timing");
	});
	Case("CandidateMissingFirstAndFinalByte", [] {
		constexpr std::uint64_t Activated = 2'000'000;
		Require(CandidateH1(77, Activated, {}, Activated + 37'324) == CandidateH1Verdict::ActiveWithinBudget,
			"known no-first-send prefix before deadline is not a completion certificate");
		Require(CandidateH1(77, Activated, {}, Activated + 37'325) == CandidateH1Verdict::DeadlineMiss,
			"known no first-send fails without waiting for later completion");
		Require(CandidateH1(524'288, Activated, {{Activated + 100, 524'287}}, Activated + 37'325) ==
			CandidateH1Verdict::DeadlineMiss, "prompt first-send cannot conceal withheld final byte");
		Require(CandidateH1(524'288, Activated,
			{{Activated + 100, 524'287}, {Activated + 37'325, 1}}, Activated + 40'000) ==
			CandidateH1Verdict::DeadlineMiss, "late last byte cannot repair the deadline");
	});
	Case("CandidateNativeTimeAndEvidenceFrontier", [] {
		constexpr std::uint64_t Activated = 3'000'000;
		Require(CandidateH1(77, Activated, {{Activated + 37'324, 77}}, Activated + 100'000) ==
			CandidateH1Verdict::CompletedTimely, "late actor feedback preserves genuinely timely native completion");
		Require(CandidateH1(77, Activated, {}, Activated + 100'000, false) ==
			CandidateH1Verdict::IncompleteFeedback, "unresolved feedback cannot manufacture an exact failure");
		Require(CandidateH1(77, Activated, {{Activated + 37'324, 77}}, Activated + 100'000, false) ==
			CandidateH1Verdict::IncompleteFeedback, "unresolved feedback cannot manufacture a completion PASS");
		Require(CandidateH1(77, Activated, {{Activated - 1, 77}}, Activated + 100) == CandidateH1Verdict::Invalid &&
			CandidateH1(77, Activated, {{Activated + 20, 76}, {Activated + 10, 1}}, Activated + 100) ==
				CandidateH1Verdict::Invalid &&
			CandidateH1(77, Activated, {{Activated + 101, 77}}, Activated + 100) == CandidateH1Verdict::Invalid,
			"pre-activation, reversed and future native event chronology is invalid");
		Require(CandidateH1(77, Activated, {{Activated + 1, 78}}, Activated + 100) == CandidateH1Verdict::Invalid &&
			CandidateH1(0, Activated, {}, Activated) == CandidateH1Verdict::Invalid &&
			CandidateH1(524'289, Activated, {}, Activated) == CandidateH1Verdict::Invalid &&
			CandidateH1(77, Activated, {}, Activated - 1) == CandidateH1Verdict::Invalid &&
			CandidateH1(77, 0, {}, 100) == CandidateH1Verdict::Invalid &&
			CandidateH1(77, UINT64_MAX, {}, UINT64_MAX) == CandidateH1Verdict::Invalid &&
			CandidateH1(77, 1, {}, UINT64_MAX) == CandidateH1Verdict::Invalid,
			"false bytes, invalid grant, reversed frontier and overflow cannot qualify");
	});
	Case("CandidateSharedProtectedFifoBudget", [] {
		// All stages belong to one follower's submission-to-native-completion
		// interval. Passing each stage against a renewed allowance is unsound.
		auto FollowerWithinBudget = [](std::uint64_t Submitted, std::uint64_t Completed) {
			return Completed >= Submitted && Completed - Submitted <= 50'000;
		};
		Require(FollowerWithinBudget(1'000'000, 1'050'000) && !FollowerWithinBudget(1'000'000, 1'050'001),
			"one protected FIFO budget has an exact 50-ms boundary");
		Require(30'000 <= 50'000 && 30'000 <= 50'000 && !FollowerWithinBudget(1'000'000, 1'060'000),
			"two individually acceptable serial stages cannot receive two queue allowances");
		Require(!FollowerWithinBudget(1'000'000, 999'999), "reversed follower timestamps cannot qualify");
		constexpr std::uint64_t Rate = 16'777'216;
		constexpr std::uint64_t StructuralByteMicroseconds = Rate * 6'000 + (524'288 + 1'248) * 1'000'000ULL;
		constexpr std::uint64_t FollowerByteMicroseconds = 20'480 * 1'000'000ULL;
		constexpr std::uint64_t Combined = StructuralByteMicroseconds + FollowerByteMicroseconds;
		Require(Combined <= Rate * 50'000 && Combined + Rate * 100'000 <= Rate * 150'000,
			"existing maximum structural budget plus 20-KiB follower fits optimistic FIFO and RPC p95 budgets");
		Require(!FollowerWithinBudget(1'000'000, 1'000'000 + Combined / Rate + 1 + 12'000),
			"additional prefix or service delay remains inside the same FIFO budget");
	});
	Case("CandidateNativeAndDeliveryDeadlinesRemainSeparate", [] {
		auto Within = [](std::uint64_t Started, std::uint64_t Finished, std::uint64_t Budget) {
			return Finished >= Started && Finished - Started <= Budget;
		};
		constexpr std::uint64_t Submitted = 1'000'000;
		Require(Within(Submitted, Submitted + 49'000, 50'000) &&
			!Within(Submitted, Submitted + 250'001, 250'000),
			"timely native follower cannot hide a missed Event ACK or action-result maximum");
		Require(!Within(Submitted, Submitted + 60'000, 50'000) &&
			Within(Submitted, Submitted + 65'000, 250'000),
			"later timely delivery cannot repair a missed native FIFO deadline");
		Require(Within(Submitted, Submitted + 250'000, 250'000) &&
			!Within(Submitted, Submitted + 250'001, 250'000), "unchanged Event/action maximum has no substituted allowance");
		// ACK/retirement and protected wire service still require the existing
		// ownership and real transport fixtures; arithmetic does not prove them.
	});
	Case("NativeCompletionUniformExactBoundaries", [] {
		for (const auto Bytes : {77ULL, 1'057ULL, 65'536ULL, 524'288ULL}) {
			NativeGrantCompletionCurve OnTime, Late;
			Require(OnTime.Activate(1, Bytes, 1'000'000) && OnTime.FirstSend(1, Bytes, 1'037'324) &&
				OnTime.CompletedAtMicroseconds == 1'037'324, "hard uniform completion lower boundary");
			Require(Late.Activate(1, Bytes, 1'000'000) && !Late.FirstSend(1, Bytes, 1'037'325) &&
				!Late.Invalid && Late.GrantFailed && Late.FirstSentBytes == Bytes && Late.CompletedAtMicroseconds == 1'037'325,
				"hard late completion fails truthfully without discarding native bytes");
		}
	});
	Case("NativeCompletionDetectsMissingFirstAndFinalService", [] {
		NativeGrantCompletionCurve Missing;
		Require(Missing.Activate(1, 77, 1'000'000) && Missing.Observe(1'037'324) &&
			!Missing.CompletedAtMicroseconds && !Missing.Observe(1'037'325) && Missing.GrantFailed,
			"known missing first send fails while still active");
		NativeGrantCompletionCurve Final;
		Require(Final.Activate(1, 65'536, 1'000'000) && Final.FirstSend(1, 65'535, 1'000'100) &&
			!Final.Observe(1'037'325) && !Final.CompletedAtMicroseconds,
			"native prefix cannot hide an overdue final byte");
		Require(!Final.FirstSend(1, 1, 1'038'000) && Final.CompletedAtMicroseconds == 1'038'000,
			"later delivery cannot repair a resolved deadline failure");
	});
	Case("NativeMaximumGrantUniformDeadlineDoesNotInferPerPeerRate", [] {
		for (const auto Duration : {31'250ULL, 31'251ULL}) {
			NativeGrantCompletionCurve Curve;
			Require(Curve.Activate(1, 524'288, 1'000'000) && Curve.FirstSend(1, 1'248, 1'000'100),
				"maximum grant has an explicit first-positive boundary");
			const auto Passed = Curve.FirstSend(1, 524'288 - 1'248, 1'000'100 + Duration);
			Require(Passed && !Curve.Invalid && Curve.CompletedAtMicroseconds && !Curve.GrantFailed,
				"31.25-ms design rate is not an additional hard completion deadline");
		}
		NativeGrantCompletionCurve Slow;
		constexpr std::uint64_t Remaining = 524'288 - 1'248, SlowRate = 14 * 1'024 * 1'024;
		constexpr std::uint64_t Duration = (Remaining * 1'000'000 + SlowRate - 1) / SlowRate;
		Require(Slow.Activate(1, 524'288, 1'000'000) && Slow.FirstSend(1, 1'248, 1'000'100) &&
			Duration + 100 < 37'324 && Slow.FirstSend(1, Remaining, 1'000'100 + Duration) && !Slow.GrantFailed,
			"prompt 14-MiB/s per-peer completion is accepted when actual hard deadline is met");
	});
	Case("LegacyMicroTimingDoesNotChangeNativeCompletionCertificate", [] {
		FiniteGrantServiceCurve Legacy;
		NativeGrantCompletionCurve Hard;
		Require(Legacy.Activate(1, 65'536, 1'000'000) && Hard.Activate(1, 65'536, 1'000'000) &&
			Legacy.FirstSend(1, 100, 1'000'100) && Hard.FirstSend(1, 100, 1'000'100), "shared native chronology");
		Require(!Legacy.FirstSend(1, 65'436, 1'005'000) && Legacy.ServiceFailed &&
			Hard.FirstSend(1, 65'436, 1'005'000) && !Hard.GrantFailed,
			"a recovered micro-timing gap preserves legacy failure and a passing hard certificate separately");
	});
	Case("NativeCompletionRejectsInvalidIdentityAndChronology", [] {
		NativeGrantCompletionCurve Wrong, Reverse, Empty, Oversize, Zero, Bootstrap, Overflow, Reuse;
		Require(Wrong.Activate(1, 77, 1'000'000) && !Wrong.FirstSend(2, 77, 1'000'001) && Wrong.Invalid,
			"wrong native grant token is hard-invalid");
		Require(Reverse.Activate(1, 77, 1'000'000) && Reverse.Observe(1'000'100) &&
			!Reverse.FirstSend(1, 77, 1'000'099) && Reverse.Invalid,
			"native result cannot backfill an already resolved observation frontier");
		Require(Empty.Activate(1, 77, 1'000'000) && !Empty.FirstSend(1, 0, 1'000'001) && Empty.Invalid &&
			Oversize.Activate(1, 77, 1'000'000) && !Oversize.FirstSend(1, 78, 1'000'001) && Oversize.Invalid,
			"zero or excess unique bytes cannot qualify native service");
		Require(!Zero.Activate(1, 77, 0) && Zero.Invalid && !Bootstrap.Activate(1, 77, UINT64_MAX) && Bootstrap.Invalid,
			"unqualified activation sentinels cannot start a hard curve");
		Require(Overflow.Activate(1, 77, 1) && !Overflow.Observe(UINT64_MAX) && Overflow.Invalid,
			"completion arithmetic cannot overflow");
		Require(Reuse.Activate(1, 77, 1'000'000) && !Reuse.Activate(2, 77, 1'000'001) && Reuse.Invalid,
			"another grant cannot replace unresolved native service");
	});
	Case("NativeCompletionLateFeedbackAndRetirementRemainSeparate", [] {
		NativeGrantCompletionCurve Curve;
		Require(Curve.Activate(1, 1'057, 1'000'000) && Curve.FirstSend(1, 1'057, 1'007'070), "timely tiny hard completion");
		Require(Curve.Observe(1'200'000) && Curve.CompletedAtMicroseconds == 1'007'070 && !Curve.GrantFailed,
			"late complete feedback or delayed ACK cannot change the actual native-completion certificate");
		Require(Curve.Activate(2, 77, 1'250'000) && !Curve.FirstSend(2, 77, 1'287'325) && Curve.EverFailed,
			"a later hard failure is retained independently");
		Require(Curve.Activate(3, 77, 1'300'000) && Curve.FirstSend(3, 77, 1'300'100) &&
			!Curve.GrantFailed && Curve.EverFailed, "new grant health does not erase historical hard failures");
	});
	std::cout << "[Network:FiniteGrant] passed=" << Passed << " total=19\n";
	return Passed == 19;
}
} // namespace FiniteGrantServiceCurveFixture
