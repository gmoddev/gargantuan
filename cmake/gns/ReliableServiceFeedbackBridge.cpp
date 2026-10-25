#include "ReliableServiceFeedback.hpp"
#include "ServiceTimingDiagnostics.hpp"
#include <steam/isteamnetworkingsockets.h>
#include <chrono>
#include <array>
#include <mutex>

namespace SteamNetworkingSocketsLib {
namespace {
thread_local GargantuanReliableAttribution PendingReliableRetirement;
struct TerminalEvidenceSlot {
	ISteamNetworkingSockets *Interface = nullptr;
	std::uint32_t Handle = 0;
	std::uint64_t Generation = 0;
	bool Complete = false;
	GargantuanReliableServiceSnapshot Snapshot;
};
std::mutex TerminalEvidenceMutex;
std::array<TerminalEvidenceSlot, 64> TerminalEvidence;
std::uint64_t NextTerminalGeneration = 1;
}

std::uint32_t GargantuanNativeTerminalEvidenceCapacity() noexcept {
	return static_cast<std::uint32_t>(TerminalEvidence.size());
}

std::uint32_t GargantuanNativeTerminalEvidenceReserved() noexcept {
	GargantuanServiceCallsiteSpan Wait(GargantuanServiceCallsite::TerminalReserved);
	std::lock_guard<std::mutex> Lock(TerminalEvidenceMutex);
	Wait.Finish();
	std::uint32_t Count = 0;
	for (const auto &Slot : TerminalEvidence) if (Slot.Generation) ++Count;
	return Count;
}

bool GargantuanReserveNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t &Generation) noexcept {
	Generation = 0;
	if (!Interface || !Handle) return false;
	GargantuanServiceCallsiteSpan Wait(GargantuanServiceCallsite::TerminalReserve, Handle);
	std::lock_guard<std::mutex> Lock(TerminalEvidenceMutex);
	Wait.Finish();
	TerminalEvidenceSlot *Free = nullptr;
	for (auto &Slot : TerminalEvidence) {
		if (Slot.Generation && Slot.Interface == Interface && Slot.Handle == Handle) return false;
		if (!Slot.Generation && !Free) Free = &Slot;
	}
	if (!Free || !NextTerminalGeneration) return false;
	Generation = NextTerminalGeneration++;
	*Free = {Interface, Handle, Generation, false, {}};
	return true;
}

bool GargantuanStoreNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation,
	const GargantuanReliableServiceSnapshot &Result) noexcept {
	if (!Generation || !Result.Counters.Purged) return false;
	GargantuanServiceCallsiteSpan Wait(GargantuanServiceCallsite::TerminalStore, Handle);
	std::lock_guard<std::mutex> Lock(TerminalEvidenceMutex);
	Wait.Finish();
	for (auto &Slot : TerminalEvidence) {
		if (Slot.Interface == Interface && Slot.Handle == Handle && Slot.Generation == Generation) {
			if (Slot.Complete) return false;
			Slot.Snapshot = Result;
			Slot.Complete = true;
			return true;
		}
	}
	return false;
}

bool GargantuanTakeNativeTerminalFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation, GargantuanReliableServiceSnapshot &Result) {
	if (!Generation) return false;
	GargantuanServiceCallsiteSpan Wait(GargantuanServiceCallsite::TerminalTake, Handle);
	std::lock_guard<std::mutex> Lock(TerminalEvidenceMutex);
	Wait.Finish();
	for (auto &Slot : TerminalEvidence) {
		if (Slot.Interface == Interface && Slot.Handle == Handle && Slot.Generation == Generation) {
			if (!Slot.Complete) return false;
			Result = Slot.Snapshot;
			Slot = {};
			return true;
		}
	}
	return false;
}

bool GargantuanReleaseNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation) noexcept {
	if (!Generation) return false;
	GargantuanServiceCallsiteSpan Wait(GargantuanServiceCallsite::TerminalRelease, Handle);
	std::lock_guard<std::mutex> Lock(TerminalEvidenceMutex);
	Wait.Finish();
	for (auto &Slot : TerminalEvidence) {
		if (Slot.Interface == Interface && Slot.Handle == Handle && Slot.Generation == Generation) {
			Slot = {};
			return true;
		}
	}
	return false;
}

bool GargantuanBeginReliableRetirementAttribution(std::uint64_t Token,
	std::uint64_t ActivatedAtMicroseconds,
	const gargantuan::network::detail::InitialServiceTrace *InitialTrace) noexcept {
	if (!Token || PendingReliableRetirement.Token) return false;
	PendingReliableRetirement = {Token, ActivatedAtMicroseconds};
	if (InitialTrace && InitialTrace->Enabled && GargantuanInitialServiceTimingEnabled())
		PendingReliableRetirement.InitialTrace = *InitialTrace;
	return true;
}

bool GargantuanBeginReliableSubmissionMetadata(bool StructuralClass,
	std::uint64_t QualifiedSequence, std::uint64_t QualifiedPhase,
	std::uint64_t SchedulerAcceptedAtMicroseconds, bool ProtectedOrdinary,
	std::uint64_t ProtectedAcceptedAtMicroseconds, unsigned ProtectedOrigin) noexcept {
	if (PendingReliableRetirement.SubmissionMetadata) return false;
	PendingReliableRetirement.SubmissionMetadata = true;
	PendingReliableRetirement.StructuralClass = StructuralClass;
	PendingReliableRetirement.QualifiedSequence = QualifiedSequence;
	PendingReliableRetirement.QualifiedPhase = QualifiedPhase;
	PendingReliableRetirement.SchedulerAcceptedAtMicroseconds = SchedulerAcceptedAtMicroseconds;
	PendingReliableRetirement.ProtectedOrdinary = ProtectedOrdinary;
	PendingReliableRetirement.ProtectedAcceptedAtMicroseconds = ProtectedAcceptedAtMicroseconds;
	PendingReliableRetirement.ProtectedOrigin = ProtectedOrigin;
	return true;
}

bool GargantuanInitialServiceTimingEnabled() noexcept {
	return GargantuanServiceTimingObserver.load(std::memory_order_acquire) != nullptr;
}

std::uint64_t GargantuanInitialServiceClock() noexcept {
	return GargantuanInitialServiceTimingEnabled() ? GargantuanReliableServiceClock() : 0;
}

void GargantuanInitialServiceApiStage(bool Locked) noexcept {
	auto &Trace = PendingReliableRetirement.InitialTrace;
	if (!Trace.Enabled) return;
	(Locked ? Trace.NativeApiLocked : Trace.NativeApiEntered) = GargantuanInitialServiceClock();
}

void GargantuanEndReliableRetirementAttribution() noexcept {
	PendingReliableRetirement = {};
}

GargantuanReliableAttribution GargantuanTakeReliableRetirementAttribution() noexcept {
	const auto Result = PendingReliableRetirement;
	PendingReliableRetirement = {};
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
	else {
		Result.ObservedAtMicroseconds = static_cast<std::uint64_t>(Time);
		Result.Counters.ObserveActiveService(Result.ObservedAtMicroseconds);
		// Pending OS results were resolved before this snapshot. The copy's
		// deadline check never replaces a timely worker completion with Main time.
		Result.Counters.ObserveQualifiedOrdinary(Result.ObservedAtMicroseconds);
	}
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
