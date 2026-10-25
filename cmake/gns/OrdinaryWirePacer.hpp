#pragma once
#include <cstdint>
#include <array>
#include <mutex>

namespace SteamNetworkingSocketsLib {

// Private native DATA pacing, not an admission reservation or F1 allowance.
inline bool GargantuanOrdinaryDataIsPaced(bool AnyRunning, std::uint64_t ActiveToken,
	std::uint64_t LastToken, std::uint64_t GrantBytes, std::uint64_t FirstSentBytes,
	bool RetentionBoundedCandidate = false) noexcept {
	// A completed but ACK-retained grant can still retransmit DATA after every
	// active send lease has returned. Keep that DATA on the same finite shared
	// budget; an inter-wave gap must not create 32 unpaced retry senders.
	// Candidate-only until protected first-send/retry arbitration is proved.
	// Existing production call sites omit this flag and retain V1 behavior.
	return (AnyRunning || (RetentionBoundedCandidate && ActiveToken)) &&
		(ActiveToken || LastToken) && GrantBytes <= FirstSentBytes;
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
		bool Reserve(GargantuanOrdinaryWirePacer &Budget, std::int64_t Now, std::uint32_t Flow = 0);
		// Pure ACK/control packets and failed sends consume no DATA budget.
		bool Complete(int NativeBytes, bool HasData) noexcept;
		void Cancel() noexcept;
	private:
		GargantuanOrdinaryWirePacer *Owner = nullptr;
	};

	GargantuanOrdinaryWirePacer(std::uint64_t WireRate, std::uint64_t MaximumNativeBytes);
	std::int64_t EligibleAt(std::int64_t Now, std::uint32_t Flow = 0);
	void Forget(std::uint32_t Flow) noexcept;
	void Suspend(std::uint32_t Flow) noexcept;
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
	// Only the opt-in native policy uses this bounded age-ordered request FIFO.
	// Ineligible requests retain age but cannot hold the ready head. A successful
	// reservation removes its request before native dispatch.
	std::array<std::uint32_t, 32> Flows{};
	std::array<bool, 32> Ready{};
	unsigned FlowCount = 0;
	bool Request(std::uint32_t Flow) noexcept;
	unsigned FirstReady() const noexcept;
};

// One process-wide bucket, shared by every marked POOLED connection/session.
// The native maximum comes from the pinned GNS constant at each call site.
GargantuanOrdinaryWirePacer &GargantuanSharedOrdinaryWirePacer(std::uint64_t MaximumNativeBytes);
std::uint64_t GargantuanOrdinaryWireRate() noexcept;

enum class GargantuanDataClass : std::uint8_t { None, Legacy, Structural, Protected, Retained };
// Fixed subdivision of the existing 24-MiB/s wire allocation. Both gameplay
// and control use Protected; retransmissions never consume its reservation.
inline constexpr std::uint64_t GargantuanProtectedWireRate = 21ULL * 1024 * 1024;
inline constexpr std::uint64_t GargantuanRetainedWireRate = 3ULL * 1024 * 1024;
inline constexpr unsigned GargantuanProtectedPacketQuantum = 7;
GargantuanOrdinaryWirePacer &GargantuanProtectedWirePacer(std::uint64_t MaximumNativeBytes);
GargantuanOrdinaryWirePacer &GargantuanRetainedWirePacer(std::uint64_t MaximumNativeBytes);
inline GargantuanDataClass GargantuanSelectDataClass(bool Structural, bool Protected,
	bool Retained, bool ProtectedReady, bool RetainedReady, unsigned ProtectedPackets) noexcept {
	if (Structural) return GargantuanDataClass::Structural;
	if (Retained && RetainedReady && (!Protected || ProtectedPackets >= GargantuanProtectedPacketQuantum))
		return GargantuanDataClass::Retained;
	if (Protected && ProtectedReady) return GargantuanDataClass::Protected;
	if (Retained && RetainedReady) return GargantuanDataClass::Retained;
	return GargantuanDataClass::None;
}
inline void GargantuanCommitDataClass(unsigned &ProtectedPackets, GargantuanDataClass Class,
	bool PositiveData) noexcept {
	if (!PositiveData) return;
	if (Class == GargantuanDataClass::Retained) ProtectedPackets = 0;
	if (Class == GargantuanDataClass::Protected && ProtectedPackets < GargantuanProtectedPacketQuantum)
		++ProtectedPackets;
}

} // namespace SteamNetworkingSocketsLib
