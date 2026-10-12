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
GargantuanReplaceFeedback("#pragma once" "#pragma once\n#include \"ReliableServiceFeedback.hpp\"\n#include \"AckDiagnostics.hpp\"\n#include \"PromptAckWireBudget.hpp\"\n#include <memory>")
GargantuanReplaceFeedback("struct LockDebugInfo;" "struct LockDebugInfo;\nstruct SNPPacketSerializeHelper;\nstruct GargantuanNativeEgressTicket;")
GargantuanReplaceFeedback("class CSteamNetworkingMessage : public SteamNetworkingMessage_t\n{" "class CSteamNetworkingMessage : public SteamNetworkingMessage_t\n{\npublic:\n\tuint32_t m_nGargantuanPayloadBytes; // Sender-local BODY size assigned before native enqueue.\n\tbool m_bGargantuanStructuralPayload; // Classification only; no reference or ordering ownership.")
GargantuanReplaceFeedback("struct SSNPSenderState\n{" "struct SSNPSenderState\n{\n\tstd::unique_ptr<GargantuanAckDiagnostics> GargantuanAckTrace;\n\tGargantuanPromptAckWireBudget GargantuanPromptWire;\n\tbool GargantuanPromptFinalGrantAck = false; // Enabled only by funded attributed structural submission.\n\tbool GargantuanNativeEgressEnabled = false; // Explicit direct-UDP connection-generation opt-in.\n\tISteamNetworkingSockets *GargantuanTerminalInterface = nullptr;\n\tuint64_t GargantuanTerminalGeneration = 0;\n\tSNPPacketSerializeHelper *GargantuanPendingHelper = nullptr; // Actor-owned until native result reconciliation.\n\tGargantuanNativeEgressTicket *GargantuanPendingTicket = nullptr;\n\tSteamNetworkingMicroseconds GargantuanPendingDeadline = 0;\n\tbool GargantuanPendingCancelRequested = false;\n\tSteamNetworkingMicroseconds GargantuanLastCompletedGnsAt = 0;\n\tbool GargantuanTerminalPurgePending = false;\n\tGargantuanReliableServiceCounters GargantuanFeedback;\n\tbool GargantuanRunningStructuralGrant = false;")
GargantuanReplaceFeedback("\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;" "\t// A failed SendEncryptedDataChunk queues retry without first-send service.\n\tbool m_bGargantuanEverSent;\n\n\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_snp.cpp 99e2b190b17993139bd3251f8862b81b58903a119ea456edacfec68f3dd9d65c)
GargantuanReplaceFeedback("#include \"steamnetworkingsockets_snp.h\"" "#include \"steamnetworkingsockets_snp.h\"\n#include \"ServiceTimingDiagnostics.hpp\"\n#include \"OrdinaryWirePacer.hpp\"\n#include \"NativeEgressWorker.hpp\"")
GargantuanReplaceFeedback("struct SNPPacketSerializeHelper\n{" [=[struct SNPPacketSerializeHelper
{
	GargantuanOrdinaryWirePacer::Permit GargantuanOrdinaryPermit;
	bool GargantuanHasData = false;
	bool GargantuanHasAck = false;
	bool GargantuanPromptAck = false;
	uint64_t GargantuanPreparationEntered = 0;
	int GargantuanExpectedNativeBytes = 0;]=])
GargantuanReplaceFeedback("int cbAckBytesWritten = pAfterAcks - pPayloadPtr;" "int cbAckBytesWritten = pAfterAcks - pPayloadPtr;\n\t\t\thelper.GargantuanHasAck = cbAckBytesWritten > 0;")
GargantuanReplaceFeedback("\t\t|| helper.InFlightPkt().m_pTransport != m_pTransport // transport is not the selected transport" [=[		|| helper.InFlightPkt().m_pTransport != m_pTransport // transport is not the selected transport
		// A control/ASAP wake cannot bypass the actual DATA reservation.  This
		// precedes native queue mutation and retains the existing ACK-only path.
		|| (GargantuanOrdinaryDataIsPaced(GargantuanHasRunningStructuralGrant(),
			m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken,
			m_senderState.GargantuanFeedback.LastAttributedRetirementToken,
			m_senderState.GargantuanFeedback.StructuralActiveGrantBytes,
			m_senderState.GargantuanFeedback.StructuralActiveGrantFirstSentBytes) &&
			!helper.GargantuanOrdinaryPermit.Reserve(
				GargantuanSharedOrdinaryWirePacer(k_cbSteamNetworkingSocketsMaxUDPMsgLen), helper.UsecNow()))]=])
GargantuanReplaceFeedback("\tbool bEmpty = segmentCollector.IsEmpty();" "\tbool bEmpty = segmentCollector.IsEmpty();\n\thelper.GargantuanHasData = !bEmpty;")
GargantuanReplaceFeedback("SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow )\n{" [=[SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow )
{
	auto &GargantuanInitialTrace = m_senderState.GargantuanFeedback.InitialTrace;
	if (GargantuanInitialTrace.Enabled && !GargantuanInitialTrace.FirstSnpVisit) {
		GargantuanInitialTrace.FirstSnpVisit = GargantuanInitialServiceClock();
		GargantuanInitialTrace.PendingAtFirstSnpVisit = m_senderState.GargantuanPendingTicket != nullptr;
	}
	if (m_senderState.GargantuanPendingTicket) {
		if (GargantuanReconcileNativeEgressByDeadline(usecNow)) {
			GargantuanDrainPendingReceived();
			// Completion and replay can be newer than the thinker's entry time.
			usecNow = SteamNetworkingSockets_GetLocalTimestamp();
		}
		else return usecNow + 1000;
	}
	GargantuanServiceTimingSpan GargantuanSender(GargantuanServiceTimingPhase::SnpSender,
		GargantuanInitialTrace.Enabled && !GargantuanInitialTrace.NativeCompleted);
	if (GargantuanSender.Enabled()) {
		GargantuanSender.SetResult(m_hConnectionSelf); // Observed native handle, never authority.
		GargantuanSender.SetDetail(m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken);
		if (GargantuanHasRunningStructuralGrant()) GargantuanSender.AddFlags(1);
	}]=])
GargantuanReplaceFeedback("void SSNPSenderState::Shutdown()\n{" "void SSNPSenderState::Shutdown()\n{\n\tGargantuanSetRunningStructuralGrant(GargantuanRunningStructuralGrant, false);\n\tGargantuanFeedback.Purged = true; // Purge is never ACK retirement.")
GargantuanReplaceFeedback("void CSteamNetworkConnectionBase::SNP_ShutdownConnection()\n{\n\tm_senderState.Shutdown();\n\tm_receiverState.Shutdown();\n}" [=[void CSteamNetworkConnectionBase::SNP_ShutdownConnection()
{
	if (m_senderState.GargantuanPendingTicket) {
		m_senderState.GargantuanTerminalPurgePending = true;
		SetNextThinkTimeASAP();
		return;
	}
	m_senderState.GargantuanTerminalPurgePending = false;
	m_senderState.Shutdown();
	m_receiverState.Shutdown();
	if (m_senderState.GargantuanTerminalGeneration) {
		GargantuanReliableServiceSnapshot Snapshot;
		GargantuanPopulateReliableServiceFeedback(Snapshot);
		if (!GargantuanStoreNativeTerminalEvidence(m_senderState.GargantuanTerminalInterface,
			m_hConnectionSelf, m_senderState.GargantuanTerminalGeneration, Snapshot))
			GargantuanFailStopNativeEgress("terminal evidence store failed");
		m_senderState.GargantuanTerminalGeneration = 0;
		m_senderState.GargantuanTerminalInterface = nullptr;
	}
}]=])
GargantuanReplaceFeedback("\t\tpMsg->Unlink();\n\t\tpMsg->Release();" "\t\tif (GargantuanAckTrace) GargantuanAckTrace->Record(GargantuanAckDiagnostics::MessageAcked, SteamNetworkingSockets_GetLocalTimestamp(), pMsg->m_nMessageNumber, pMsg->m_cbSize);\n\t\tGargantuanFeedback.AckMessage(pMsg->m_nMessageNumber, pMsg->m_cbSize, info.m_cbHdr);\n\t\tpMsg->Unlink();\n\t\tpMsg->Release();")
GargantuanReplaceFeedback("\tAssertLocksHeldByCurrentThread( \"SNP_SendPacket\" );" [=[
	AssertLocksHeldByCurrentThread( "SNP_SendPacket" );
	const uint64_t GargantuanPreparationEntered = m_senderState.GargantuanFeedback.InitialTrace.Enabled &&
		!m_senderState.GargantuanFeedback.InitialTrace.NativeCompleted ? GargantuanInitialServiceClock() : 0;
	if (m_senderState.GargantuanPendingTicket) return false; // One unresolved numbered packet per connection.
	if (m_senderState.GargantuanNativeEgressEnabled && GargantuanReplayingNativeDatagrams)
		return false; // Replay cannot start another send until receive order is restored.
	if (m_senderState.GargantuanNativeEgressEnabled && GargantuanIsDirectUDP() &&
		!GargantuanCanQueueNativeEgress()) {
		m_senderState.GargantuanFeedback.Invalid = true; // Capacity failure before packet/ACK mutation.
		return false;
	}
	ctx.m_bGargantuanUseNativeEgress = m_senderState.GargantuanNativeEgressEnabled &&
		GargantuanIsDirectUDP();
	if (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->SerializedAckPacket = 0;]=])
GargantuanReplaceFeedback("\tSNPPacketSerializeHelper helper;" [=[
	std::unique_ptr<SNPPacketSerializeHelper> GargantuanPendingCandidate;
	if (ctx.m_bGargantuanUseNativeEgress)
		GargantuanPendingCandidate.reset(new (std::nothrow) SNPPacketSerializeHelper());
	if (ctx.m_bGargantuanUseNativeEgress && !GargantuanPendingCandidate) {
		m_senderState.GargantuanFeedback.Invalid = true;
		return false;
	}
	SNPPacketSerializeHelper GargantuanStackHelper;
	SNPPacketSerializeHelper &helper = GargantuanPendingCandidate ? *GargantuanPendingCandidate : GargantuanStackHelper;
	helper.GargantuanPreparationEntered = GargantuanPreparationEntered;
	const bool GargantuanHadRetry = !m_senderState.m_listReadyRetryReliableRange.IsEmpty();]=])
GargantuanReplaceFeedback("\t\tSNP_RecordReceivedPktNum( nPktNum, usecNow, bScheduleAck );" "\t\tSNP_RecordReceivedPktNum( nPktNum, usecNow, bScheduleAck );\n\t\tif (bScheduleAck && m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Received(usecNow, nPktNum, m_receiverState.TimeWhenFlushAcks());")
GargantuanReplaceFeedback("\t\tpReliableDecode += cbMsgSize;" "\t\tif (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::MessageReceived, usecNow, nMsgNum, cbMsgSize);\n\t\tpReliableDecode += cbMsgSize;")
GargantuanReplaceFeedback("\t\tm_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior = INT64_MAX; // Clear timer, we wrote everything we needed to" "\t\tif (m_senderState.GargantuanAckTrace) {\n\t\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nLastPktToAck;\n\t\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nLastPktToAck, 0);\n\t\t}\n\t\tm_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior = INT64_MAX; // Clear timer, we wrote everything we needed to")
GargantuanReplaceFeedback("\t// Fit as many blocks as possible." "\tif (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::FragmentedAckSerialized, helper.UsecNow(), nLastPktToAck, 0);\n\t// Fit as many blocks as possible.")
GargantuanReplaceFeedback("\t\t\t*pLatestPktNum = LittleWord( uint16( nLastRecvPktNum ) );" "\t\t\t*pLatestPktNum = LittleWord( uint16( nLastRecvPktNum ) );\n\t\t\tif (m_senderState.GargantuanAckTrace) {\n\t\t\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nLastRecvPktNum;\n\t\t\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nLastRecvPktNum, 0);\n\t\t\t}")
GargantuanReplaceFeedback("\t++nAckEnd;\n\n\t#ifdef SNP_ENABLE_PACKETSENDLOG" "\t++nAckEnd;\n\tif (m_senderState.GargantuanAckTrace) {\n\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nAckEnd - 1;\n\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nAckEnd - 1, nBlocks);\n\t}\n\n\t#ifdef SNP_ENABLE_PACKETSENDLOG")
GargantuanReplaceFeedback("\t// OK, we have a plaintext payload.  Encrypt and send it." [=[
	ctx.m_bGargantuanProtectedEgress = !helper.GargantuanHasData || helper.GargantuanHasAck || GargantuanHadRetry;
	// The request belongs to the existing packet that completes unique first
	// transmission, never to enqueue, a segment boundary, or retransmission.
	if (ctx.m_bGargantuanPromptAckReserved && GargantuanCanReservePromptAck())
	{
		const auto &Grant = m_senderState.GargantuanFeedback;
		uint64 UniqueBytes = 0;
		for (uint16 SegmentHandle : helper.InFlightPkt().m_vecReliableSegments)
		{
			const auto &Segment = m_senderState.m_listSentReliableSegments[SegmentHandle];
			if (!Segment.m_bGargantuanEverSent && Grant.ActiveAttributedRetirementToken &&
				Grant.ActiveAttributedMessageNumber == uint64_t(Segment.m_pMsg->m_nMessageNumber))
			{
				const int Begin = std::max(Segment.m_nOffset, Segment.m_pMsg->ReliableSendInfo().m_cbHdr);
				const int End = std::min(Segment.m_nOffset + Segment.m_cbSize,
					Segment.m_pMsg->ReliableSendInfo().m_cbHdr + Segment.m_pMsg->m_cbSize);
				UniqueBytes += std::max(0, End - Begin);
			}
		}
		ctx.m_bGargantuanPromptAck = UniqueBytes && Grant.StructuralActiveGrantFirstSentBytes > 0 &&
			Grant.StructuralActiveGrantBytes > Grant.StructuralActiveGrantFirstSentBytes &&
			UniqueBytes == Grant.StructuralActiveGrantBytes - Grant.StructuralActiveGrantFirstSentBytes;
		if (!m_senderState.GargantuanPromptWire.CanRequest(k_cbSteamNetworkingSocketsMaxUDPMsgLen + 48))
			ctx.m_bGargantuanPromptAck = false;
		if (ctx.m_bGargantuanPromptAck && m_senderState.GargantuanAckTrace) {
			m_senderState.GargantuanAckTrace->PromptFinalWireAllowed = true;
			m_senderState.GargantuanAckTrace->PromptFinalPriorWireBytes = m_senderState.GargantuanPromptWire.WireBytes;
		}
	}

	// OK, we have a plaintext payload.  Encrypt and send it.]=])
GargantuanReplaceFeedback("\t// OK, we have a plaintext payload.  Encrypt and send it." [=[
	// Bounded opt-in diagnostic: exercise the real native-send failure/retry
	// branch without transmitting a packet and then pretending it failed.
	const bool GargantuanFailPromptPacket = ctx.m_bGargantuanPromptAck && m_senderState.GargantuanAckTrace &&
		m_senderState.GargantuanAckTrace->FailNextPromptPacket;
	if (GargantuanFailPromptPacket) {
		m_senderState.GargantuanAckTrace->FailNextPromptPacket = false;
		++m_senderState.GargantuanAckTrace->InjectedNativeSendFailures;
		m_senderState.GargantuanAckTrace->FirstSentBytesAtInjectedFailure = m_senderState.GargantuanFeedback.StructuralActiveGrantFirstSentBytes;
	}
	// OK, we have a plaintext payload.  Encrypt and send it.]=])
GargantuanReplaceFeedback("nBytesSent = helper.InFlightPkt().m_pTransport->SendEncryptedDataChunk(" "nBytesSent = GargantuanFailPromptPacket ? 0 : helper.InFlightPkt().m_pTransport->SendEncryptedDataChunk(")
GargantuanReplaceFeedback("\tif ( nBytesSent <= 0 )" [=[
	if (nBytesSent == -1 && ctx.m_pGargantuanNativeEgressTicket && GargantuanPendingCandidate) {
		Assert(!m_senderState.GargantuanPendingTicket && !m_senderState.GargantuanPendingHelper);
		helper.GargantuanPromptAck = ctx.m_bGargantuanPromptAck;
		helper.GargantuanExpectedNativeBytes = ctx.m_nGargantuanQueuedBytes;
		m_senderState.GargantuanPendingTicket = ctx.m_pGargantuanNativeEgressTicket;
		m_senderState.GargantuanPendingHelper = GargantuanPendingCandidate.release();
		m_senderState.GargantuanPendingDeadline = SteamNetworkingSockets_GetLocalTimestamp() + 250000;
		m_senderState.GargantuanPendingCancelRequested = false;
		if (!GargantuanRegisterNativePending(this))
			GargantuanFailStopNativeEgress("pending connection registry exhausted");
		Assert(GargantuanPendingReceive && !GargantuanPendingReceive->IsActive());
		GargantuanPendingReceive->Begin();
		return true;
	}
	if ( nBytesSent <= 0 )]=])
GargantuanReplaceFeedback("\t\t// We have potentially transfered ownership of some reliable messages" [=[
		if (ctx.m_bGargantuanPromptAck && m_senderState.GargantuanAckTrace)
			m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::PromptRequestFailed,
				helper.UsecNow(), m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken, 0);
		// We have potentially transfered ownership of some reliable messages]=])
GargantuanReplaceFeedback("\t\tSNP_QueueReliableSegmentsForRetry( helper.m_insertInflightPkt.second, 0, \"Send fail\" );" [=[
		SNP_QueueReliableSegmentsForRetry( helper.m_insertInflightPkt.second, 0, "Send fail" );
		// This local packet will never enter the in-flight map. Drop its segment
		// references after establishing retry ownership. A zero-ref retry segment
		// remains in the native retry list; RemoveRefCountReliableSegment only
		// destroys an ACKED zero-ref segment. The next send takes its own ref.
		for (uint16 SegmentHandle : helper.InFlightPkt().m_vecReliableSegments) {
			m_senderState.RemoveRefCountReliableSegment(SegmentHandle);
			if (m_senderState.GargantuanAckTrace)
				++m_senderState.GargantuanAckTrace->FailedPacketReferencesReleased;
		}]=])
GargantuanReplaceFeedback("\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight" "\t\t\t\t\t\tm_senderState.GargantuanFeedback.AckSegment(cbSeg, relSeg.m_hStatusOrRetry == SNPSendReliableSegment_t::k_nStatus_Acked);\n\n\t\t\t\t\t\t// The most common case (hopefully): the segment is currently in flight")
GargantuanReplaceFeedback("pInFlightSeg->m_hStatusOrRetry = SNPSendReliableSegment_t::k_nStatus_InFlight;" "pInFlightSeg->m_hStatusOrRetry = SNPSendReliableSegment_t::k_nStatus_InFlight;\n\t\t\t\tpInFlightSeg->m_bGargantuanEverSent = false;")
GargantuanReplaceFeedback("\t// We sent a packet.  Track it" [=[
	// Count service only after the transport accepted this native packet.  A
	// failed send leaves the segment on the retry list with EverSent=false;
	// its later successful retry is unique first-send, not retransmission.
	m_senderState.GargantuanFeedback.NativePacket(nBytesSent);
	if (!helper.GargantuanOrdinaryPermit.Complete(nBytesSent, helper.GargantuanHasData))
		m_senderState.GargantuanFeedback.Invalid = true; // Oversized native packet cannot qualify.
	GargantuanServiceTimingPacket(nBytesSent, m_hConnectionSelf);
	if (m_senderState.GargantuanPromptFinalGrantAck && m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken)
		m_senderState.GargantuanPromptWire.Charge(nBytesSent, 48); // IPv6 + UDP, conservative for IPv4.
	if (ctx.m_bGargantuanPromptAck && m_senderState.GargantuanAckTrace)
		m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::PromptRequestSent,
			helper.UsecNow(), m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken, nBytesSent);
	if (m_senderState.GargantuanAckTrace && m_senderState.GargantuanAckTrace->SerializedAckPacket)
		m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckPacketSent,
			helper.UsecNow(), m_senderState.GargantuanAckTrace->SerializedAckPacket, nBytesSent);
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
			const int OrdinaryBegin = std::max(GargantuanSeg.m_nOffset, GargantuanSeg.m_pMsg->ReliableSendInfo().m_cbHdr);
			const int OrdinaryEnd = std::min(GargantuanSeg.m_nOffset + GargantuanSeg.m_cbSize,
				GargantuanSeg.m_pMsg->ReliableSendInfo().m_cbHdr + int(GargantuanSeg.m_pMsg->m_nGargantuanPayloadBytes));
			const int OrdinaryBytes = GargantuanSeg.m_pMsg->m_bGargantuanStructuralPayload ? 0 : std::max(0, OrdinaryEnd - OrdinaryBegin);
			m_senderState.GargantuanFeedback.FirstSend(GargantuanSeg.m_cbSize, nGargantuanStructuralBytes,
				GargantuanPacketSentAt, OrdinaryBytes);
			m_senderState.GargantuanFeedback.FirstSendQualifiedOrdinary(GargantuanSeg.m_pMsg->m_nMessageNumber,
				OrdinaryBytes, GargantuanPacketSentAt);
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
GargantuanReplaceFeedback("// Base class for a list of segments for the same lane" [=[
bool CSteamNetworkConnectionBase::GargantuanReconcileNativeEgressByDeadline(
	SteamNetworkingMicroseconds UsecNow)
{
	if (GargantuanReconcileNativeEgress()) return true;
	if (UsecNow < m_senderState.GargantuanPendingDeadline) return false;
	if (m_senderState.GargantuanPendingCancelRequested)
		GargantuanFailStopNativeEgress("pre-OS native cancellation exceeded deadline");
	if (!GargantuanCancelUnstartedNativeEgress(m_senderState.GargantuanPendingTicket)) {
		// Ready may have been published after our first acquire read.
		if (GargantuanReconcileNativeEgress()) return true;
		GargantuanFailStopNativeEgress("begun native send exceeded protected service deadline");
	}
	m_senderState.GargantuanPendingCancelRequested = true;
	m_senderState.GargantuanPendingDeadline = UsecNow + 250000;
	return GargantuanReconcileNativeEgress();
}

bool CSteamNetworkConnectionBase::GargantuanReconcileNativeEgress()
{
	auto *Ticket = m_senderState.GargantuanPendingTicket;
	if (!Ticket) return true;
	GargantuanNativeEgressResult Result{};
	if (!GargantuanReadNativeEgressResult(Ticket, Result)) return false;
	auto *Helper = m_senderState.GargantuanPendingHelper;
	Assert(Helper);
	const int Bytes = Result.Positive && Result.Bytes == Helper->GargantuanExpectedNativeBytes ? Result.Bytes : 0;
	if (!Bytes || !Result.CompletedAtMicroseconds || !Result.CompletedAtGnsMicroseconds) {
		m_senderState.GargantuanLastCompletedGnsAt = 0;
		if (Result.Positive) m_senderState.GargantuanFeedback.Invalid = true;
		if (Helper->GargantuanPromptAck && m_senderState.GargantuanAckTrace)
			m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::PromptRequestFailed,
				Helper->UsecNow(), m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken, 0);
		SNP_QueueReliableSegmentsForRetry(Helper->m_insertInflightPkt.second, 0, "Native send fail");
		for (uint16 SegmentHandle : Helper->InFlightPkt().m_vecReliableSegments) {
			m_senderState.RemoveRefCountReliableSegment(SegmentHandle);
			if (m_senderState.GargantuanAckTrace)
				++m_senderState.GargantuanAckTrace->FailedPacketReferencesReleased;
		}
	} else {
		m_senderState.GargantuanLastCompletedGnsAt = Result.CompletedAtGnsMicroseconds;
		m_senderState.GargantuanFeedback.NativePacket(Bytes);
		if (!Helper->GargantuanOrdinaryPermit.Complete(Bytes, Helper->GargantuanHasData))
			m_senderState.GargantuanFeedback.Invalid = true;
		GargantuanServiceTimingPacket(Bytes, m_hConnectionSelf);
		if (m_senderState.GargantuanPromptFinalGrantAck && m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken)
			m_senderState.GargantuanPromptWire.Charge(Bytes, 48);
		if (Helper->GargantuanPromptAck && m_senderState.GargantuanAckTrace)
			m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::PromptRequestSent,
				Result.CompletedAtMicroseconds, m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken, Bytes);
		if (m_senderState.GargantuanAckTrace && m_senderState.GargantuanAckTrace->SerializedAckPacket)
			m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckPacketSent,
				Result.CompletedAtMicroseconds, m_senderState.GargantuanAckTrace->SerializedAckPacket, Bytes);
		for (uint16 SegmentHandle : Helper->InFlightPkt().m_vecReliableSegments) {
			auto &Segment = m_senderState.m_listSentReliableSegments[SegmentHandle];
			if (!Segment.m_bGargantuanEverSent) {
				const auto &Counters = m_senderState.GargantuanFeedback;
				int StructuralBytes = 0;
				if (Counters.ActiveAttributedRetirementToken &&
					Counters.ActiveAttributedMessageNumber == uint64_t(Segment.m_pMsg->m_nMessageNumber)) {
					const int Begin = std::max(Segment.m_nOffset, Segment.m_pMsg->ReliableSendInfo().m_cbHdr);
					const int End = std::min(Segment.m_nOffset + Segment.m_cbSize,
						Segment.m_pMsg->ReliableSendInfo().m_cbHdr + Segment.m_pMsg->m_cbSize);
					StructuralBytes = std::max(0, End - Begin);
				}
				auto &Trace = m_senderState.GargantuanFeedback.InitialTrace;
				if (StructuralBytes > 0 && Trace.Enabled && !Trace.NativeCompleted &&
					Trace.Token == Counters.ActiveAttributedRetirementToken &&
					Trace.MessageNumber == uint64_t(Segment.m_pMsg->m_nMessageNumber)) {
					Trace.PacketPreparationEntered = Helper->GargantuanPreparationEntered;
					Trace.WorkerQueued = Result.QueuedAtMicroseconds;
					Trace.WorkerStarted = Result.StartedAtMicroseconds;
					Trace.NativeCompleted = Result.CompletedAtMicroseconds;
					Trace.ActorReconciled = GargantuanInitialServiceClock();
					Trace.DispatchSequence = Result.DispatchSequence;
				}
				Segment.m_bGargantuanEverSent = true;
				const int OrdinaryBegin = std::max(Segment.m_nOffset, Segment.m_pMsg->ReliableSendInfo().m_cbHdr);
				const int OrdinaryEnd = std::min(Segment.m_nOffset + Segment.m_cbSize,
					Segment.m_pMsg->ReliableSendInfo().m_cbHdr + int(Segment.m_pMsg->m_nGargantuanPayloadBytes));
				const int OrdinaryBytes = Segment.m_pMsg->m_bGargantuanStructuralPayload ? 0 : std::max(0, OrdinaryEnd - OrdinaryBegin);
				m_senderState.GargantuanFeedback.FirstSend(Segment.m_cbSize, StructuralBytes,
					Result.CompletedAtMicroseconds, OrdinaryBytes);
				m_senderState.GargantuanFeedback.FirstSendQualifiedOrdinary(Segment.m_pMsg->m_nMessageNumber,
					OrdinaryBytes, Result.CompletedAtMicroseconds);
			} else m_senderState.GargantuanFeedback.Retransmit(Segment.m_cbSize);
		}
		const auto &Grant = m_senderState.GargantuanFeedback;
		GargantuanSetRunningStructuralGrant(m_senderState.GargantuanRunningStructuralGrant,
			Grant.StructuralActiveGrantBytes && Grant.StructuralActiveGrantFirstSentBytes > 0 &&
			Grant.StructuralActiveGrantFirstSentBytes < Grant.StructuralActiveGrantBytes);
		Helper->InFlightPkt().m_usecWhenSent = Result.CompletedAtGnsMicroseconds;
		auto Inserted = m_senderState.m_mapInFlightPacketsByPktNum.insert(Helper->m_insertInflightPkt);
		if (!Inserted.second) GargantuanFailStopNativeEgress("duplicate numbered pending packet");
		if (!Helper->InFlightPkt().m_vecReliableSegments.empty())
			m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck(Result.CompletedAtGnsMicroseconds, true);
		if (m_senderState.m_itNextInFlightPacketToTimeout == m_senderState.m_mapInFlightPacketsByPktNum.end())
			m_senderState.m_itNextInFlightPacketToTimeout = Inserted.first;
		m_senderState.MaybeCheckInFlightPacketMap();
		m_sendRateData.m_flTokenBucket -= (float)Bytes;
	}
	// Retain the first completed packet before unique structural service,
	// including ordinary/control packets created after the grant was enqueued.
	auto &PriorTrace = m_senderState.GargantuanFeedback.InitialTrace;
	if (PriorTrace.Enabled && !PriorTrace.NativeCompleted && !PriorTrace.PriorActorReconciled) {
		PriorTrace.PriorWorkerQueued = Result.QueuedAtMicroseconds;
		PriorTrace.PriorWorkerStarted = Result.StartedAtMicroseconds;
		PriorTrace.PriorNativeCompleted = Result.CompletedAtMicroseconds;
		PriorTrace.PriorNativePositive = Bytes > 0;
		PriorTrace.PriorActorReconciled = GargantuanInitialServiceClock();
	}
	GargantuanReleaseNativeEgress(Ticket);
	GargantuanUnregisterNativePending(this);
	delete Helper;
	m_senderState.GargantuanPendingHelper = nullptr;
	m_senderState.GargantuanPendingTicket = nullptr;
	m_senderState.GargantuanPendingDeadline = 0;
	m_senderState.GargantuanPendingCancelRequested = false;
	return true;
}

void CSteamNetworkConnectionBase::GargantuanServiceReadyNativeEgress()
{
	AssertLocksHeldByCurrentThread("GargantuanServiceReadyNativeEgress");
	if (!m_senderState.GargantuanPendingTicket || !GargantuanReconcileNativeEgress()) return;
	// Restore receive order before another numbered packet can be prepared.
	GargantuanDrainPendingReceived();
	if (m_senderState.GargantuanTerminalPurgePending) {
		SNP_ShutdownConnection();
		SetNextThinkTimeASAP();
		return;
	}
	// This is a bounded sender opportunity, not a replacement for the
	// connection state machine. Keep its earlier timer/deferred work intact.
	if ((m_eConnectionState == k_ESteamNetworkingConnectionState_Connected ||
		m_eConnectionState == k_ESteamNetworkingConnectionState_Linger) &&
		m_pTransport && m_pTransport->BCanSendEndToEndData()) {
		const SteamNetworkingMicroseconds UsecNow = SteamNetworkingSockets_GetLocalTimestamp();
		EnsureMinThinkTime(SNP_ThinkSendState(UsecNow));
	} else SetNextThinkTimeASAP();
}

// Base class for a list of segments for the same lane]=])
GargantuanReplaceFeedback("\tpSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;" [=[
	pSendMessage->m_nMessageNumber = ++lane.m_nLastSentMsgNum;
	pSendMessage->m_nGargantuanPayloadBytes = uint32_t(std::max(0, pSendMessage->m_cbSize));
	pSendMessage->m_bGargantuanStructuralPayload = false;
	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_Reliable )
	{
		const auto nGargantuanAttribution = GargantuanTakeReliableRetirementAttribution();
		pSendMessage->m_bGargantuanStructuralPayload = nGargantuanAttribution.StructuralClass || nGargantuanAttribution.Token;
		if (!pSendMessage->m_bGargantuanStructuralPayload)
			m_senderState.GargantuanFeedback.RegisterQualifiedOrdinary(pSendMessage->m_nMessageNumber,
				pSendMessage->m_cbSize, nGargantuanAttribution.QualifiedSequence,
				nGargantuanAttribution.QualifiedPhase, nGargantuanAttribution.SchedulerAcceptedAtMicroseconds);
		if ( nGargantuanAttribution.Token ) {
			m_senderState.GargantuanFeedback.AttributeMessage( nGargantuanAttribution.Token,
				pSendMessage->m_nMessageNumber, pSendMessage->m_cbSize,
				nGargantuanAttribution.ActivatedAtMicroseconds );
			if (nGargantuanAttribution.InitialTrace.Enabled) {
				auto &Trace = m_senderState.GargantuanFeedback.InitialTrace;
				Trace = nGargantuanAttribution.InitialTrace;
				Trace.Token = nGargantuanAttribution.Token;
				Trace.MessageNumber = uint64_t(pSendMessage->m_nMessageNumber);
				Trace.NativeHandle = m_hConnectionSelf;
				Trace.PendingAtEnqueue = m_senderState.GargantuanPendingTicket != nullptr;
			}
			if (!m_senderState.GargantuanFeedback.Invalid && !m_senderState.GargantuanFeedback.Purged &&
				m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken == nGargantuanAttribution.Token)
				m_senderState.GargantuanPromptWire.Begin(nGargantuanAttribution.Token, pSendMessage->m_cbSize);
			else m_senderState.GargantuanPromptWire.Invalid = true;
		}
	}]=])
GargantuanReplaceFeedback("\tint64 result = pSendMessage->m_nMessageNumber;" [=[
	int64 result = pSendMessage->m_nMessageNumber;
	auto &GargantuanEnqueueTrace = m_senderState.GargantuanFeedback.InitialTrace;
	if (GargantuanEnqueueTrace.Enabled && GargantuanEnqueueTrace.MessageNumber == uint64_t(result))
		GargantuanEnqueueTrace.NativeEnqueued = GargantuanInitialServiceClock();]=])
GargantuanReplaceFeedback("\t\tSteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );" [=[
		SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
		if (GargantuanEnqueueTrace.Enabled && GargantuanEnqueueTrace.MessageNumber == uint64_t(result)) {
			// Observe existing local eligibility, not a raced thinker-heap getter.
			GargantuanEnqueueTrace.EligibilityRecorded = GargantuanInitialServiceClock();
			GargantuanEnqueueTrace.EligibilityNativeSample = usecNow;
			GargantuanEnqueueTrace.RequestedDataThinkNative = usecNextThink;
			GargantuanEnqueueTrace.EligibilityMode = usecNextThink > usecNow ? 1 : 2;
		}]=])
GargantuanReplaceFeedback("\tint nMaxPacketsPerThinkRemaining = g_cbUDPSocketBufferSize >> 11;" [=[
	int nMaxPacketsPerThinkRemaining = g_cbUDPSocketBufferSize >> 11;
	int nGargantuanStructuralPacketsThisThink = 0;]=])
GargantuanReplaceFeedback("\t\t// We spent some tokens, do we have any left?" [=[
		if (m_senderState.GargantuanPendingTicket) return usecNow + 1000;
		// We spent some tokens, do we have any left?]=])
GargantuanReplaceFeedback("\t// Start with the time when the receiver needs to flush out ack." [=[
	if (m_senderState.GargantuanPendingTicket) return usecNow + 1000;
	// Start with the time when the receiver needs to flush out ack.]=])
GargantuanReplaceFeedback("\t\t// Sent too many packets in one burst?" [=[
		// Keep four packets for this structural obligation. Other ordinary
		// work on an attributed POOLED connection yields after one packet while
		// another structural obligation runs. Unmarked FULL/client paths stay
		// unchanged; credit, FIFO and native pacing are never replaced.
		const auto &GargantuanGrant = m_senderState.GargantuanFeedback;
		const int nGargantuanPacketQuantum = GargantuanGrant.SenderPacketQuantum(GargantuanHasRunningStructuralGrant());
		if (nGargantuanPacketQuantum && ++nGargantuanStructuralPacketsThisThink >= nGargantuanPacketQuantum)
		{
			GargantuanSender.AddFlags(2); // Selected sender quantum reached.
			const SteamNetworkingMicroseconds usecReschedule = SteamNetworkingSockets_GetLocalTimestamp();
			SNP_TokenBucket_Accumulate(usecReschedule);
			return std::max(usecReschedule + 1, SNP_GetNextThinkTime(usecReschedule));
		}

		// Sent too many packets in one burst?]=])
GargantuanReplaceFeedback([=[	// Reliable retry triggered?  Then send it ASAP
	if ( !m_senderState.m_listReadyRetryReliableRange.IsEmpty() )
		return 0;

	// Anything queued?
	SteamNetworkingMicroseconds usecNextSend;
	if ( m_senderState.m_messagesQueued.empty() )]=] [=[	const bool GargantuanPacedData = GargantuanOrdinaryDataIsPaced(GargantuanHasRunningStructuralGrant(),
		m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken,
		m_senderState.GargantuanFeedback.LastAttributedRetirementToken,
		m_senderState.GargantuanFeedback.StructuralActiveGrantBytes,
		m_senderState.GargantuanFeedback.StructuralActiveGrantFirstSentBytes);
	const bool GargantuanRetryData = !m_senderState.m_listReadyRetryReliableRange.IsEmpty();
	if (GargantuanRetryData && !GargantuanPacedData) return 0; // Original exempt retry path.

	// DATA eligibility is separate from the independent ACK/NACK deadlines.
	SteamNetworkingMicroseconds usecNextSend;
	if (GargantuanRetryData) usecNextSend = 0;
	else if ( m_senderState.m_messagesQueued.empty() )]=])
GargantuanReplaceFeedback([=[		if ( m_senderState.PendingBytesTotal() >= m_cbMaxPlaintextPayloadSend )
			// Send it ASAP
			return 0;

		// We have less than a full packet's worth of data.  Wait until
		// the Nagle time, if we have one
		usecNextSend = m_senderState.m_messagesQueued.m_pFirst->SNPSend_UsecNagle();]=] [=[		if ( m_senderState.PendingBytesTotal() >= m_cbMaxPlaintextPayloadSend ) {
			if (!GargantuanPacedData) return 0; // Original exempt full-packet path.
			usecNextSend = 0;
		} else {
			// Retain the original Nagle deadline for a partial DATA packet.
			usecNextSend = m_senderState.m_messagesQueued.m_pFirst->SNPSend_UsecNagle();
		}]=])
GargantuanReplaceFeedback("\t// Check if the receiver wants to send a NACK." [=[	if (GargantuanPacedData && usecNextSend != k_nThinkTime_Never) {
		const auto GargantuanNow = SteamNetworkingSockets_GetLocalTimestamp();
		const auto GargantuanEligible = GargantuanSharedOrdinaryWirePacer(
			k_cbSteamNetworkingSocketsMaxUDPMsgLen).EligibleAt(GargantuanNow);
		// Ready DATA keeps the original zero/past Nagle deadline. Retimestamping
		// it to this query's later clock could defer every otherwise-ready Think.
		usecNextSend = static_cast<SteamNetworkingMicroseconds>(
			GargantuanOrdinaryDataDeadline(usecNextSend, GargantuanNow, GargantuanEligible));
	}

	// Check if the receiver wants to send a NACK.]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_connections.h 9ece0f7051f1b67e44c75c27c10867a863b56e2a0d0ac95849aa116b5274a9ef)
GargantuanReplaceFeedback("#include \"steamnetworkingsockets_snp.h\"" "#include \"steamnetworkingsockets_snp.h\"\n#include \"PendingNativeDatagramQueue.hpp\"\n#include <new>")
GargantuanReplaceFeedback("\tint m_cbMaxEncryptedPayload;" "\tint m_cbMaxEncryptedPayload;\n\tbool m_bGargantuanPromptAckReserved = false;\n\tbool m_bGargantuanPromptAck = false;\n\tbool m_bGargantuanUseNativeEgress = false;\n\tbool m_bGargantuanProtectedEgress = false;\n\tint m_nGargantuanQueuedBytes = 0;\n\tGargantuanNativeEgressTicket *m_pGargantuanNativeEgressTicket = nullptr;")
GargantuanReplaceFeedback("\t/// Called when we close the connection locally" [=[
	bool GargantuanConfigureNativeEgress(bool Enabled, uint64_t Generation, ISteamNetworkingSockets *Interface) {
		if (Enabled && !GargantuanIsDirectUDP()) return false;
		if (m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken ||
			m_senderState.GargantuanPendingTicket ||
			m_senderState.GargantuanTerminalPurgePending) return false;
		if (!Enabled && (m_senderState.m_cbPendingReliable ||
			m_senderState.m_cbSentUnackedReliable ||
			m_senderState.m_mapInFlightPacketsByPktNum.size() > 1)) return false;
		if (Enabled && !GargantuanPendingReceive)
			GargantuanPendingReceive.reset(new (std::nothrow) GargantuanPendingNativeDatagramQueue());
		if (Enabled && !GargantuanPendingReceive) return false;
		m_senderState.GargantuanNativeEgressEnabled = Enabled;
		m_senderState.GargantuanTerminalGeneration = Enabled ? Generation : 0;
		m_senderState.GargantuanTerminalInterface = Enabled ? Interface : nullptr;
		return true;
	}
	void GargantuanMarkNativeEgressInvalid() { m_senderState.GargantuanFeedback.Invalid = true; }
	bool GargantuanReadPendingReceiveJournal(uint32_t &Count, uint32_t &HighWater, bool &Overflow) const {
		if (!GargantuanPendingReceive) return false;
		Count = static_cast<uint32_t>(GargantuanPendingReceive->Size());
		HighWater = static_cast<uint32_t>(GargantuanPendingReceive->MaximumObserved());
		Overflow = GargantuanPendingReceive->HasOverflowed();
		return true;
	}
	GargantuanNativeEgressTicket *GargantuanNativePendingTicket() const { return m_senderState.GargantuanPendingTicket; }
	uint64_t GargantuanNativeTerminalGeneration() const { return m_senderState.GargantuanTerminalGeneration; }
	bool GargantuanNativeTerminalPurgePending() const { return m_senderState.GargantuanTerminalPurgePending; }
	bool GargantuanNativePurged() const { return m_senderState.GargantuanFeedback.Purged; }
	void GargantuanFinishNativeTerminalPurge() { if (m_senderState.GargantuanTerminalPurgePending) SNP_ShutdownConnection(); }
	std::unique_ptr<GargantuanPendingNativeDatagramQueue> GargantuanPendingReceive;
	bool GargantuanReplayingNativeDatagrams = false;
	bool GargantuanNativeEgressFeedbackComplete() const {
		return !m_senderState.GargantuanPendingTicket &&
			(!GargantuanPendingReceive || !GargantuanPendingReceive->IsActive());
	}
	bool GargantuanReconcileNativeEgress();
	bool GargantuanReconcileNativeEgressByDeadline(SteamNetworkingMicroseconds UsecNow);
	void GargantuanDrainPendingReceived();
	void GargantuanServiceReadyNativeEgress();
	virtual bool GargantuanIsDirectUDP() const { return false; }
	bool GargantuanArmPromptFailure(bool AtSocket) {
		if (!m_senderState.GargantuanPromptFinalGrantAck || !m_senderState.GargantuanAckTrace ||
			m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken) return false;
		if (AtSocket) m_senderState.GargantuanAckTrace->FailNextPromptSocketSend = true;
		else m_senderState.GargantuanAckTrace->FailNextPromptPacket = true;
		return true;
	}
	bool GargantuanFailSocketSend(bool Prompt) {
		auto *Trace = m_senderState.GargantuanAckTrace.get();
		if (!Prompt || !Trace || !Trace->FailNextPromptSocketSend) return false;
		Trace->FailNextPromptSocketSend = false;
		++Trace->InjectedSocketSendFailures;
		Trace->FirstSentBytesAtInjectedFailure = m_senderState.GargantuanFeedback.StructuralActiveGrantFirstSentBytes;
		return true;
	}
	bool GargantuanCanReservePromptAck() const {
		const auto &Grant = m_senderState.GargantuanFeedback;
		return GargantuanIsDirectUDP() && m_senderState.GargantuanPromptFinalGrantAck && !Grant.Invalid && !Grant.Purged &&
			Grant.ActiveAttributedRetirementToken && Grant.StructuralActiveGrantBytes &&
			m_senderState.GargantuanPromptWire.Token == Grant.ActiveAttributedRetirementToken &&
			m_senderState.GargantuanPromptWire.Bytes == Grant.StructuralActiveGrantBytes &&
			m_senderState.GargantuanPromptWire.CanRequest(k_cbSteamNetworkingSocketsMaxUDPMsgLen + 48);
	}
	void GargantuanRecordIncomingWire(int Bytes) {
		if (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Incoming(Bytes);
		if (m_senderState.GargantuanPromptFinalGrantAck && m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken && Bytes > 0)
			m_senderState.GargantuanPromptWire.Charge(Bytes, 48);
	}
	void GargantuanRecordStats(bool Sent, bool Request, bool Immediate, bool Instantaneous, bool Lifetime, bool Tracer) {
		if (m_senderState.GargantuanAckTrace)
			m_senderState.GargantuanAckTrace->Stats(Sent, Request, Immediate, Instantaneous, Lifetime, Tracer);
	}
	void GargantuanRecordStatsNeed(int Mask) {
		if (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->ObservedStatsNeedMask |= Mask;
	}
	void GargantuanRecordPromptReserve(int Bytes) {
		if (auto *Trace = m_senderState.GargantuanAckTrace.get())
			Trace->MaximumPromptReserveBytes = std::max(Trace->MaximumPromptReserveBytes, uint64_t(Bytes));
	}
	bool GargantuanConfigurePromptGrantAck(bool Enabled, uint64_t Reserve, uint64_t Pool, uint64_t TailBudget, uint64_t Peers) {
		if (Enabled && (!GargantuanIsDirectUDP() || TailBudget < k_cbSteamNetworkingSocketsMaxUDPMsgLen + 48)) return false;
		if (m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken) return false;
		GargantuanPromptAckWireBudget Budget;
		Budget.StructuralPool = Pool;
		Budget.TailBudget = TailBudget;
		if (Enabled && (!Pool || !Budget.ConfigureBackground(Reserve, Peers, k_cbSteamNetworkingSocketsMaxUDPMsgLen + 48,
			k_usecLinkStatsMinPingRequestInterval / k_nMillion, k_usecLinkStatsInstantaneousReportInterval / k_nMillion,
			k_usecLinkStatsLifetimeReportInterval / k_nMillion))) return false;
		m_senderState.GargantuanPromptFinalGrantAck = Enabled;
		m_senderState.GargantuanPromptWire = Budget;
		return true;
	}
	bool GargantuanAccessAckDiagnostics(bool Reset, GargantuanAckDiagnostics &Result) {
		if (Reset) m_senderState.GargantuanAckTrace.reset(new GargantuanAckDiagnostics());
		if (!m_senderState.GargantuanAckTrace) return false;
		Result = *m_senderState.GargantuanAckTrace;
		Result.NativeSnapshotNow = SteamNetworkingSockets_GetLocalTimestamp();
		Result.NativeLastPingSent = m_statsEndToEnd.m_ping.m_usecTimeLastSentPingRequest;
		Result.NativeLastPingReceived = m_statsEndToEnd.m_ping.TimeRecvMostRecentPing();
		Result.NativeStatsInFlight = m_statsEndToEnd.m_pktNumInFlight;
		Result.NativeTracerReady = m_statsEndToEnd.ReadyToSendTracerPing(Result.NativeSnapshotNow);
		Result.NativeActivity = int(m_statsEndToEnd.GetActivityLevel());
		Result.GrantWholeWireBytes = m_senderState.GargantuanPromptWire.WireBytes;
		Result.GrantWireInvalid = m_senderState.GargantuanPromptWire.Invalid;
		Result.PromptTailBudget = m_senderState.GargantuanPromptWire.TailBudget;
		Result.BackgroundRate = m_senderState.GargantuanPromptWire.BackgroundRate;
		Result.BackgroundBurst = m_senderState.GargantuanPromptWire.BackgroundBurst;
		m_senderState.GargantuanPromptWire.Ceiling(Result.GrantWholeWireCeiling);
		return true;
	}
	// Caller holds the existing connection lock. Direct fields avoid a second
	// mutable status sample or the rate-estimator side effects of realtime status.
	bool GargantuanPopulateReliableServiceFeedback(GargantuanReliableServiceSnapshot &Result) const {
		if (!GargantuanNativeEgressFeedbackComplete()) {
			Result = {};
			Result.Counters.Invalid = true;
			return false;
		}
		GargantuanCopyReliableServiceFeedback(m_senderState.GargantuanFeedback,
			m_senderState.m_cbPendingReliable, m_senderState.m_cbSentUnackedReliable,
			static_cast<int>(GetState()), Result);
		return true;
	}

	/// Called when we close the connection locally]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_connections.cpp 724eb5215fd2ed66e691c4ac798405531b973e499f3aee26c8d9f101f6bd0ae9)
GargantuanReplaceFeedback("#include \"steamnetworkingsockets_connections.h\"" "#include \"steamnetworkingsockets_connections.h\"\n#include \"NativeEgressWorker.hpp\"")
GargantuanReplaceFeedback("#include \"NativeEgressWorker.hpp\"" "#include \"NativeEgressWorker.hpp\"\n#include \"ServiceTimingDiagnostics.hpp\"")
GargantuanReplaceFeedback("\t// Fill out a message object\n\tCSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( cbData );" [=[
	GargantuanServiceTimingSpan GargantuanCopy(GargantuanServiceTimingPhase::ApiMessageCopy);
	GargantuanCopy.SetResult(m_hConnectionSelf);
	GargantuanCopy.SetBytes(cbData);
	// Fill out a message object
	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( cbData );]=])
GargantuanReplaceFeedback("\t// Copy in the payload\n\tmemcpy( pMsg->m_pData, pData, cbData );" "\t// Copy in the payload\n\tmemcpy( pMsg->m_pData, pData, cbData );\n\tGargantuanCopy.Finish();")
GargantuanReplaceFeedback("void CSteamNetworkConnectionBase::FreeResources()\n{\n\tAssertLocksHeldByCurrentThread();" [=[void CSteamNetworkConnectionBase::FreeResources()
{
	AssertLocksHeldByCurrentThread();
	// Normal terminal Think reconciles asynchronously. Forced FreeResources is
	// the final lifetime fence, including direct destruction paths.
	if (auto *Ticket = m_senderState.GargantuanPendingTicket) {
		GargantuanCancelUnstartedNativeEgress(Ticket);
		GargantuanNativeEgressResult Result{};
		if (!GargantuanWaitNativeEgressResult(Ticket, 250, Result))
			GargantuanFailStopNativeEgress("forced connection free with unresolved native send");
		if (!GargantuanReconcileNativeEgress())
			GargantuanFailStopNativeEgress("forced connection free cannot reconcile native send");
		GargantuanDrainPendingReceived();
		SNP_ShutdownConnection();
	}]=])
GargantuanReplaceFeedback("void CSteamNetworkConnectionBase::Think( SteamNetworkingMicroseconds usecNow )\n{" [=[void CSteamNetworkConnectionBase::Think( SteamNetworkingMicroseconds usecNow )
{]=])
GargantuanReplaceFeedback("\t// Check our state\n\tswitch ( m_eConnectionState )\n\t{\n\t\tcase k_ESteamNetworkingConnectionState_Dead:\n\t\tcase k_ESteamNetworkingConnectionState_None:\n\t\tdefault:\n\t\t\t// WAT" [=[
	if (m_senderState.GargantuanTerminalPurgePending) {
		if (!GargantuanReconcileNativeEgressByDeadline(usecNow)) {
			SetNextThinkTime(usecNow + 1000);
			return;
		}
		GargantuanDrainPendingReceived();
		SNP_ShutdownConnection();
	}
	// Check our state
	switch ( m_eConnectionState )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			// WAT]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_udp.h ea4f517b674eb15f367c8b443a90738bebdcfb3f892b2a59ba26829eabf4ccea)
GargantuanReplaceFeedback("#pragma once" "#pragma once\n#include \"NativeEgressWorker.hpp\"")
GargantuanReplaceFeedback("\tvirtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;" "\tvirtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;\n\tvirtual GargantuanNativeEgressQueueStatus GargantuanQueueNativeDatagram( int ChunkCount, const iovec *Chunks, int TotalBytes, bool Protected, GargantuanNativeEgressTicket *&Ticket );\n\tvoid GargantuanReplayReceivedData(const uint8 *Packet, int Bytes, SteamNetworkingMicroseconds ReceivedAtMicroseconds) { Received_Data(Packet, Bytes, ReceivedAtMicroseconds); }")
GargantuanReplaceFeedback("\tvirtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;" "\tvirtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;\n\tvirtual GargantuanNativeEgressQueueStatus GargantuanQueueNativeDatagram( int ChunkCount, const iovec *Chunks, int TotalBytes, bool Protected, GargantuanNativeEgressTicket *&Ticket ) override;")
GargantuanReplaceFeedback("\tint m_nStatsNeed;" "\tint m_nStatsNeed;\n\tint m_nGargantuanTracerReady = 0;")
GargantuanReplaceFeedback("\tCSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock );" "\tbool GargantuanIsDirectUDP() const override { return true; }\n\tCSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock );")
GargantuanReplaceFeedback("\tvoid Trim( int cbHdrOutSpaceRemaining );" "\tvoid GargantuanReservePromptAck(size_t HeaderBytes, CSteamNetworkConnectionBase &Connection);\n\tvoid Trim( int cbHdrOutSpaceRemaining );")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_udp.cpp a60888c40ea5a814485e56c1c528774d05df408130137fd909f66fc25d0aee2e)
GargantuanReplaceFeedback("#include \"steamnetworkingsockets_udp.h\"" "#include \"steamnetworkingsockets_udp.h\"\n#include \"NativeEgressWorker.hpp\"")
GargantuanReplaceFeedback("void CConnectionTransportUDPBase::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )" [=[
void CSteamNetworkConnectionBase::GargantuanDrainPendingReceived()
{
	auto *Queue = GargantuanPendingReceive.get();
	if (!Queue || !Queue->IsActive() || m_senderState.GargantuanPendingTicket) return;
	GargantuanReplayingNativeDatagrams = true;
	GargantuanPendingNativeDatagramQueue::Datagram Datagram;
	while (Queue->Take(Datagram)) {
		if (!m_pTransport || !GargantuanIsDirectUDP()) {
			m_senderState.GargantuanFeedback.Invalid = true;
			continue;
		}
		static_cast<CConnectionTransportUDPBase *>(m_pTransport)->GargantuanReplayReceivedData(
			Datagram.Bytes.data(), Datagram.Size,
			std::max(static_cast<SteamNetworkingMicroseconds>(Datagram.ReceivedAtMicroseconds),
				m_senderState.GargantuanLastCompletedGnsAt));
	}
	if (Queue->HasOverflowed()) m_senderState.GargantuanFeedback.Invalid = true;
	if (!Queue->End()) GargantuanFailStopNativeEgress("pending receive replay incomplete");
	GargantuanReplayingNativeDatagrams = false;
}

void CConnectionTransportUDPBase::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )]=])
GargantuanReplaceFeedback("int CConnectionTransportUDPBase::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctxBase )" [=[
GargantuanNativeEgressQueueStatus CConnectionTransportUDPBase::GargantuanQueueNativeDatagram(
	int, const iovec *, int, bool, GargantuanNativeEgressTicket *&) {
	return GargantuanNativeEgressQueueStatus::Disabled;
}

int CConnectionTransportUDPBase::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctxBase )]=])
GargantuanReplaceFeedback("\t// Send it\n\tif ( SendPacketGather( 2, gather, cbSend ) )" [=[
	// The numbered packet is now immutable.  A queued attempt is not a
	// positive send; SNP retains its packet and segment references until the
	// actor receives the actual OS result.
	if (ctx.m_bGargantuanUseNativeEgress) {
		GargantuanNativeEgressTicket *Ticket = nullptr;
		const auto Status = GargantuanQueueNativeDatagram(2, gather, cbSend,
			ctx.m_bGargantuanProtectedEgress || ctx.msg.has_stats(), Ticket);
		if (Status == GargantuanNativeEgressQueueStatus::Queued && Ticket) {
			ctx.m_pGargantuanNativeEgressTicket = Ticket;
			ctx.m_nGargantuanQueuedBytes = cbSend;
			return -1; // Distinct pending result; never native service.
		}
		m_connection.GargantuanMarkNativeEgressInvalid();
		return 0; // Rejected before OS execution; ordinary retry ownership.
	}
	// Send it
	if ( SendPacketGather( 2, gather, cbSend ) )]=])
GargantuanReplaceFeedback("\tconst uint8 *pIn = pPkt + sizeof(*hdr);" [=[
	// Accept no ACK, sequence or stats side effects while a local numbered
	// packet has an unresolved native outcome.  Overflow is packet loss at the
	// UDP boundary, and invalidates qualification rather than fabricating ACK
	// history or F1 progress.
	if (auto *Queue = m_connection.GargantuanPendingReceive.get()) {
		if (Queue->IsActive() && !m_connection.GargantuanReplayingNativeDatagrams) {
			if (!Queue->Push(pPkt, cbPkt, usecNow))
				m_connection.GargantuanMarkNativeEgressInvalid();
			return;
		}
	}
	const uint8 *pIn = pPkt + sizeof(*hdr);]=])
GargantuanReplaceFeedback("bool CConnectionTransportUDP::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )" [=[
GargantuanNativeEgressQueueStatus CConnectionTransportUDP::GargantuanQueueNativeDatagram(
	int ChunkCount, const iovec *Chunks, int TotalBytes, bool Protected,
	GargantuanNativeEgressTicket *&Ticket) {
	if (!m_pSocket) return GargantuanNativeEgressQueueStatus::Rejected;
	m_connection.m_statsEndToEnd.TrackSentPacket(TotalBytes);
	return GargantuanQueueNativeEgress(m_pSocket, ChunkCount, Chunks, TotalBytes, Protected, Ticket);
}

bool CConnectionTransportUDP::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )]=])
GargantuanReplaceFeedback("\tconst uint8 *pIn = pPkt + sizeof(*hdr);" "\t// Charge once after header/connection/state association, before decrypt can\n\t// reject a duplicate, old sequence, or crypto-invalid consumed datagram.\n\tm_connection.GargantuanRecordIncomingWire(cbPkt);\n\tconst uint8 *pIn = pPkt + sizeof(*hdr);")
GargantuanReplaceFeedback("\tif ( SendPacketGather( 2, gather, cbSend ) )" "\t// Distinct opt-in socket failure: preserve packet-number and TrackSentStats\n\t// mutation, but never transmit a packet and then pretend it failed.\n\tif ( !m_connection.GargantuanFailSocketSend(ctx.m_bGargantuanPromptAck) && SendPacketGather( 2, gather, cbSend ) )")
GargantuanReplaceFeedback("\t\treturn cbSend;\n\treturn 0;" [=[
	{
		m_connection.GargantuanRecordStats(true, (ctx.msg.flags() & ctx.msg.ACK_REQUEST_E2E) != 0,
			(ctx.msg.flags() & ctx.msg.ACK_REQUEST_IMMEDIATE) != 0,
			ctx.msg.stats().has_instantaneous(), ctx.msg.stats().has_lifetime(), ctx.m_nGargantuanTracerReady > 0);
		return cbSend;
	}
	return 0;]=])
GargantuanReplaceFeedback("\t// Connection quality stats?" [=[
	m_connection.GargantuanRecordStats(false, (msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_E2E) != 0,
		(msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_IMMEDIATE) != 0,
		msgStatsIn.stats().has_instantaneous(), msgStatsIn.stats().has_lifetime(), false);
	// Connection quality stats?]=])
GargantuanReplaceFeedback("\tint nReadyToSendTracer = 0;" "\tm_nGargantuanTracerReady = statsEndToEnd.ReadyToSendTracerPing(m_usecNow); // Pure observation before reply-request branches.\n\tint nReadyToSendTracer = 0;")
GargantuanReplaceFeedback("\tm_nStatsNeed = statsEndToEnd.GetStatsSendNeed( m_usecNow );" "\tm_nStatsNeed = statsEndToEnd.GetStatsSendNeed( m_usecNow );\n\tconnection.GargantuanRecordStatsNeed(m_nStatsNeed);")
GargantuanReplaceFeedback("\t// Save time when we sent the last sequenced packet." [=[
	if (ctx.m_bGargantuanPromptAck)
	{
		Assert(ctx.m_bGargantuanPromptAckReserved);
		ctx.m_nFlags |= ctx.msg.ACK_REQUEST_E2E | ctx.msg.ACK_REQUEST_IMMEDIATE;
		ctx.SlamFlagsAndCalcSize();
	}
	// Save time when we sent the last sequenced packet.]=])
GargantuanReplaceFeedback("\nvoid UDPSendPacketContext_t::Trim( int cbHdrOutSpaceRemaining )" [=[

void UDPSendPacketContext_t::GargantuanReservePromptAck(size_t HeaderBytes, CSteamNetworkConnectionBase &Connection)
{
	if (!Connection.GargantuanCanReservePromptAck()) return;
	const auto OriginalFlags = m_nFlags;
	const int OriginalMaximum = m_cbMaxEncryptedPayload;
	m_nFlags |= msg.ACK_REQUEST_E2E | msg.ACK_REQUEST_IMMEDIATE;
	SlamFlagsAndCalcSize();
	CalcMaxEncryptedPayloadSize(HeaderBytes, &Connection);
	const int PromptMaximum = m_cbMaxEncryptedPayload;
	m_nFlags = OriginalFlags;
	SlamFlagsAndCalcSize();
	m_cbMaxEncryptedPayload = std::min(OriginalMaximum, PromptMaximum);
	m_bGargantuanPromptAckReserved = true;
	Connection.GargantuanRecordPromptReserve(OriginalMaximum - m_cbMaxEncryptedPayload);
}

void UDPSendPacketContext_t::Trim( int cbHdrOutSpaceRemaining )]=])
GargantuanReplaceFeedback("\t// Would we like to try to send some additional stats, if there is room?" "\tGargantuanReservePromptAck(cbHdrtReserve, connection);\n\t// Would we like to try to send some additional stats, if there is room?")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(csteamnetworkingsockets.cpp 2b260c05cc8c262e785387ea3d08eee74cc03b6eed00493e961a82c05dec5451)
GargantuanReplaceFeedback("#include \"steamnetworkingsockets_connections.h\"" "#include \"steamnetworkingsockets_connections.h\"\n#include \"ServiceTimingDiagnostics.hpp\"")
GargantuanReplaceFeedback("EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber )\n{" [=[EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber )
{
	GargantuanInitialServiceApiStage(false);
	GargantuanServiceTimingSpan GargantuanApiLock(GargantuanServiceTimingPhase::ApiConnectionLockWait);
	GargantuanApiLock.SetResult(hConn);
	GargantuanApiLock.SetBytes(cbData);]=])
GargantuanReplaceFeedback("\tCSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, \"SendMessageToConnection\" );" [=[
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "SendMessageToConnection" );
	GargantuanApiLock.Finish();
	GargantuanInitialServiceApiStage(true);]=])
GargantuanReplaceFeedback("static CSteamNetworkListenSocketBase *GetListenSocketByHandle" [=[
static CSteamNetworkConnectionBase *GargantuanPendingConnections[64]{};

bool GargantuanRegisterNativePending(CSteamNetworkConnectionBase *Connection) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	if (!Connection) return false;
	for (auto *Entry : GargantuanPendingConnections) if (Entry == Connection) return false;
	for (auto &Entry : GargantuanPendingConnections) {
		if (!Entry) { Entry = Connection; return true; }
	}
	return false;
}

void GargantuanUnregisterNativePending(CSteamNetworkConnectionBase *Connection) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	for (auto &Entry : GargantuanPendingConnections) {
		if (Entry == Connection) { Entry = nullptr; return; }
	}
	GargantuanFailStopNativeEgress("pending connection registry missing entry");
}

void GargantuanWakeReadyNativeEgressConnections() noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	// One fixed registry pass. A newly queued packet never causes a rescan,
	// and a contended peer cannot block another peer's ready result.
	for (auto *Connection : GargantuanPendingConnections) {
		if (!Connection) continue;
		// Pending-ticket publication and release both require the global lock;
		// Ready is immutable until release. Unready peers need no actor lock.
		GargantuanNativeEgressResult Result{};
		if (!GargantuanReadNativeEgressResult(Connection->GargantuanNativePendingTicket(), Result)) continue;
		if (!Connection->TryLock()) {
			Connection->SetNextThinkTimeASAP();
			continue;
		}
		ConnectionScopeLock Lock;
		Lock.TakeLockOwnership(Connection->m_pLock, "GargantuanReadyNativeEgress");
		Connection->GargantuanServiceReadyNativeEgress();
	}
}

bool GargantuanConfigureNativeEgress(ISteamNetworkingSockets *Interface, uint32 Handle,
	bool Enabled, uint64_t *Generation) {
	if (Generation) *Generation = 0;
	SteamNetworkingGlobalLock GlobalLock("GargantuanNativeEgressConfigure");
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanNativeEgress");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	uint64_t ReservedGeneration = 0;
	if (Enabled && (!GargantuanCanQueueNativeEgress() ||
		!GargantuanReserveNativeTerminalEvidence(Interface, Handle, ReservedGeneration))) return false;
	const uint64_t PriorGeneration = Connection->GargantuanNativeTerminalGeneration();
	if (!Connection->GargantuanConfigureNativeEgress(Enabled, ReservedGeneration, Interface)) {
		if (ReservedGeneration) GargantuanReleaseNativeTerminalEvidence(Interface, Handle, ReservedGeneration);
		return false;
	}
	if (!Enabled && PriorGeneration &&
		!GargantuanReleaseNativeTerminalEvidence(Interface, Handle, PriorGeneration))
		GargantuanFailStopNativeEgress("native opt-out lost terminal reservation");
	if (Generation) *Generation = ReservedGeneration;
	return true;
}

bool GargantuanConfigureNativeEgressWorkers(ISteamNetworkingSockets *Interface,
	bool Enabled, unsigned Workers) {
	if (!Interface) return false;
	SteamNetworkingGlobalLock GlobalLock("GargantuanNativeEgressWorkers");
	return GargantuanConfigureNativeEgress(Enabled, Workers);
}

bool GargantuanReadPendingNativeReceiveJournal(ISteamNetworkingSockets *Interface,
	uint32 Handle, uint32_t &Count, uint32_t &HighWater, bool &Overflow) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanPendingReceiveJournal");
	return Connection && Connection->m_pSteamNetworkingSocketsInterface == Interface &&
		Connection->GargantuanReadPendingReceiveJournal(Count, HighWater, Overflow);
}

bool GargantuanDrainNativeConnection(ISteamNetworkingSockets *Interface,
	uint32 Handle, uint64_t Generation, uint32 TimeoutMilliseconds) {
	if (!Interface || !Generation) return false;
	SteamNetworkingGlobalLock GlobalLock("GargantuanNativeEgressDrainConnection");
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandle(Handle, Lock);
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface ||
		Connection->GargantuanNativeTerminalGeneration() != Generation) return false;
	if (auto *Ticket = Connection->GargantuanNativePendingTicket()) {
		GargantuanCancelUnstartedNativeEgress(Ticket);
		GargantuanNativeEgressResult Result{};
		if (!GargantuanWaitNativeEgressResult(Ticket, TimeoutMilliseconds, Result))
			GargantuanFailStopNativeEgress("connection drain exceeded bounded OS call");
		if (!Connection->GargantuanReconcileNativeEgress())
			GargantuanFailStopNativeEgress("connection drain result not visible");
		Connection->GargantuanDrainPendingReceived();
	}
	Connection->GargantuanFinishNativeTerminalPurge();
	return Connection->GargantuanNativePurged();
}

bool GargantuanDrainNativeEgress(ISteamNetworkingSockets *Interface, uint32 TimeoutMilliseconds) {
	if (!Interface) return false;
	{
		SteamNetworkingGlobalLock GlobalLock("GargantuanNativeEgressFence");
		GargantuanFenceNativeEgress();
	}
	if (!GargantuanWaitNativeEgressIdle(TimeoutMilliseconds))
		GargantuanFailStopNativeEgress("global drain exceeded bounded OS call");
	SteamNetworkingGlobalLock GlobalLock("GargantuanNativeEgressDrain");
	FOR_EACH_HASHMAP(g_mapConnections, Index) {
		auto *Connection = g_mapConnections[Index];
		if (Connection->m_pSteamNetworkingSocketsInterface != Interface) continue;
		ConnectionScopeLock Lock(*Connection);
		if (Connection->GargantuanNativePendingTicket()) {
			if (!Connection->GargantuanReconcileNativeEgress())
				GargantuanFailStopNativeEgress("global drain result not visible");
			Connection->GargantuanDrainPendingReceived();
		}
		Connection->GargantuanFinishNativeTerminalPurge();
	}
	return true;
}

bool GargantuanArmPromptFailure(ISteamNetworkingSockets *Interface, uint32 Handle, bool AtSocket) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanPromptFailure");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	return Connection->GargantuanArmPromptFailure(AtSocket);
}

bool GargantuanConfigurePromptGrantAck(ISteamNetworkingSockets *Interface, uint32 Handle, bool Enabled,
	uint64_t Reserve, uint64_t Pool, uint64_t TailBudget, uint64_t Peers) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanPromptGrantAck");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	return Connection->GargantuanConfigurePromptGrantAck(Enabled, Reserve, Pool, TailBudget, Peers);
}

bool GargantuanConfigureFundedGrantAck(ISteamNetworkingSockets *Interface, uint32 Handle,
	uint64_t Reserve, uint64_t Pool, uint64_t Peers) {
	// The final packet and its immediate response use the pinned maximum
	// datagram plus conservative IPv6/UDP headers, never a minimum-size Q gate.
	return GargantuanConfigurePromptGrantAck(Interface, Handle, true, Reserve, Pool,
		k_cbSteamNetworkingSocketsMaxUDPMsgLen + 48, Peers);
}

bool GargantuanAccessAckDiagnostics(ISteamNetworkingSockets *Interface, uint32 Handle,
	bool Reset, GargantuanAckDiagnostics &Result) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanAckDiagnostics");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	return Connection->GargantuanAccessAckDiagnostics(Reset, Result);
}

bool GargantuanReadNativeFeedback(ISteamNetworkingSockets *Interface, uint32 Handle, GargantuanReliableServiceSnapshot &Result) {
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanReliableServiceFeedback");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface) return false;
	return Connection->GargantuanPopulateReliableServiceFeedback(Result);
}

bool GargantuanReadResolvedNativeFeedback(ISteamNetworkingSockets *Interface, uint32 Handle,
	uint64_t DeadlineMicroseconds, GargantuanReliableServiceSnapshot &Result) {
	SteamNetworkingGlobalLock GlobalLock("GargantuanCessationFeedback");
	ConnectionScopeLock Lock;
	auto *Connection = GetConnectionByHandleForAPI(Handle, Lock, "GargantuanCessationFeedback");
	if (!Connection || Connection->m_pSteamNetworkingSocketsInterface != Interface ||
		!DeadlineMicroseconds || GargantuanReliableServiceClock() >= DeadlineMicroseconds) return false;
	if (auto *Ticket = Connection->GargantuanNativePendingTicket()) {
		GargantuanNativeEgressResult Native{};
		const auto Now = GargantuanReliableServiceClock();
		if (Now >= DeadlineMicroseconds) return false;
		const auto Remaining = DeadlineMicroseconds - Now;
		// Rounded wait is checked against the absolute deadline after returning.
		const auto Milliseconds = static_cast<uint32>((std::min<uint64_t>(Remaining, 250000) + 999) / 1000);
		if (!GargantuanWaitNativeEgressResult(Ticket, Milliseconds, Native) ||
			GargantuanReliableServiceClock() >= DeadlineMicroseconds) return false;
		if (!Connection->GargantuanReconcileNativeEgress()) return false;
	}
	Connection->GargantuanDrainPendingReceived();
	return GargantuanReliableServiceClock() < DeadlineMicroseconds &&
		Connection->GargantuanPopulateReliableServiceFeedback(Result) && !Result.Counters.Invalid;
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
add_library(gargantuan_gns_ordinary_pacer OBJECT "${GargantuanFeedbackDirectory}/OrdinaryWirePacer.cpp")
target_compile_features(gargantuan_gns_ordinary_pacer PRIVATE cxx_std_17)
target_sources(GameNetworkingSockets_s PRIVATE $<TARGET_OBJECTS:gargantuan_gns_ordinary_pacer>)
