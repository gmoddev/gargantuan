#pragma once
#include <cstdint>
#include <mutex>

namespace SteamNetworkingSocketsLib {

// Private native DATA pacing, not an admission reservation or F1 allowance.
inline bool GargantuanOrdinaryDataIsPaced(bool AnyRunning, std::uint64_t ActiveToken,
	std::uint64_t LastToken, std::uint64_t GrantBytes, std::uint64_t FirstSentBytes) noexcept {
	return AnyRunning && (ActiveToken || LastToken) && GrantBytes <= FirstSentBytes;
}

inline std::int64_t GargantuanOrdinaryDataDeadline(std::int64_t DataDeadline,
	std::int64_t QueryNow, std::int64_t Eligible) noexcept {
	// An open gate must not turn ASAP into a fresh timestamp after Think's cutoff.
	return Eligible > QueryNow && Eligible > DataDeadline ? Eligible : DataDeadline;
}

class GargantuanOrdinaryWirePacer {
public:
	class Permit {
	public:
		Permit() = default;
		~Permit() { Cancel(); }
		Permit(const Permit &) = delete;
		Permit &operator=(const Permit &) = delete;
		bool Reserve(GargantuanOrdinaryWirePacer &Budget, std::int64_t Now);
		// Pure ACK/control packets and failed sends consume no DATA budget.
		bool Complete(int NativeBytes, bool HasData) noexcept;
		void Cancel() noexcept;
	private:
		GargantuanOrdinaryWirePacer *Owner = nullptr;
	};

	GargantuanOrdinaryWirePacer(std::uint64_t WireRate, std::uint64_t MaximumNativeBytes);
	std::int64_t EligibleAt(std::int64_t Now);
	std::uint64_t MaximumNativePacket() const noexcept { return MaximumNativeBytes; }
	std::uint64_t Rate() const noexcept { return WireRate; }
private:
	friend class Permit;
	static constexpr std::uint64_t Microseconds = 1000000;
	void Refill(std::int64_t Now) noexcept;
	void Refund(std::uint64_t WireBytes) noexcept;
	const std::uint64_t WireRate, MaximumNativeBytes, MaximumWireBytes, CreditCap;
	std::mutex Mutex;
	std::uint64_t Credit;
	std::int64_t LastRefill = 0;
};

// One process-wide bucket, shared by every marked POOLED connection/session.
// The native maximum comes from the pinned GNS constant at each call site.
GargantuanOrdinaryWirePacer &GargantuanSharedOrdinaryWirePacer(std::uint64_t MaximumNativeBytes);
std::uint64_t GargantuanOrdinaryWireRate() noexcept;

} // namespace SteamNetworkingSocketsLib
