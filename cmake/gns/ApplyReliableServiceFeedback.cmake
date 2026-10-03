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
GargantuanReplaceFeedback("struct SSNPSenderState\n{" "struct SSNPSenderState\n{\n\tstd::unique_ptr<GargantuanAckDiagnostics> GargantuanAckTrace;\n\tGargantuanPromptAckWireBudget GargantuanPromptWire;\n\tbool GargantuanPromptFinalGrantAck = false; // Enabled only by funded attributed structural submission.\n\tGargantuanReliableServiceCounters GargantuanFeedback;\n\tbool GargantuanRunningStructuralGrant = false;")
GargantuanReplaceFeedback("\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;" "\t// A failed SendEncryptedDataChunk queues retry without first-send service.\n\tbool m_bGargantuanEverSent;\n\n\tstatic constexpr uint16 k_nStatus_InFlight = 0xffff;")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_snp.cpp 99e2b190b17993139bd3251f8862b81b58903a119ea456edacfec68f3dd9d65c)
GargantuanReplaceFeedback("void SSNPSenderState::Shutdown()\n{" "void SSNPSenderState::Shutdown()\n{\n\tGargantuanSetRunningStructuralGrant(GargantuanRunningStructuralGrant, false);\n\tGargantuanFeedback.Purged = true; // Purge is never ACK retirement.")
GargantuanReplaceFeedback("\t\tpMsg->Unlink();\n\t\tpMsg->Release();" "\t\tif (GargantuanAckTrace) GargantuanAckTrace->Record(GargantuanAckDiagnostics::MessageAcked, SteamNetworkingSockets_GetLocalTimestamp(), pMsg->m_nMessageNumber, pMsg->m_cbSize);\n\t\tGargantuanFeedback.AckMessage(pMsg->m_nMessageNumber, pMsg->m_cbSize, info.m_cbHdr);\n\t\tpMsg->Unlink();\n\t\tpMsg->Release();")
GargantuanReplaceFeedback("\tAssertLocksHeldByCurrentThread( \"SNP_SendPacket\" );" "\tAssertLocksHeldByCurrentThread( \"SNP_SendPacket\" );\n\tif (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->SerializedAckPacket = 0;")
GargantuanReplaceFeedback("\t\tSNP_RecordReceivedPktNum( nPktNum, usecNow, bScheduleAck );" "\t\tSNP_RecordReceivedPktNum( nPktNum, usecNow, bScheduleAck );\n\t\tif (bScheduleAck && m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Received(usecNow, nPktNum, m_receiverState.TimeWhenFlushAcks());")
GargantuanReplaceFeedback("\t\tpReliableDecode += cbMsgSize;" "\t\tif (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::MessageReceived, usecNow, nMsgNum, cbMsgSize);\n\t\tpReliableDecode += cbMsgSize;")
GargantuanReplaceFeedback("\t\tm_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior = INT64_MAX; // Clear timer, we wrote everything we needed to" "\t\tif (m_senderState.GargantuanAckTrace) {\n\t\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nLastPktToAck;\n\t\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nLastPktToAck, 0);\n\t\t}\n\t\tm_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior = INT64_MAX; // Clear timer, we wrote everything we needed to")
GargantuanReplaceFeedback("\t// Fit as many blocks as possible." "\tif (m_senderState.GargantuanAckTrace) m_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::FragmentedAckSerialized, helper.UsecNow(), nLastPktToAck, 0);\n\t// Fit as many blocks as possible.")
GargantuanReplaceFeedback("\t\t\t*pLatestPktNum = LittleWord( uint16( nLastRecvPktNum ) );" "\t\t\t*pLatestPktNum = LittleWord( uint16( nLastRecvPktNum ) );\n\t\t\tif (m_senderState.GargantuanAckTrace) {\n\t\t\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nLastRecvPktNum;\n\t\t\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nLastRecvPktNum, 0);\n\t\t\t}")
GargantuanReplaceFeedback("\t++nAckEnd;\n\n\t#ifdef SNP_ENABLE_PACKETSENDLOG" "\t++nAckEnd;\n\tif (m_senderState.GargantuanAckTrace) {\n\t\tm_senderState.GargantuanAckTrace->SerializedAckPacket = nAckEnd - 1;\n\t\tm_senderState.GargantuanAckTrace->Record(GargantuanAckDiagnostics::AckSerialized, helper.UsecNow(), nAckEnd - 1, nBlocks);\n\t}\n\n\t#ifdef SNP_ENABLE_PACKETSENDLOG")
GargantuanReplaceFeedback("\t// OK, we have a plaintext payload.  Encrypt and send it." [=[
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
		if ( nGargantuanAttribution.Token ) {
			m_senderState.GargantuanFeedback.AttributeMessage( nGargantuanAttribution.Token,
				pSendMessage->m_nMessageNumber, pSendMessage->m_cbSize,
				nGargantuanAttribution.ActivatedAtMicroseconds );
			if (!m_senderState.GargantuanFeedback.Invalid && !m_senderState.GargantuanFeedback.Purged &&
				m_senderState.GargantuanFeedback.ActiveAttributedRetirementToken == nGargantuanAttribution.Token)
				m_senderState.GargantuanPromptWire.Begin(nGargantuanAttribution.Token, pSendMessage->m_cbSize);
			else m_senderState.GargantuanPromptWire.Invalid = true;
		}
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
GargantuanReplaceFeedback("\tint m_cbMaxEncryptedPayload;" "\tint m_cbMaxEncryptedPayload;\n\tbool m_bGargantuanPromptAckReserved = false;\n\tbool m_bGargantuanPromptAck = false;")
GargantuanReplaceFeedback("\t/// Called when we close the connection locally" [=[
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
	void GargantuanPopulateReliableServiceFeedback(GargantuanReliableServiceSnapshot &Result) const {
		GargantuanCopyReliableServiceFeedback(m_senderState.GargantuanFeedback,
			m_senderState.m_cbPendingReliable, m_senderState.m_cbSentUnackedReliable,
			static_cast<int>(GetState()), Result);
	}

	/// Called when we close the connection locally]=])
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_udp.h ea4f517b674eb15f367c8b443a90738bebdcfb3f892b2a59ba26829eabf4ccea)
GargantuanReplaceFeedback("\tint m_nStatsNeed;" "\tint m_nStatsNeed;\n\tint m_nGargantuanTracerReady = 0;")
GargantuanReplaceFeedback("\tCSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock );" "\tbool GargantuanIsDirectUDP() const override { return true; }\n\tCSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock );")
GargantuanReplaceFeedback("\tvoid Trim( int cbHdrOutSpaceRemaining );" "\tvoid GargantuanReservePromptAck(size_t HeaderBytes, CSteamNetworkConnectionBase &Connection);\n\tvoid Trim( int cbHdrOutSpaceRemaining );")
GargantuanWriteFeedbackSource()

GargantuanReadFeedbackSource(steamnetworkingsockets_udp.cpp a60888c40ea5a814485e56c1c528774d05df408130137fd909f66fc25d0aee2e)
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
GargantuanReplaceFeedback("static CSteamNetworkListenSocketBase *GetListenSocketByHandle" [=[
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
