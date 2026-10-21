# Exact pinned-source observation hooks. Preserve original/last-applied text so
# reconfigure is idempotent, rejects unknown edits, and avoids timestamp churn.
set(GargantuanFeedbackDirectory "${CMAKE_CURRENT_LIST_DIR}")
function(GargantuanReadFeedbackSource Name ExpectedHash)
	set(Path "${gamenetworkingsockets_SOURCE_DIR}/src/steamnetworkingsockets/clientlib/${Name}")
	file(READ "${Path}" Current)
	string(REPLACE "\r\n" "\n" Current "${Current}")
	if(EXISTS "${Path}.gargantuan-feedback-original")
		file(READ "${Path}.gargantuan-feedback-original" Original)
	else()
		set(Original "${Current}")
	endif()
	string(SHA256 ActualHash "${Original}")
	if(NOT ActualHash STREQUAL ExpectedHash)
		message(FATAL_ERROR "Pinned GNS feedback input mismatch: ${Name}")
	endif()
	if(NOT Current STREQUAL Original)
		if(NOT EXISTS "${Path}.gargantuan-feedback-applied")
			message(FATAL_ERROR "Unknown GNS feedback modification: ${Name}")
		endif()
		file(READ "${Path}.gargantuan-feedback-applied" Previous)
		if(NOT Current STREQUAL Previous)
			message(FATAL_ERROR "Concurrent GNS feedback modification: ${Name}")
		endif()
	endif()
	set(GnsFeedbackPath "${Path}" PARENT_SCOPE)
	set(GnsFeedbackOriginal "${Original}" PARENT_SCOPE)
	set(GnsFeedbackCurrent "${Current}" PARENT_SCOPE)
	set(GnsFeedbackSource "${Original}" PARENT_SCOPE)
endfunction()
macro(GargantuanReplaceFeedback Old New)
	string(FIND "${GnsFeedbackSource}" "${Old}" Position)
	if(Position EQUAL -1)
		message(FATAL_ERROR "Pinned GNS feedback transition missing in ${GnsFeedbackPath}")
	endif()
	string(REPLACE "${Old}" "${New}" GnsFeedbackSource "${GnsFeedbackSource}")
endmacro()
macro(GargantuanWriteFeedbackSource)
	if(NOT EXISTS "${GnsFeedbackPath}.gargantuan-feedback-original")
		file(WRITE "${GnsFeedbackPath}.gargantuan-feedback-original" "${GnsFeedbackOriginal}")
	endif()
	if(NOT GnsFeedbackCurrent STREQUAL GnsFeedbackSource)
		file(WRITE "${GnsFeedbackPath}" "${GnsFeedbackSource}")
		file(WRITE "${GnsFeedbackPath}.gargantuan-feedback-applied" "${GnsFeedbackSource}")
	endif()
endmacro()

