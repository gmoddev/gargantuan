#include "ReliableServiceFeedback.hpp"
#include <steam/isteamnetworkingsockets.h>
#include <chrono>

namespace SteamNetworkingSocketsLib {
namespace {
thread_local std::uint64_t PendingReliableRetirementToken = 0;
}

bool GargantuanBeginReliableRetirementAttribution(std::uint64_t Token) noexcept {
	if (!Token || PendingReliableRetirementToken) return false;
	PendingReliableRetirementToken = Token;
	return true;
}

void GargantuanEndReliableRetirementAttribution() noexcept {
	PendingReliableRetirementToken = 0;
}

std::uint64_t GargantuanTakeReliableRetirementAttribution() noexcept {
	const auto Result = PendingReliableRetirementToken;
	PendingReliableRetirementToken = 0;
	return Result;
}

// Called from the native ownership hook while its existing lock is held.
void GargantuanCopyReliableServiceFeedback(const GargantuanReliableServiceCounters &Counters,
	int Pending, int Unacked, int NativeState, GargantuanReliableServiceSnapshot &Result) {
	Result.Counters = Counters;
	if (Pending < 0 || Unacked < 0) Result.Counters.Invalid = true;
	Result.PendingReliableStreamBytes = Pending;
	Result.SentUnackedReliableStreamBytes = Unacked;
	Result.NativeState = NativeState;
	// Stamp while the native connection lock still protects these values. A
	// delayed adapter return must not make an older sample appear fresh.
	const auto Time = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	if (Time < 0) Result.Counters.Invalid = true;
	else Result.ObservedAtMicroseconds = static_cast<std::uint64_t>(Time);
}

// Borrowed only for the synchronous existing CloseConnection call on this thread.
// No new lock, callback, allocation, or persistent connection registry.
static thread_local GargantuanReliableServiceSnapshot *GargantuanClosingFeedback = nullptr;

GargantuanReliableServiceSnapshot *GargantuanGetClosingFeedback() { return GargantuanClosingFeedback; }

bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, GargantuanReliableServiceSnapshot &Result) {
	return GargantuanReadNativeFeedback(Interface, Handle, Result) && !Result.Counters.Invalid;
}

bool GargantuanCloseWithReliableServiceFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, int Reason, const char *Diagnostic, GargantuanReliableServiceSnapshot &Result) {
	Result = {};
	Result.Counters.Invalid = true;
	if (!Interface) return false;
	struct CaptureScope {
		GargantuanReliableServiceSnapshot *Previous = GargantuanClosingFeedback;
		explicit CaptureScope(GargantuanReliableServiceSnapshot &Value) { GargantuanClosingFeedback = &Value; }
		~CaptureScope() { GargantuanClosingFeedback = Previous; }
	} Capture(Result);
	// Reuse the existing close operation, its existing lock order, and no-linger
	// semantics. Capture happens after purge under the SAME connection lock.
	return Interface->CloseConnection(Handle, Reason, Diagnostic, false);
}
}
