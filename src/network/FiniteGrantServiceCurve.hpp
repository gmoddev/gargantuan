#pragma once

#include <cstdint>
#include <limits>

namespace gargantuan { namespace network {

// F1 evaluates one already accepted structural grant. Admission, ACK, and
// retirement are separate clocks. All bounds are exact byte-microseconds.
struct FiniteGrantServiceCurve {
	static const std::uint64_t PeerRateBytesPerSecond = 16ULL * 1024 * 1024;
	static const std::uint64_t MaximumGrantBytes = 512ULL * 1024;
	static const std::uint64_t StartupMicroseconds = 5000;
	static const std::uint64_t RunningHandoffMicroseconds = 1000;
	static const std::uint64_t QuantumBytes = 1248;
	static const std::uint64_t FiniteInterceptByteMicroseconds =
		PeerRateBytesPerSecond * (StartupMicroseconds + RunningHandoffMicroseconds) +
		QuantumBytes * 1000000;
	static const std::uint64_t RunningBoundByteMicroseconds =
		PeerRateBytesPerSecond * RunningHandoffMicroseconds + QuantumBytes * 1000000;

	bool Invalid = false;
	// Failure is latched for this grant and its ACK gap. A later independently
	// admitted grant may requalify; EverFailed remains diagnostic history.
	bool ServiceFailed = false;
	bool GrantFailed = false;
	bool LastGrantFailed = false;
	bool EverFailed = false;
	std::uint64_t ActiveToken = 0;
	std::uint64_t LastToken = 0;
	std::uint64_t GrantBytes = 0;
	std::uint64_t FirstSentBytes = 0;
	std::uint64_t ActivatedAtMicroseconds = 0;
	std::uint64_t FirstSentAtMicroseconds = 0;
	std::uint64_t CompletedAtMicroseconds = 0;
	std::uint64_t QualifiedRunningMicroseconds = 0;
	std::uint64_t CurrentRunningDeficitByteMicroseconds = 0;
	std::uint64_t MaximumRunningDeficitByteMicroseconds = 0;
	std::uint64_t MaximumFiniteShortfallByteMicroseconds = 0;
	bool HasFirstSend = false;
	std::uint64_t LastObservedAtMicroseconds = 0;
	std::int64_t MinimumRunningBalanceByteMicroseconds = 0;

	bool Activate(std::uint64_t Token, std::uint64_t Bytes,
		std::uint64_t NowMicroseconds) noexcept {
		if (Invalid || Token == 0 || Token <= LastToken || Bytes == 0 ||
			Bytes > MaximumGrantBytes || (ActiveToken != 0 && FirstSentBytes != GrantBytes) ||
			(ActiveToken != 0 && NowMicroseconds < LastObservedAtMicroseconds)) {
			Invalid = true;
			return false;
		}
		ActiveToken = Token;
		LastToken = Token;
		GrantBytes = Bytes;
		FirstSentBytes = 0;
		ActivatedAtMicroseconds = NowMicroseconds;
		FirstSentAtMicroseconds = 0;
		CompletedAtMicroseconds = 0;
		QualifiedRunningMicroseconds = 0;
		CurrentRunningDeficitByteMicroseconds = 0;
		MaximumRunningDeficitByteMicroseconds = 0;
		MaximumFiniteShortfallByteMicroseconds = 0;
		MinimumRunningBalanceByteMicroseconds = 0;
		FirstSentBytesAtFirstEvent = 0;
		HasFirstSend = false;
		LastGrantFailed = GrantFailed;
		GrantFailed = ServiceFailed = false;
		LastObservedAtMicroseconds = NowMicroseconds;
		return true;
	}