GargantuanReadFeedbackSource(steamnetworkingsockets_snp.h 35a8d2721334f5632e042f3165095dcae90ced590b78392cc0f4346fceaba170)
GargantuanReplaceFeedback("#pragma once" "#pragma once\n#include \"ReliableServiceFeedback.hpp\"")
GargantuanReplaceFeedback("struct SSNPSenderState\n{" "struct SSNPSenderState\n{\n\tGargantuanReliableServiceCounters GargantuanFeedback;\n\tbool GargantuanRunningStructuralGrant = false;")
GargantuanReplaceFeedback("\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;" "\t// A failed SendEncryptedDataChunk queues retry without first-send service.\n\tbool m_bGargantuanEverSent;\n\n\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_snp.cpp 99e2b190b17993139bd3251f8862b81b58903a119ea456edacfec68f3dd9d65c)
GargantuanReplaceFeedback("void SSNPSenderState::Shutdown()\n{" "void SSNPSenderState::Shutdown()\n{\n\tGargantuanSetRunningStructuralGrant(GargantuanRunningStructuralGrant, false);\n\tGargantuanFeedback.Purged = true; // Purge is never ACK retirement.")
GargantuanReplaceFeedback("\t\tpMsg->Unlink();\n\t\tpMsg->Release();" "\t\tGargantuanFeedback.AckMessage(pMsg->m_nMessageNumber, pMsg->m_cbSize, info.m_cbHdr);\n\t\tpMsg->Unlink();\n\t\tpMsg->Release();")
GargantuanReplaceFeedback("\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight" "\t\t\t\t\t\tm_senderState.GargantuanFeedback.AckSegment(cbSeg, relSeg.m_hStatusOrRetry == SNPSendReliableSegment_t::k_nStatus_Acked);\n\n\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight")
GargantuanReplaceFeedback("pInFlightSeg->m_hStatusOrRetry = SNPSendReliableSegment_t::k_nStatus_InFlight;" "pInFlightSeg->m_hStatusOrRetry = SNPSendReliableSegment_t::k_nStatus_InFlight;\n\t\t\t\tpInFlightSeg->m_bGargantuanEverSent = false;")
GargantuanReplaceFeedback("\t// We sent a packet.  Track it" [=[
	// Count service only after the transport accepted this native packet.  A
	// failed send leaves the segment on the retry list with EverSent=false;
	// its later successful retry is unique first-send, not retransmission.
	m_senderState.GargantuanFeedback.NativePacket(nBytesSent);
	const uint64 GargantuanPacketSentAt = GargantuanReliableServiceClock();
	for (uint16 hGargantuanSeg : helper.InFlightPkt().m_vecReliableSegments)
	{
		SNPSendReliableSegment_t &GargantuanSeg = m_senderState.m_listSentReliableSegments[hGargantuanSeg];
		if (!GargantuanSeg.m_bGargantuanEverSent)
		{
			const auto &GargantuanCounters = m_senderState.GargantuanFeedback;
			int nGargantuanStructuralBytes = 0;
			if (GargantuanCounters.ActiveAttributedRetirementToken &&
				GargantuanCounters.ActiveAttributedMessageNumber == uint64_t(GargantuanSeg.m_pMsg->m_nMessageNumber))
			{
				const int nBodyBegin = std::max(GargantuanSeg.m_nOffset, GargantuanSeg.m_pMsg->ReliableSendInfo().m_cbHdr);
				const int nBodyEnd = std::min(GargantuanSeg.m_nOffset + GargantuanSeg.m_cbSize,
					GargantuanSeg.m_pMsg->ReliableSendInfo().m_cbHdr + GargantuanSeg.m_pMsg->m_cbSize);
				nGargantuanStructuralBytes = std::max(0, nBodyEnd - nBodyBegin);
			}
			GargantuanSeg.m_bGargantuanEverSent = true;
			m_senderState.GargantuanFeedback.FirstSend(GargantuanSeg.m_cbSize, nGargantuanStructuralBytes,
				GargantuanPacketSentAt);
		}
		else
			m_senderState.GargantuanFeedback.Retransmit(GargantuanSeg.m_cbSize);
	}
	const auto &GargantuanGrant = m_senderState.GargantuanFeedback;
	GargantuanSetRunningStructuralGrant(m_senderState.GargantuanRunningStructuralGrant,
		GargantuanGrant.StructuralActiveGrantBytes &&
		GargantuanGrant.StructuralActiveGrantFirstSentBytes > 0 &&
		GargantuanGrant.StructuralActiveGrantFirstSentBytes < GargantuanGrant.StructuralActiveGrantBytes);

	// We sent a packet.  Track it]=])
GargantuanReplaceFeedback("\tpSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;" [=[
	pSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;
	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_Reliable )
	{
		const auto nGargantuanAttribution = GargantuanTakeReliableRetirementAttribution();
		if ( nGargantuanAttribution.Token )
			m_senderState.GargantuanFeedback.AttributeMessage( nGargantuanAttribution.Token,
				pSendMessage->m_nMessageNumber, pSendMessage->m_cbSize,
				nGargantuanAttribution.ActivatedAtMicroseconds );
	}]=])
GargantuanReplaceFeedback("\tint nMaxPacketsPerThinkRemaining = g_cbUDPSocketBufferSize >> 11;" [=[
	int nMaxPacketsPerThinkRemaining = g_cbUDPSocketBufferSize >> 11;
	int nGargantuanStructuralPacketsThisThink = 0;]=])
GargantuanReplaceFeedback("\t\t// Sent too many packets in one burst?" [=[
		// Give other finite structural grants a sender visit before one
		// connection drains an accumulated pacing burst. Keep this sender's
		// token bucket and the ordinary traffic packet limit unchanged.
		const auto &GargantuanGrant = m_senderState.GargantuanFeedback;
		if (GargantuanGrant.StructuralActiveGrantBytes > GargantuanGrant.StructuralActiveGrantFirstSentBytes &&
			++nGargantuanStructuralPacketsThisThink >= 4)
		{
			const SteamNetworkingMicroseconds usecReschedule = SteamNetworkingSockets_GetLocalTimestamp();
			SNP_TokenBucket_Accumulate(usecReschedule);
			return std::max(usecReschedule + 1, SNP_GetNextThinkTime(usecReschedule));
		}

		// Sent too many packets in one burst?]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_connections.h 9ece0f7051f1b67e44c75c27c10867a863b56e2a0d0ac95849aa116b5274a9ef)
GargantuanReplaceFeedback("\t/// Called when we close the connection locally" [=[
	// Caller holds the existing connection lock. Direct fields avoid a second
	// mutable status sample or the rate-estimator side effects of realtime status.
	void GargantuanPopulateReliableServiceFeedback(GargantuanReliableServiceSnapshot &Result) const {
		GargantuanCopyReliableServiceFeedback(m_senderState.GargantuanFeedback,
			m_senderState.m_cbPendingReliable, m_senderState.m_cbSentUnackedReliable,
			static_cast<int>(GetState()), Result);
	}

	/// Called when we close the connection locally]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(csteamnetworkingsockets.cpp 2b260c05cc8c262e785387ea3d08eee74cc03b6eed00493e961a82c05dec5451)
GargantuanReplaceFeedback("static CSteamNetworkListenSocketBase *GetListenSocketByHandle" [=[
bool GargantuanReadNativeFeedback(ISteamNetworkingSockets *Interface, uint32 Handle, GargantuanReliableServiceSnapshot &Result) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanReliableServiceFeedback");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	Connection->GargantuanPopulateReliableServiceFeedback(Result);
	return true;
}

static CSteamNetworkListenSocketBase *GetListenSocketByHandle]=])
GargantuanReplaceFeedback("\tpConn->APICloseConnection( nReason, pszDebug, bEnableLinger );\n\treturn true;" "\tpConn->APICloseConnection( nReason, pszDebug, bEnableLinger );\n\tif (auto *Result = GargantuanGetClosingFeedback()) pConn->GargantuanPopulateReliableServiceFeedback(*Result);\n\treturn true;")
GargantuanWriteFeedbackSource()

# Our checked arithmetic is compiled independently, with full project sanitizer
# instrumentation; GNS-only compatibility exclusions cannot leak onto it.
add_library(gargantuan_gns_feedback_counters OBJECT "${GargantuanFeedbackDirectory}/ReliableServiceFeedback.cpp")
target_compile_features(gargantuan_gns_feedback_counters PRIVATE cxx_std_11)
target_include_directories(GameNetworkingSockets_s PRIVATE "${GargantuanFeedbackDirectory}")
target_sources(GameNetworkingSockets_s PRIVATE $<TARGET_OBJECTS:gargantuan_gns_feedback_counters>)
add_library(gargantuan_gns_feedback_snapshot OBJECT "${GargantuanFeedbackDirectory}/ReliableServiceFeedbackBridge.cpp")
target_compile_features(gargantuan_gns_feedback_snapshot PRIVATE cxx_std_17)
target_include_directories(gargantuan_gns_feedback_snapshot PRIVATE
	"${gamenetworkingsockets_SOURCE_DIR}/include")
target_compile_definitions(gargantuan_gns_feedback_snapshot PRIVATE
	$<TARGET_PROPERTY:GameNetworkingSockets_s,COMPILE_DEFINITIONS>)
if(MSVC)
	target_compile_options(gargantuan_gns_feedback_snapshot PRIVATE $<IF:$<CONFIG:Debug>,/GR,/GR->)
endif()
target_sources(GameNetworkingSockets_s PRIVATE $<TARGET_OBJECTS:gargantuan_gns_feedback_snapshot>)
