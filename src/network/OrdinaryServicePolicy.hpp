#pragma once
#include "../runtime/OrdinaryServiceMetadata.hpp"
#include <cstdint>
#include <memory>
#include <thread>

namespace gargantuan::network::detail {
// Prospective membership of the existing canonical combined ordinary source
// envelope. These allocations do not throttle accepted traffic or promise a
// delivered rate. BODY includes the existing 32-byte adapter envelope.
struct OrdinarySourceBucket {
	std::uint64_t Rate, Cap, Credit, Updated = 0, Fraction = 0;
	OrdinarySourceBucket(std::uint64_t RateValue, std::uint64_t CapValue)
		: Rate(RateValue), Cap(CapValue), Credit(CapValue) {}
	bool Advance(std::uint64_t At) noexcept {
		if (!At || (Updated && At < Updated)) return false;
		if (!Updated) { Updated = At; return true; }
		const auto Elapsed = At - Updated; Updated = At;
		// Each canonical cap fits one second of its refill. The short product
		// below is at most 262144*999999+999999, without elapsed-time overflow.
		if (Elapsed >= 1'000'000) { Credit = Cap; Fraction = 0; return true; }
		const auto Numerator = Rate * Elapsed + Fraction;
		const auto Added = Numerator / 1'000'000;
		Fraction = Numerator % 1'000'000;
		if (Added >= Cap - Credit) { Credit = Cap; Fraction = 0; }
		else Credit += Added;
		return true;
	}
};
class OrdinaryServiceBudget final {
	friend class OrdinaryServicePolicy;
	const std::thread::id Owner = std::this_thread::get_id();
	OrdinarySourceBucket Messages{512, 128}, Bytes{262'144, 163'840};
	bool Normal = true;
public:
	bool OnOwnerThread() const noexcept { return Owner == std::this_thread::get_id(); }
	bool SetNormal(bool Value) noexcept {
		if (!OnOwnerThread()) return false;
		Normal = Value; // Scope changes never refill or recreate source budgets.
		return true;
	}
};
class OrdinaryServicePolicy final {
	std::shared_ptr<OrdinaryServiceBudget> Global;
	OrdinarySourceBucket Messages{64, 16}, Bytes{32'768, 20'480};
	bool Ready = false;
public:
	explicit OrdinaryServicePolicy(std::shared_ptr<OrdinaryServiceBudget> Value) : Global(std::move(Value)) {}
	OrdinaryServicePolicy(const OrdinaryServicePolicy &) = delete;
	OrdinaryServicePolicy &operator=(const OrdinaryServicePolicy &) = delete;
	OrdinaryServicePolicy(OrdinaryServicePolicy &&) noexcept = default;
	OrdinaryServicePolicy &operator=(OrdinaryServicePolicy &&) noexcept = default;
	bool OnOwnerThread() const noexcept { return Global && Global->OnOwnerThread(); }
	bool SetReady(bool Value) noexcept {
		if (!OnOwnerThread()) return false;
		Ready = Value;
		return true;
	}
	// Prepare does not charge a submission that might WouldBlock. All declared
	// source access is Main-affine; direct accepted submissions cannot reorder
	// their entry timestamps through concurrent API callers.
	runtime_detail::OrdinaryServiceMetadata Prepare(std::uint64_t CompleteBytes,
		std::uint8_t Origin, std::uint64_t At) noexcept {
		runtime_detail::OrdinaryServiceMetadata Result{At, Origin, false};
		if (!OnOwnerThread() || !Ready || !Global->Normal || !CompleteBytes || Origin > 6 ||
			!Messages.Advance(At) || !Bytes.Advance(At) ||
			!Global->Messages.Advance(At) || !Global->Bytes.Advance(At)) return Result;
		Result.Protected = Messages.Credit && Global->Messages.Credit &&
			CompleteBytes <= Bytes.Credit && CompleteBytes <= Global->Bytes.Credit;
		return Result;
	}
	bool Commit(std::uint64_t CompleteBytes, runtime_detail::OrdinaryServiceMetadata Accepted) noexcept {
		if (!Accepted.AcceptedAtMicroseconds || Accepted.Origin > 6 || !CompleteBytes) return false;
		if (!Accepted.Protected) return true;
		if (!OnOwnerThread()) return false;
		// Do not reconsider the prepared membership on a later scope change.
		// The Main-owned synchronous send has no host-driven callback/reentrancy.
		if (!Messages.Credit || !Global->Messages.Credit || CompleteBytes > Bytes.Credit ||
			CompleteBytes > Global->Bytes.Credit) return false;
		--Messages.Credit; --Global->Messages.Credit;
		Bytes.Credit -= CompleteBytes; Global->Bytes.Credit -= CompleteBytes;
		return true;
	}
	runtime_detail::OrdinaryServiceMetadata Accept(std::uint64_t CompleteBytes,
		std::uint8_t Origin, std::uint64_t At) noexcept {
		auto Result = Prepare(CompleteBytes, Origin, At);
		if (!Commit(CompleteBytes, Result)) Result.Protected = false;
		return Result;
	}
};
}
