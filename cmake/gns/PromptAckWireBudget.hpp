#pragma once
#include <cstdint>
#include <limits>

// Transport-only, optional ACK acceleration eligibility. Denial never rejects
// a legal grant or changes its F1 clock. The caller supplies the independently
// justified tail budget; this helper does not claim to bound future control.
struct GargantuanPromptAckWireBudget {
	std::uint64_t Token = 0, Bytes = 0, WireBytes = 0;
	std::uint64_t Reserve = 0, StructuralPool = 0, TailBudget = 0;
	std::uint64_t BackgroundRate = 0, BackgroundBurst = 0;
	bool Invalid = false;
	static bool AddChecked(std::uint64_t Left, std::uint64_t Right, std::uint64_t &Result) noexcept {
		if (Right > std::numeric_limits<std::uint64_t>::max() - Left) return false;
		Result = Left + Right;
		return true;
	}
	static bool MultiplyChecked(std::uint64_t Left, std::uint64_t Right, std::uint64_t &Result) noexcept {
		if (Right && Left > std::numeric_limits<std::uint64_t>::max() / Right) return false;
		Result = Left * Right;
		return true;
	}
	// Healthy periodic control only: request + confirmation, at each pinned
	// minimum interval, for both endpoints of every peer. Loss/retries are NOT
	// covered by these timers and remain actual whole-wire charges.
	bool ConfigureBackground(std::uint64_t AvailableReserve, std::uint64_t Peers, std::uint64_t MaximumPacketWire,
		std::uint64_t PingSeconds, std::uint64_t InstantSeconds, std::uint64_t LifetimeSeconds) noexcept {
		Reserve = 0; BackgroundRate = 0; BackgroundBurst = 0;
		if (!Peers || !MaximumPacketWire || !PingSeconds || !InstantSeconds || !LifetimeSeconds) return false;
		const std::uint64_t Periods[] = {PingSeconds, InstantSeconds, LifetimeSeconds};
		std::uint64_t Common = 1;
		for (const auto Period : Periods) {
			std::uint64_t A = Common, B = Period;
			while (B) { const auto Remainder = A % B; A = B; B = Remainder; }
			if (!MultiplyChecked(Common / A, Period, Common)) return false;
		}
		std::uint64_t Events = 0, PairEndpointBytes = 0, Numerator = 0;
		for (const auto Period : Periods) if (!AddChecked(Events, Common / Period, Events)) return false;
		if (!MultiplyChecked(Peers, 2, PairEndpointBytes) ||
			!MultiplyChecked(PairEndpointBytes, MaximumPacketWire, PairEndpointBytes) ||
			!MultiplyChecked(PairEndpointBytes, 2, PairEndpointBytes) ||
			!MultiplyChecked(PairEndpointBytes, Events, Numerator) ||
			!MultiplyChecked(PairEndpointBytes, 3, BackgroundBurst)) return false;
		BackgroundRate = Numerator / Common;
		if (Numerator % Common && !AddChecked(BackgroundRate, 1, BackgroundRate)) return false;
		if (BackgroundRate > AvailableReserve) return false;
		Reserve = AvailableReserve - BackgroundRate;
		return true;
	}
	void Begin(std::uint64_t NextToken, std::uint64_t Size) noexcept {
		Token = NextToken; Bytes = Size; WireBytes = 0;
		Invalid = !Token || !Bytes;
	}
	void Charge(std::uint64_t DatagramBytes, std::uint64_t HeaderBytes) noexcept {
		std::uint64_t PacketBytes = 0;
		if (!Token || Invalid) return;
		if (!DatagramBytes || !AddChecked(DatagramBytes, HeaderBytes, PacketBytes) ||
			!AddChecked(WireBytes, PacketBytes, WireBytes)) Invalid = true;
	}
	bool Ceiling(std::uint64_t &Result) const noexcept {
		if (Invalid || !Token || !Bytes || !StructuralPool || !TailBudget ||
			(Reserve && Bytes > std::numeric_limits<std::uint64_t>::max() / Reserve)) return false;
		return AddChecked(Bytes, Bytes * Reserve / StructuralPool, Result);
	}
	bool CanRequest(std::uint64_t FinalPacketMaximumWireBytes) const noexcept {
		std::uint64_t Limit = 0, Cost = 0;
		return FinalPacketMaximumWireBytes && Ceiling(Limit) &&
			AddChecked(WireBytes, FinalPacketMaximumWireBytes, Cost) &&
			AddChecked(Cost, TailBudget, Cost) && Cost <= Limit;
	}
};
