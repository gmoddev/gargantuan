#include <steam/steamnetworkingsockets.h>
#include <steamnetworkingsockets_thinker.h>
#include <clientlib/steamnetworkingsockets_lowlevel.h>
#include <clientlib/steamnetworkingsockets_connections.h>
#include <clientlib/steamnetworkingsockets_udp.h>
#include "NativeEgressWorker.hpp"
#include "ReliableServiceFeedback.hpp"
#include "AckDiagnostics.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode(bool);
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_Poll(int);

namespace SteamNetworkingSocketsLib {
struct GargantuanDataArbitrationTest {
	static bool ControlOnlyDoesNotHoldDataCredit() {
		SteamNetworkingSockets_SetManualPollMode(true);
		auto *Api = SteamNetworkingSockets();
		HSteamNetConnection Source = 0, Target = 0;
		bool Passed = Api->CreateSocketPair(&Source, &Target, true, nullptr, nullptr);
		std::array<unsigned char, 4000> Payload{};
		Passed = Passed && Api->SendMessageToConnection(Source, Payload.data(), int(Payload.size()), k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK;
		{
			SteamNetworkingGlobalLock Global("ControlOnlyDataArrange");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Source, Lock);
			if (!Connection) Passed = false;
			else {
				auto &State = Connection->m_senderState;
				Connection->m_sendRateData.m_flCurrentSendRateUsed = 18.0f * 1024 * 1024;
				auto Now = SteamNetworkingSockets_GetLocalTimestamp();
				for (unsigned Packet = 0; Packet < 8 && !State.m_messagesQueued.empty(); ++Packet) {
					Connection->SNP_TokenBucket_Accumulate(Now += 1000);
					Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
				}
				Passed = State.m_messagesQueued.empty() && Passed;
				for (auto &Flight : State.m_mapInFlightPacketsByPktNum)
					Connection->SNP_QueueReliableSegmentsForRetry(Flight.second, Flight.first, "Control-only fixture NACK");
			}
		}
		std::uint64_t Generation = 0;
		Passed = Passed && GargantuanConfigureNativeEgress(Api, Source, true, &Generation, true);
		Passed = Passed && GargantuanArmNativeEgressPauseForTest(GargantuanNativeEgressTestPausePoint::AfterOsReturn, 250);
		if (Passed) {
			SteamNetworkingGlobalLock Global("ControlOnlyDataCredit");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Source, Lock);
			if (!Connection) Passed = false;
			else {
				const auto Now = SteamNetworkingSockets_GetLocalTimestamp() + 10000;
				Connection->SNP_TokenBucket_Accumulate(Now);
				UDPSendPacketContext_t Context(Now, "small control-only native regression");
				Context.Populate(sizeof(UDPDataMsgHdr), k_EStatsReplyRequest_NothingToSend, static_cast<CConnectionTransportUDPBase *>(Connection->m_pTransport));
				Context.m_cbMaxEncryptedPayload = 100; // Existing positive retry cannot fit; STOP_WAITING still can.
				const auto Unique = Connection->m_senderState.GargantuanFeedback.UniqueReliableStreamBytesFirstSent;
				const auto Retry = Connection->m_senderState.GargantuanFeedback.ReliableStreamBytesRetransmitted;
				Passed = Connection->SNP_SendPacket(Connection->m_pTransport, Context) && GargantuanWaitNativeEgressPauseForTest(100) && Passed;
				Passed = GargantuanSharedOrdinaryWirePacer(1300).EligibleAt(Now) <= Now &&
					GargantuanRetainedWirePacer(1300).EligibleAt(Now) <= Now && Passed;
				auto *Ticket = Connection->GargantuanNativePendingTicket();
				GargantuanResumeNativeEgressForTest();
				GargantuanNativeEgressResult Native{};
				Passed = Ticket && GargantuanWaitNativeEgressResult(Ticket, 100, Native) && Native.Positive && Native.Bytes < 300 &&
					Connection->GargantuanReconcileNativeEgress() && Passed;
				Connection->GargantuanDrainPendingReceived();
				Passed = Connection->m_senderState.GargantuanFeedback.UniqueReliableStreamBytesFirstSent == Unique &&
					Connection->m_senderState.GargantuanFeedback.ReliableStreamBytesRetransmitted == Retry &&
					!Connection->m_senderState.GargantuanFeedback.Invalid && Passed;
			}
		}
		GargantuanResumeNativeEgressForTest();
		if (Source) Api->CloseConnection(Source, 0, "control-only regression complete", false);
		if (Target) Api->CloseConnection(Target, 0, "control-only regression complete", false);
		GargantuanReliableServiceSnapshot Terminal{};
		if (Generation) Passed = GargantuanTakeNativeTerminalFeedback(Api, Source, Generation, Terminal) && Terminal.Counters.Purged && Passed;
		SteamNetworkingSockets_SetManualPollMode(false);
		std::fprintf(stderr, "[Network:NativeEgress] ControlOnlyPendingPacketCannotHoldDataCredit=%s\n", Passed ? "PASS" : "FAIL");
		return Passed;
	}
	static bool PendingDoesNotHoldReadyTurn(HSteamNetConnection Handle) {
		SteamNetworkingGlobalLock Global("PendingDataEligibilityFixture");
		ConnectionScopeLock Lock;
		auto *Connection = GetConnectionByHandle(Handle, Lock);
		if (!Connection || !Connection->GargantuanNativePendingTicket()) return false;
		const auto Now = SteamNetworkingSockets_GetLocalTimestamp() + 1000;
		auto &Protected = GargantuanProtectedWirePacer(1300);
		auto &Retained = GargantuanRetainedWirePacer(1300);
		(void)Protected.EligibleAt(Now, Handle); (void)Retained.EligibleAt(Now, Handle);
		std::uint64_t Cutoff = 0;
		SteamNetworkingMicroseconds Eligible = -1;
		const auto Class = Connection->GargantuanPlanData(Now, nullptr, Cutoff, &Eligible);
		const auto Other = Handle ^ 0x10000u;
		const bool Passed = Class == GargantuanDataClass::None && Eligible == 0 &&
			Protected.EligibleAt(Now, Other) <= Now && Retained.EligibleAt(Now, Other) <= Now;
		Protected.Forget(Other); Retained.Forget(Other);
		return Passed;
	}
	static bool SendPartial(HSteamNetConnection Handle) {
		SteamNetworkingGlobalLock Global("PartialNativeResultFixture");
		ConnectionScopeLock Lock;
		auto *Connection = GetConnectionByHandle(Handle, Lock);
		if (!Connection || !Connection->m_pTransport->SendDataPacket(SteamNetworkingSockets_GetLocalTimestamp())) return false;
		auto *Ticket = Connection->GargantuanNativePendingTicket();
		GargantuanNativeEgressResult Result{};
		if (!Ticket || !GargantuanWaitNativeEgressResult(Ticket, 100, Result) || Result.Bytes != 17 ||
			Result.Positive || !Connection->GargantuanReconcileNativeEgress()) return false;
		Connection->GargantuanDrainPendingReceived();
		return true;
	}
};
}