	// Observe checks the entire open interval since the previous observation:
	// before the next first-send, D_g is constant and beta_g is nondecreasing.
	bool Observe(std::uint64_t NowMicroseconds) noexcept {
		if (Invalid || ActiveToken == 0 || NowMicroseconds < LastObservedAtMicroseconds) {
			Invalid = true;
			return false;
		}
		if (FirstSentBytes == GrantBytes) {
			LastObservedAtMicroseconds = NowMicroseconds;
			return !GrantFailed;
		}
		const std::uint64_t Elapsed = NowMicroseconds - ActivatedAtMicroseconds;
		if (Elapsed > std::numeric_limits<std::uint64_t>::max() / PeerRateBytesPerSecond) {
			Invalid = true;
			return false;
		}
		const std::uint64_t Accrued = Elapsed * PeerRateBytesPerSecond;
		const std::uint64_t FiniteDemand = Accrued > FiniteInterceptByteMicroseconds
			? Accrued - FiniteInterceptByteMicroseconds : 0;
		const std::uint64_t LimitedDemand = FiniteDemand < GrantBytes * 1000000
			? FiniteDemand : GrantBytes * 1000000;
		const std::uint64_t Served = FirstSentBytes * 1000000;
		if (LimitedDemand > Served) {
			const std::uint64_t Shortfall = LimitedDemand - Served;
			if (Shortfall > MaximumFiniteShortfallByteMicroseconds)
				MaximumFiniteShortfallByteMicroseconds = Shortfall;
			GrantFailed = ServiceFailed = EverFailed = true;
		}
		if (HasFirstSend) CheckRunning(NowMicroseconds);
		LastObservedAtMicroseconds = NowMicroseconds;
		return !GrantFailed;
	}

	bool FirstSend(std::uint64_t Token, std::uint64_t Bytes,
		std::uint64_t NowMicroseconds) noexcept {
		if (Invalid || ActiveToken == 0 || Token != ActiveToken || Bytes == 0 ||
			Bytes > GrantBytes - FirstSentBytes || NowMicroseconds < LastObservedAtMicroseconds) {
			Invalid = true;
			return false;
		}
		// A native first-send is atomic. Immediately before it, previous D_g
		// still applies; checking the pre-event endpoint prevents a long gap
		// from being erased by a late packet at the observation timestamp.
		Observe(NowMicroseconds);
		if (Invalid) return false;
		if (!HasFirstSend) {
			HasFirstSend = true;
			FirstSentAtMicroseconds = NowMicroseconds;
			FirstSentBytesAtFirstEvent = Bytes;
			MinimumRunningBalanceByteMicroseconds = 0;
		}
		FirstSentBytes += Bytes;
		if (FirstSentBytes == GrantBytes) {
			CompletedAtMicroseconds = NowMicroseconds;
			CurrentRunningDeficitByteMicroseconds = 0;
		} else if (!CheckRunning(NowMicroseconds)) return false;
		return !GrantFailed;
	}

private:
	bool CheckRunning(std::uint64_t NowMicroseconds) noexcept {
		const std::uint64_t Elapsed = NowMicroseconds - FirstSentAtMicroseconds;
		const std::uint64_t Served = (FirstSentBytes - FirstSentBytesAtFirstEvent) * 1000000;
		if (Elapsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
			PeerRateBytesPerSecond) {
			Invalid = true;
			return false;
		}
		const std::int64_t Balance =
			static_cast<std::int64_t>(Elapsed * PeerRateBytesPerSecond) -
			static_cast<std::int64_t>(Served);
		if (Balance > MinimumRunningBalanceByteMicroseconds) {
			const std::uint64_t Deficit = static_cast<std::uint64_t>(Balance) -
				static_cast<std::uint64_t>(MinimumRunningBalanceByteMicroseconds);
			CurrentRunningDeficitByteMicroseconds = Deficit;
			if (Deficit > MaximumRunningDeficitByteMicroseconds)
				MaximumRunningDeficitByteMicroseconds = Deficit;
			if (Deficit > RunningBoundByteMicroseconds)
				GrantFailed = ServiceFailed = EverFailed = true;
		} else {
			MinimumRunningBalanceByteMicroseconds = Balance;
			CurrentRunningDeficitByteMicroseconds = 0;
		}
		QualifiedRunningMicroseconds = Elapsed;
		return !GrantFailed;
	}

	std::uint64_t FirstSentBytesAtFirstEvent = 0;
};

}} // namespace gargantuan::network
