// Included inside the pinned SNP namespace. No packet/message/reference owner
// is introduced: this selects which existing collector inputs may enter a packet.
GargantuanDataClass CSteamNetworkConnectionBase::GargantuanPlanData(
	SteamNetworkingMicroseconds Now, GargantuanOrdinaryWirePacer::Permit *Permit,
	uint64_t &Cutoff, SteamNetworkingMicroseconds *Eligible,
	GargantuanOrdinaryWirePacer::Permit *AggregatePermit) const
{
	auto &State = m_senderState;
	const auto Flow = uint32_t(m_hConnectionSelf);
	State.GargantuanDataFlow = Flow;
	auto &Protected = GargantuanProtectedWirePacer(k_cbSteamNetworkingSocketsMaxUDPMsgLen);
	auto &Retained = GargantuanRetainedWirePacer(k_cbSteamNetworkingSocketsMaxUDPMsgLen);
	auto &Aggregate = GargantuanSharedOrdinaryWirePacer(k_cbSteamNetworkingSocketsMaxUDPMsgLen);
	if (State.GargantuanPendingTicket || m_sendRateData.m_flTokenBucket < 0) {
		// A non-eligible connection must not hold either class's ready turn.
		// Keep request age so repeated protected sends cannot reset retry fairness.
		Protected.Suspend(Flow); Retained.Suspend(Flow);
		Cutoff = 0;
		if (Eligible) *Eligible = 0; // Preserve GNS's own pending/token wake deadline.
		return GargantuanDataClass::None;
	}
	const auto &Grant = State.GargantuanFeedback;
	if (Grant.ActiveAttributedRetirementToken &&
		Grant.StructuralActiveGrantFirstSentBytes < Grant.StructuralActiveGrantBytes) {
		// A future structural prefix must not hold a ready ordinary/retry turn.
		Protected.Forget(Flow); Retained.Forget(Flow);
		Cutoff = Grant.ActiveAttributedMessageNumber;
		if (Eligible) *Eligible = 0;
		return GargantuanDataClass::Structural;
	}
	Cutoff = State.GargantuanProtectedOrdinary.LastPendingMessage();
	const auto RetryHead = State.m_listReadyRetryReliableRange.Head();
	const bool HasRetry = RetryHead != State.m_listReadyRetryReliableRange.InvalidIndex();
	const bool PositiveRetry = HasRetry &&
		State.m_listSentReliableSegments[State.m_listReadyRetryReliableRange[RetryHead]].m_bGargantuanEverSent;
	const bool HasRetained = PositiveRetry || (!Cutoff && (HasRetry || !State.m_messagesQueued.empty()));
	if (!Cutoff) Protected.Forget(Flow);
	if (!HasRetained) Retained.Forget(Flow);
	const auto ProtectedAt = Cutoff ? Protected.EligibleAt(Now, Flow) : k_nThinkTime_Never;
	const auto RetainedAt = HasRetained ? Retained.EligibleAt(Now, Flow) : k_nThinkTime_Never;
	const SteamNetworkingMicroseconds AggregateAt = Aggregate.EligibleAt(Now);
	if (Eligible) *Eligible = std::max(AggregateAt, std::min(ProtectedAt, RetainedAt));
	if (AggregateAt > Now) return GargantuanDataClass::None;
	const auto Selected = GargantuanSelectDataClass(false, Cutoff != 0, HasRetained,
		ProtectedAt <= Now, RetainedAt <= Now, State.GargantuanProtectedPacketsSinceRetry);
	if (!Permit) return Selected;
	if (Selected == GargantuanDataClass::None || !AggregatePermit || !AggregatePermit->Reserve(Aggregate, Now)) return GargantuanDataClass::None;
	if (Selected == GargantuanDataClass::Protected && Permit->Reserve(Protected, Now, Flow)) { Retained.Suspend(Flow); return Selected; }
	if (Selected == GargantuanDataClass::Retained && Permit->Reserve(Retained, Now, Flow)) { Protected.Suspend(Flow); return Selected; }
	AggregatePermit->Cancel();
	return GargantuanDataClass::None; // ACK/NACK serialization remains independent.
}

static uint16 GargantuanFirstUniqueRetry(const SSNPSenderState &State)
{
	// Single-lane first-send order is monotonic. A failed packet establishes the
	// unsent frontier; retry it before advancing new-message cursors. Consequently
	// never-positive retry ranges form the queue suffix. Walking that suffix
	// avoids rescanning every ACK-retained positive range on each ordinary packet.
	const auto &Queue = State.m_listReadyRetryReliableRange;
	uint16 First = Queue.InvalidIndex();
	for (auto Link = Queue.Tail(); Link != Queue.InvalidIndex(); Link = Queue.Previous(Link)) {
		if (State.m_listSentReliableSegments[Queue[Link]].m_bGargantuanEverSent) break;
		First = Link;
	}
	return First;
}
