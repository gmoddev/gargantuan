#include "ReliableServiceFeedback.hpp"
#include <clientlib/steamnetworkingsockets_connections.h>
#include <clientlib/csteamnetworkingsockets.h>

namespace SteamNetworkingSocketsLib {
// One forwarding hook in the pinned API translation unit retains its lookup
// and lock acquisition. All owned snapshot/capture logic is fully instrumented.
CSteamNetworkConnectionBase *GargantuanFindConnection(std::uint32_t Handle, ConnectionScopeLock &Lock);

void CSteamNetworkConnectionBase::GargantuanPopulateReliableServiceFeedback(GargantuanReliableServiceSnapshot &Result) const {
	Result.Counters = m_senderState.GargantuanFeedback;
	if (m_senderState.m_cbPendingReliable < 0 || m_senderState.m_cbSentUnackedReliable < 0)
		Result.Counters.Invalid = true;
	Result.PendingReliableStreamBytes = m_senderState.m_cbPendingReliable;
	Result.SentUnackedReliableStreamBytes = m_senderState.m_cbSentUnackedReliable;
	Result.NativeState = static_cast<int>(GetState());
}

// Borrowed only for the synchronous existing CloseConnection call on this thread.
// No new lock, callback, allocation, or persistent connection registry.
static thread_local GargantuanReliableServiceSnapshot *GargantuanClosingFeedback = nullptr;

void GargantuanCaptureClosingFeedback(const CSteamNetworkConnectionBase &Connection) {
	if (GargantuanClosingFeedback) Connection.GargantuanPopulateReliableServiceFeedback(*GargantuanClosingFeedback);
}

bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, GargantuanReliableServiceSnapshot &Result) {
	ConnectionScopeLock Lock;
	auto *Connection = GargantuanFindConnection(Handle, Lock);
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	Connection->GargantuanPopulateReliableServiceFeedback(Result);
	return !Result.Counters.Invalid;
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
