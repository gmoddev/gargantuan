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
GargantuanReplaceFeedback("struct SSNPSenderState\n{" "struct SSNPSenderState\n{\n\tGargantuanReliableServiceCounters GargantuanFeedback;")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_snp.cpp 99e2b190b17993139bd3251f8862b81b58903a119ea456edacfec68f3dd9d65c)
GargantuanReplaceFeedback("void SSNPSenderState::Shutdown()\n{" "void SSNPSenderState::Shutdown()\n{\n\tGargantuanFeedback.Purged = true; // Purge is never ACK retirement.")
GargantuanReplaceFeedback("\t\tpMsg->Unlink();\n\t\tpMsg->Release();" "\t\tGargantuanFeedback.AckMessage(pMsg->m_nMessageNumber, pMsg->m_cbSize, info.m_cbHdr);\n\t\tpMsg->Unlink();\n\t\tpMsg->Release();")
GargantuanReplaceFeedback("\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight" "\t\t\t\t\t\tm_senderState.GargantuanFeedback.AckSegment(cbSeg, relSeg.m_hStatusOrRetry == SNPSendReliableSegment_t::k_nStatus_Acked);\n\n\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight")
GargantuanReplaceFeedback("// First time sending this segment.  Fill out an inflight segment record" "m_senderState.GargantuanFeedback.FirstSend(pSeg->m_cbSegSize);\n\t\t\t\t// First time sending this segment.  Fill out an inflight segment record")
GargantuanReplaceFeedback("// It's a retry\n\t\t\t\tpInFlightSeg" "m_senderState.GargantuanFeedback.Retransmit(pSeg->m_cbSegSize);\n\t\t\t\t// It's a retry\n\t\t\t\tpInFlightSeg")
GargantuanReplaceFeedback("\tpSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;" [=[
	pSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;
	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_Reliable )
	{
		const uint64_t nGargantuanRetirementToken = GargantuanTakeReliableRetirementAttribution();
		if ( nGargantuanRetirementToken )
			m_senderState.GargantuanFeedback.AttributeMessage( nGargantuanRetirementToken, pSendMessage->m_nMessageNumber );
	}]=])
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