namespace {
using namespace SteamNetworkingSocketsLib;
using Clock = std::chrono::steady_clock;

ISteamNetworkingSockets *Interface = nullptr;
HSteamListenSocket Listener = k_HSteamListenSocket_Invalid;
HSteamNetConnection Client = k_HSteamNetConnection_Invalid;
HSteamNetConnection Server = k_HSteamNetConnection_Invalid;
bool CallbackFailed = false;

void Status(SteamNetConnectionStatusChangedCallback_t *Event) {
	if (Event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
		Event->m_info.m_hListenSocket == Listener) {
		Server = Event->m_hConn;
		if (Interface->AcceptConnection(Server) != k_EResultOK) CallbackFailed = true;
	}
	if (Event->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
		CallbackFailed = true;
}

template<class Predicate> bool Until(Predicate Ready, std::chrono::milliseconds Limit = std::chrono::milliseconds(2000)) {
	const auto Deadline = Clock::now() + Limit;
	while (Clock::now() < Deadline) {
		Interface->RunCallbacks();
		if (Ready()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

bool Connected(HSteamNetConnection Handle) {
	SteamNetConnectionInfo_t Info{};
	return Handle != k_HSteamNetConnection_Invalid && Interface->GetConnectionInfo(Handle, &Info) &&
		Info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

bool SendReliable(std::uint64_t ProtectedAcceptedAt = 0, unsigned ProtectedOrigin = 6) {
	std::array<std::uint8_t, 1200> Payload{};
	if (ProtectedAcceptedAt && !GargantuanBeginReliableSubmissionMetadata(false, 0, 0, 0,
		true, ProtectedAcceptedAt, ProtectedOrigin)) return false;
	const auto Result = Interface->SendMessageToConnection(Server, Payload.data(), static_cast<std::uint32_t>(Payload.size()),
		k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
	if (ProtectedAcceptedAt) GargantuanEndReliableRetirementAttribution();
	return Result == k_EResultOK;
}

bool ReceiveOne() {
	SteamNetworkingMessage_t *Message = nullptr;
	if (Interface->ReceiveMessagesOnConnection(Client, &Message, 1) <= 0) return false;
	if (Message) Message->Release();
	return true;
}

bool AwaitRetirement(std::uint64_t PriorPayload, GargantuanReliableServiceSnapshot &Final,
	bool *Delivered = nullptr) {
	return Until([&] {
		if (ReceiveOne() && Delivered) *Delivered = true;
		if (!GargantuanGetReliableServiceFeedback(Interface, static_cast<std::uint32_t>(Server), Final)) return false;
		return Final.Counters.ReliablePayloadBytesAcked >= PriorPayload + 1200 &&
			Final.Counters.UniqueReliableStreamBytesAcked == Final.Counters.UniqueReliableStreamBytesFirstSent;
	});
}

class ReadyCheckpointTimer final : public IThinker {
public:
	bool Called = false;
	bool Passed = false;
private:
	void Think(SteamNetworkingMicroseconds) override {
		Called = true;
		SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
		ConnectionScopeLock Lock;
		auto *Connection = GetConnectionByHandle(Server, Lock);
		auto *Ticket = Connection ? Connection->GargantuanNativePendingTicket() : nullptr;
		GargantuanResumeNativeEgressForTest();
		GargantuanNativeEgressResult First{};
		if (!Ticket || !GargantuanWaitNativeEgressResult(Ticket, 100, First) || !First.Positive)
			return;
		// Make the next packet eligible under the unchanged native token bucket.
		// The assertion below is event ordering, not a wall-clock latency gate.
		while (SteamNetworkingSockets_GetLocalTimestamp() <= First.CompletedAtGnsMicroseconds + 1000) {}
		GargantuanWakeReadyNativeEgressConnections();
		Ticket = Connection->GargantuanNativePendingTicket();
		GargantuanNativeEgressResult Next{};
		Passed = Ticket && GargantuanWaitNativeEgressResult(Ticket, 100, Next) &&
			Next.Positive && Next.DispatchSequence > First.DispatchSequence &&
			Next.CompletedAtMicroseconds >= First.CompletedAtMicroseconds;
	}
};

bool ReadyCheckpointDuringThinker(std::uint64_t PriorPayload) {
	// Freeze normal actor dispatch. A completion produced inside this already
	// executing callback falls after the thinker's fixed eligibility cutoff.
	// Merely scheduling ASAP cannot prepare the next packet before it returns.
	SteamNetworkingSockets_SetManualPollMode(true);
	bool Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::AfterOsReturn, 250,
		GargantuanNativeEgressTestTraffic::Any, 1000) && SendReliable() && SendReliable();
	bool Paused = false;
	const auto Deadline = Clock::now() + std::chrono::milliseconds(100);
	while (Passed && !Paused && Clock::now() < Deadline) {
		SteamNetworkingSockets_Poll(0);
		Paused = GargantuanWaitNativeEgressPauseForTest(1);
	}
	Passed = Passed && Paused;
	if (Passed) {
		SteamNetworkingGlobalLock GlobalLock("NativeEgressReadyCheckpointTest");
		ReadyCheckpointTimer Timer;
		Timer.SetNextThinkTimeASAP();
		while (SteamNetworkingSockets_GetLocalTimestamp() <= Timer.GetNextThinkTime()) {}
		IThinker::Thinker_ProcessThinkers();
		Passed = Timer.Called && Timer.Passed;
	}
	GargantuanResumeNativeEgressForTest();
	SteamNetworkingSockets_SetManualPollMode(false);
	GargantuanReliableServiceSnapshot Final{};
	const bool Retired = AwaitRetirement(PriorPayload + 1200, Final);
	Passed = Passed && Retired && Final.Counters.ReliablePayloadBytesAcked == PriorPayload + 2400 &&
		Final.Counters.ReliableStreamBytesRetransmitted == 0 && !Final.Counters.Invalid;
	std::fprintf(stderr, "[Network:NativeEgress] ReadyCheckpointDuringFixedCutoffThinker=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool Run() {
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) {
		std::fprintf(stderr, "[Network:NativeEgress] setup init failed: %s\n", Error);
		return false;
	}
	Interface = SteamNetworkingSockets();
	if (!Interface || !GargantuanConfigureNativeEgressWorkers(Interface, true, 1)) {
		std::fprintf(stderr, "[Network:NativeEgress] setup worker configuration failed\n");
		return false;
	}
	SteamNetworkingConfigValue_t Option{};
	Option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void *>(Status));
	SteamNetworkingIPAddr Address{};
	for (std::uint16_t Port = 39700; Port < 39732 && Listener == k_HSteamListenSocket_Invalid; ++Port) {
		Address.SetIPv4(0x7f000001, Port);
		Listener = Interface->CreateListenSocketIP(Address, 1, &Option);
	}
	if (Listener == k_HSteamListenSocket_Invalid) {
		std::fprintf(stderr, "[Network:NativeEgress] setup direct listener failed\n");
		return false;
	}
	Client = Interface->ConnectByIPAddress(Address, 1, &Option);
	if (!Until([&] { return Connected(Client) && Connected(Server); }) || CallbackFailed) {
		std::fprintf(stderr, "[Network:NativeEgress] setup direct connection failed client=%u server=%u callback=%d\n",
			Client, Server, CallbackFailed);
		return false;
	}
	std::uint64_t Generation = 0;
	if (!GargantuanConfigureNativeEgress(Interface, static_cast<std::uint32_t>(Server), true, &Generation, true) || !Generation) {
		std::fprintf(stderr, "[Network:NativeEgress] setup connection opt-in failed handle=%u generation=%llu reserved=%u\n",
			Server, static_cast<unsigned long long>(Generation), GargantuanNativeTerminalEvidenceReserved());
		return false;
	}
	const auto FirstGeneration = Generation;
	const bool OptOutReleased = GargantuanNativeTerminalEvidenceReserved() == 1 &&
		Until([&] { return GargantuanConfigureNativeEgress(
			Interface, static_cast<std::uint32_t>(Server), false); }) &&
		GargantuanNativeTerminalEvidenceReserved() == 0 &&
		GargantuanConfigureNativeEgress(Interface, static_cast<std::uint32_t>(Server), true, &Generation, true) &&
		Generation > FirstGeneration && GargantuanNativeTerminalEvidenceReserved() == 1;
	std::fprintf(stderr, "[Network:NativeEgress] OptOutGenerationReservation=%s\n",
		OptOutReleased ? "PASS" : "FAIL");
	if (!OptOutReleased) return false;

	// A positive OS send remains unpublished while the worker pauses after the
	// return.  The peer can receive and ACK during this pause, but the sender's
	// SNP parser must replay that datagram only after committing the packet.
	const auto ProtectedAccepted = GargantuanReliableServiceClock();
	if (!GargantuanArmNativeEgressPauseForTest(GargantuanNativeEgressTestPausePoint::AfterOsReturn, 250,
		GargantuanNativeEgressTestTraffic::Any, 1000) ||
		!SendReliable(ProtectedAccepted) || !GargantuanWaitNativeEgressPauseForTest(100)) {
		std::fprintf(stderr, "[Network:NativeEgress] first pending OS pause was not reached\n");
		return false;
	}
	const bool PendingTurn = GargantuanDataArbitrationTest::PendingDoesNotHoldReadyTurn(Server);
	std::fprintf(stderr, "[Network:NativeEgress] PendingPacketCannotHoldOtherPeersReadyTurn=%s\n", PendingTurn ? "PASS" : "FAIL");
	if (!PendingTurn) return false;
	GargantuanReliableServiceSnapshot Incomplete{};
	const bool Unavailable = !GargantuanGetReliableServiceFeedback(
		Interface, static_cast<std::uint32_t>(Server), Incomplete);
	const bool ReceivedBeforeCommit = Until([] { return ReceiveOne(); }, std::chrono::milliseconds(50));
	std::uint32_t JournalCount = 0, JournalHighWater = 0;
	bool JournalOverflow = false;
	const bool AckQueuedBeforeCommit = Until([&] {
		return GargantuanReadPendingNativeReceiveJournal(Interface, static_cast<std::uint32_t>(Server),
			JournalCount, JournalHighWater, JournalOverflow) && JournalCount > 0;
	}, std::chrono::milliseconds(75));
	GargantuanReliableServiceSnapshot Resolved{};
	const bool ResolutionTimedOut = !GargantuanReadResolvedNativeFeedback(Interface,
		static_cast<std::uint32_t>(Server), GargantuanReliableServiceClock() + 1000, Resolved);
	std::thread ReleaseResult([] {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		GargantuanResumeNativeEgressForTest();
	});
	const bool ResolutionCompleted = GargantuanReadResolvedNativeFeedback(Interface,
		static_cast<std::uint32_t>(Server), GargantuanReliableServiceClock() + 100000, Resolved);
	ReleaseResult.join();
	const bool ResolutionTruth = ResolutionTimedOut && ResolutionCompleted &&
		Resolved.Counters.ReliablePayloadBytesAcked <= 1200 &&
		Resolved.Counters.UniqueReliableStreamBytesFirstSent > 0 && !Resolved.Counters.Invalid;
	std::fprintf(stderr, "[Network:NativeEgress] ResolvedBaselineTimeoutAndNativeTruth=%s\n",
		ResolutionTruth ? "PASS" : "FAIL");
	if (!ResolutionTruth) std::fprintf(stderr, "[Network:NativeEgress] resolution detail timeout=%d completed=%d unique=%llu acked=%llu invalid=%d protected_invalid=%d\n",
		ResolutionTimedOut, ResolutionCompleted, static_cast<unsigned long long>(Resolved.Counters.UniqueReliableStreamBytesFirstSent),
		static_cast<unsigned long long>(Resolved.Counters.ReliablePayloadBytesAcked), Resolved.Counters.Invalid, Resolved.Counters.ProtectedOrdinaryEvidenceInvalid);
	if (!ResolutionTruth) return false;
	GargantuanReliableServiceSnapshot First{};
	bool ReceivedAfterCommit = false;
	const bool FirstRetired = AwaitRetirement(0, First, &ReceivedAfterCommit);
	const bool FirstExact = Unavailable && (ReceivedBeforeCommit || ReceivedAfterCommit) && AckQueuedBeforeCommit &&
		JournalHighWater > 0 && !JournalOverflow && FirstRetired &&
		First.Counters.ReliablePayloadBytesAcked == 1200 &&
		First.Counters.ReliableStreamBytesRetransmitted == 0 && !First.Counters.Invalid &&
		!First.Counters.ProtectedOrdinaryEvidenceInvalid && !First.Counters.QualifiedOrdinaryRegisteredMessages;
	std::fprintf(stderr, "[Network:NativeEgress] AckBeforeCommitAndFrontier=%s\n", FirstExact ? "PASS" : "FAIL");
	if (!FirstExact) std::fprintf(stderr,
		"[Network:NativeEgress] ack detail unavailable=%d receivedBefore=%d receivedAfter=%d journaled=%d count=%u high=%u overflow=%d retired=%d payloadAcked=%llu firstSent=%llu uniqueAcked=%llu retransmit=%llu invalid=%d\n",
		Unavailable, ReceivedBeforeCommit, ReceivedAfterCommit, AckQueuedBeforeCommit, JournalCount, JournalHighWater,
		JournalOverflow, FirstRetired,
		static_cast<unsigned long long>(First.Counters.ReliablePayloadBytesAcked),
		static_cast<unsigned long long>(First.Counters.UniqueReliableStreamBytesFirstSent),
		static_cast<unsigned long long>(First.Counters.UniqueReliableStreamBytesAcked),
		static_cast<unsigned long long>(First.Counters.ReliableStreamBytesRetransmitted), First.Counters.Invalid);
	if (!FirstExact) return false;

	GargantuanAckDiagnostics Trace{};
	if (!GargantuanAccessAckDiagnostics(Interface, static_cast<std::uint32_t>(Server), true, Trace) ||
		!GargantuanFailNextNativeEgressForTest() || !SendReliable(GargantuanReliableServiceClock())) {
		std::fprintf(stderr, "[Network:NativeEgress] negative result setup failed\n");
		return false;
	}
	const bool FailedOnce = Until([] { return GargantuanNativeEgressFailuresForTest() >= 1; });
	GargantuanReliableServiceSnapshot Second{};
	const bool SecondRetired = AwaitRetirement(First.Counters.ReliablePayloadBytesAcked, Second);
	const bool Traced = GargantuanAccessAckDiagnostics(Interface, static_cast<std::uint32_t>(Server), false, Trace);
	const bool RetryExact = FailedOnce && SecondRetired && Traced &&
		Trace.FailedPacketReferencesReleased > 0 &&
		Second.Counters.ReliablePayloadBytesAcked == 2400 &&
		Second.Counters.ReliableStreamBytesRetransmitted == 0 && !Second.Counters.Invalid &&
		!Second.Counters.ProtectedOrdinaryEvidenceInvalid;
	std::fprintf(stderr, "[Network:NativeEgress] NegativeFirstSendRetryReferences=%s\n", RetryExact ? "PASS" : "FAIL");
	if (!RetryExact) return false;
	if (!ReadyCheckpointDuringThinker(Second.Counters.ReliablePayloadBytesAcked)) return false;
	GargantuanReliableServiceSnapshot BeforeLate{};
	if (!GargantuanGetReliableServiceFeedback(Interface, static_cast<std::uint32_t>(Server), BeforeLate) ||
		!SendReliable(GargantuanReliableServiceClock() - 60'000, 5)) return false;
	GargantuanReliableServiceSnapshot Late{};
	const bool LateExact = AwaitRetirement(BeforeLate.Counters.ReliablePayloadBytesAcked, Late) &&
		Late.Counters.ProtectedOrdinaryEverFailed && !Late.Counters.ProtectedOrdinaryEvidenceInvalid &&
		!Late.Counters.QualifiedOrdinaryRegisteredMessages && !Late.Counters.Invalid;
	std::fprintf(stderr, "[Network:NativeEgress] ProtectedNoSinkWorkerCompletion=%s\n", LateExact ? "PASS" : "FAIL");
	if (!LateExact) return false;

	// Inject a reported partial OS result before any native packet is sent.
	// Qualify accounting/requeue, never pretend this is a measured partial UDP.
	SteamNetworkingSockets_SetManualPollMode(true);
	bool Partial = GargantuanFailNextNativeEgressForTest(17) && SendReliable(GargantuanReliableServiceClock());
	Partial = GargantuanDataArbitrationTest::SendPartial(Server) && Partial;
	GargantuanReliableServiceSnapshot PartialSnapshot{};
	Partial = GargantuanReadNativeFeedback(Interface, static_cast<std::uint32_t>(Server), PartialSnapshot) &&
		PartialSnapshot.Counters.Invalid &&
		PartialSnapshot.Counters.NativePacketBytesSent == Late.Counters.NativePacketBytesSent + 17 &&
		PartialSnapshot.Counters.UniqueReliableStreamBytesFirstSent == Late.Counters.UniqueReliableStreamBytesFirstSent && Partial;
	SteamNetworkingSockets_SetManualPollMode(false);
	GargantuanReliableServiceSnapshot AfterPartial{};
	bool PartialDelivered = false;
	Partial = Until([&] {
		PartialDelivered = ReceiveOne() || PartialDelivered;
		return GargantuanReadNativeFeedback(Interface, static_cast<std::uint32_t>(Server), AfterPartial) &&
			PartialDelivered && !AfterPartial.PendingReliableStreamBytes && !AfterPartial.SentUnackedReliableStreamBytes;
	}) &&
		AfterPartial.Counters.Invalid && // Fatal health never turns into a valid ACK certificate.
		AfterPartial.Counters.ReliableStreamBytesRetransmitted == Late.Counters.ReliableStreamBytesRetransmitted && Partial;
	std::fprintf(stderr, "[Network:NativeEgress] PartialPositiveDebitNoUniqueRetryOwnership=%s injected=1\n", Partial ? "PASS" : "FAIL");
	if (!Partial) return false;

	if (!GargantuanArmNativeEgressPauseForTest(GargantuanNativeEgressTestPausePoint::BeforeOsCall,
		250, GargantuanNativeEgressTestTraffic::Any, 1000) ||
		!SendReliable(GargantuanReliableServiceClock()) || !GargantuanWaitNativeEgressPauseForTest(100)) {
		std::fprintf(stderr, "[Network:NativeEgress] terminal pending OS pause was not reached\n");
		return false;
	}
	GargantuanReliableServiceSnapshot Closing{};
	const bool Closed = GargantuanCloseWithReliableServiceFeedback(Interface,
		static_cast<std::uint32_t>(Server), 0, "pending close regression", Closing);
	const bool ClosingIncomplete = Closed && Closing.Counters.Invalid && !Closing.Counters.Purged;
	GargantuanResumeNativeEgressForTest();
	const bool Drained = GargantuanDrainNativeConnection(Interface,
		static_cast<std::uint32_t>(Server), Generation, 250);
	GargantuanReliableServiceSnapshot Terminal{};
	const bool Took = GargantuanTakeNativeTerminalFeedback(Interface,
		static_cast<std::uint32_t>(Server), Generation, Terminal);
	GargantuanReliableServiceSnapshot Duplicate{};
	const bool DuplicateTook = GargantuanTakeNativeTerminalFeedback(Interface,
		static_cast<std::uint32_t>(Server), Generation, Duplicate);
	const bool ExactTerminal = ClosingIncomplete && Took && Terminal.Counters.Purged &&
		!DuplicateTook && !Terminal.Counters.ProtectedOrdinaryEvidenceInvalid;
	std::fprintf(stderr, "[Network:NativeEgress] PendingCloseTerminalGeneration=%s\n",
		ExactTerminal ? "PASS" : "FAIL");
	if (!ExactTerminal) std::fprintf(stderr,
		"[Network:NativeEgress] terminal detail closed=%d closingInvalid=%d closingPurged=%d drained=%d took=%d terminalInvalid=%d terminalPurged=%d duplicate=%d reserved=%u\n",
		Closed, Closing.Counters.Invalid, Closing.Counters.Purged, Drained, Took,
		Terminal.Counters.Invalid, Terminal.Counters.Purged, DuplicateTook,
		GargantuanNativeTerminalEvidenceReserved());
	return ExactTerminal && GargantuanDataArbitrationTest::ControlOnlyDoesNotHoldDataCredit();
}
}

int main() {
	const bool Passed = Run();
	if (Interface) {
		if (Client != k_HSteamNetConnection_Invalid) Interface->CloseConnection(Client, 0, "test cleanup", false);
		if (Server != k_HSteamNetConnection_Invalid) Interface->CloseConnection(Server, 0, "test cleanup", false);
		if (Listener != k_HSteamListenSocket_Invalid) Interface->CloseListenSocket(Listener);
		const bool Drained = GargantuanDrainNativeEgress(Interface, 250);
		const bool Stopped = GargantuanConfigureNativeEgressWorkers(Interface, false, 0);
		GameNetworkingSockets_Kill();
		return Passed && Drained && Stopped ? 0 : 1;
	}
	return 1;
}
