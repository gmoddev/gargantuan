#include <steam/steamnetworkingsockets.h>
#include <clientlib/steamnetworkingsockets_lowlevel.h>
#include "NativeEgressWorker.hpp"

#include <array>
#include <cstdio>

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode(bool);

namespace {
using namespace SteamNetworkingSocketsLib;

constexpr int SocketCount = 66;
constexpr int TicketCount = 64;

bool QueueByte(IBoundUDPSocket *Socket, bool Protected,
	GargantuanNativeEgressTicket *&Ticket) {
	char Byte = Protected ? 'P' : 'D';
	iovec Chunk{};
	Chunk.iov_base = &Byte;
	Chunk.iov_len = 1;
	return GargantuanQueueNativeEgress(Socket, 1, &Chunk, 1, Protected, Ticket) ==
		GargantuanNativeEgressQueueStatus::Queued;
}

bool WaitResult(GargantuanNativeEgressTicket *Ticket,
	GargantuanNativeEgressResult &Result) {
	if (GargantuanWaitNativeEgressResult(Ticket, 1000, Result)) return true;
	GargantuanFailStopNativeEgress("native worker regression timed out");
}

bool OpenPairs(std::array<IBoundUDPSocket *, SocketCount> &Sockets) {
	SteamNetworkingErrMsg Error{};
	for (int Index = 0; Index < SocketCount; Index += 2) {
		if (!CreateBoundSocketPair({}, {}, &Sockets[Index], Error)) {
			std::fprintf(stderr, "[Network:NativeEgress] socket pair %d failed: %s\n", Index / 2, Error);
			return false;
		}
	}
	return true;
}

void ClosePairs(std::array<IBoundUDPSocket *, SocketCount> &Sockets) {
	for (auto *&Socket : Sockets) {
		if (Socket) Socket->Close();
		Socket = nullptr;
	}
}

bool QueueReserveAndPriority() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	std::array<GargantuanNativeEgressTicket *, TicketCount> Tickets{};
	std::array<GargantuanNativeEgressResult, TicketCount> Results{};
	bool Passed = OpenPairs(Sockets);
	const auto GnsBefore = SteamNetworkingSockets_GetLocalTimestamp();
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250);
	if (Passed) Passed = QueueByte(Sockets[0], false, Tickets[0]);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	GargantuanNativeEgressTicket *Duplicate = nullptr;
	if (Passed) {
		char Byte = 'X';
		iovec Chunk{};
		Chunk.iov_base = &Byte;
		Chunk.iov_len = 1;
		Passed = GargantuanQueueNativeEgress(Sockets[0], 1, &Chunk, 1, true, Duplicate) ==
			GargantuanNativeEgressQueueStatus::Rejected && Duplicate == nullptr;
	}
	for (int Index = 1; Passed && Index < 56; ++Index)
		Passed = QueueByte(Sockets[Index], false, Tickets[Index]);
	if (Passed) {
		char Byte = 'D';
		iovec Chunk{};
		Chunk.iov_base = &Byte;
		Chunk.iov_len = 1;
		Passed = GargantuanQueueNativeEgress(Sockets[56], 1, &Chunk, 1, false, Duplicate) ==
			GargantuanNativeEgressQueueStatus::Rejected && Duplicate == nullptr;
	}
	for (int Index = 56; Passed && Index < TicketCount; ++Index)
		Passed = QueueByte(Sockets[Index], true, Tickets[Index]);
	if (Passed) {
		char Byte = 'P';
		iovec Chunk{};
		Chunk.iov_base = &Byte;
		Chunk.iov_len = 1;
		Passed = GargantuanQueueNativeEgress(Sockets[64], 1, &Chunk, 1, true, Duplicate) ==
			GargantuanNativeEgressQueueStatus::Rejected && Duplicate == nullptr;
	}
	GargantuanResumeNativeEgressForTest();
	for (int Index = 0; Index < TicketCount; ++Index) {
		if (!Tickets[Index]) continue;
		Passed = WaitResult(Tickets[Index], Results[Index]) && Passed;
	}
	const auto GnsAfter = SteamNetworkingSockets_GetLocalTimestamp();
	if (Passed) {
		for (const auto &Result : Results)
			Passed = Result.Positive && Result.Bytes == 1 &&
				Result.CompletedAtMicroseconds > 0 &&
				Result.CompletedAtGnsMicroseconds >= GnsBefore &&
				Result.CompletedAtGnsMicroseconds <= GnsAfter && Passed;
		for (int ProtectedIndex = 56; ProtectedIndex < TicketCount; ++ProtectedIndex)
			for (int OrdinaryIndex = 1; OrdinaryIndex < 56; ++OrdinaryIndex)
				Passed = Results[ProtectedIndex].DispatchSequence <
					Results[OrdinaryIndex].DispatchSequence && Passed;
		Passed = Results[0].DispatchSequence < Results[56].DispatchSequence && Passed;
	}
	for (auto *Ticket : Tickets)
		if (Ticket) GargantuanReleaseNativeEgress(Ticket);
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] SlotReservePriorityPeerFifo=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool ProtectedBurstFairness() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	std::array<GargantuanNativeEgressTicket *, 11> Tickets{};
	std::array<GargantuanNativeEgressResult, 11> Results{};
	bool Passed = OpenPairs(Sockets);
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250);
	if (Passed) Passed = QueueByte(Sockets[0], false, Tickets[0]);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	if (Passed) Passed = QueueByte(Sockets[1], false, Tickets[1]);
	for (int Index = 2; Passed && Index < 11; ++Index)
		Passed = QueueByte(Sockets[Index], true, Tickets[Index]);
	GargantuanResumeNativeEgressForTest();
	for (int Index = 0; Index < 11; ++Index)
		if (Tickets[Index]) Passed = WaitResult(Tickets[Index], Results[Index]) && Passed;
	if (Passed) {
		Passed = Results[0].DispatchSequence < Results[2].DispatchSequence &&
			Results[9].DispatchSequence < Results[1].DispatchSequence &&
			Results[1].DispatchSequence < Results[10].DispatchSequence;
		for (int Index = 2; Index < 11; ++Index)
			Passed = Results[Index].Positive && Passed;
	}
	for (auto *Ticket : Tickets)
		if (Ticket) GargantuanReleaseNativeEgress(Ticket);
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] ProtectedBurstFairness=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool PauseTrafficFilter() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	bool Passed = OpenPairs(Sockets);
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250,
		GargantuanNativeEgressTestTraffic::Ordinary);
	GargantuanNativeEgressTicket *ProtectedTicket = nullptr;
	GargantuanNativeEgressTicket *OrdinaryTicket = nullptr;
	if (Passed) Passed = QueueByte(Sockets[0], true, ProtectedTicket);
	if (Passed) Passed = QueueByte(Sockets[1], false, OrdinaryTicket);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	GargantuanNativeEgressResult ProtectedResult{}, OrdinaryResult{};
	if (Passed) Passed = GargantuanReadNativeEgressResult(ProtectedTicket, ProtectedResult) &&
		ProtectedResult.Positive &&
		!GargantuanReadNativeEgressResult(OrdinaryTicket, OrdinaryResult);
	GargantuanResumeNativeEgressForTest();
	if (OrdinaryTicket) Passed = WaitResult(OrdinaryTicket, OrdinaryResult) && Passed;
	if (OrdinaryTicket) Passed = OrdinaryResult.Positive && Passed;
	if (ProtectedTicket) GargantuanReleaseNativeEgress(ProtectedTicket);
	if (OrdinaryTicket) GargantuanReleaseNativeEgress(OrdinaryTicket);
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250,
		GargantuanNativeEgressTestTraffic::Any, 1000);
	GargantuanNativeEgressTicket *SmallTicket = nullptr;
	if (Passed) Passed = QueueByte(Sockets[2], false, SmallTicket);
	GargantuanNativeEgressResult SmallResult{};
	if (SmallTicket) Passed = WaitResult(SmallTicket, SmallResult) &&
		SmallResult.Positive && !GargantuanWaitNativeEgressPauseForTest(1) && Passed;
	if (SmallTicket) GargantuanReleaseNativeEgress(SmallTicket);
	char LargeDatagram[1000]{};
	iovec LargeChunk{};
	LargeChunk.iov_base = LargeDatagram;
	LargeChunk.iov_len = sizeof(LargeDatagram);
	GargantuanNativeEgressTicket *LargeTicket = nullptr;
	if (Passed) Passed = GargantuanQueueNativeEgress(Sockets[3], 1, &LargeChunk,
		sizeof(LargeDatagram), true, LargeTicket) == GargantuanNativeEgressQueueStatus::Queued;
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	GargantuanResumeNativeEgressForTest();
	GargantuanNativeEgressResult LargeResult{};
	if (LargeTicket) Passed = WaitResult(LargeTicket, LargeResult) && LargeResult.Positive && Passed;
	if (LargeTicket) GargantuanReleaseNativeEgress(LargeTicket);
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] PauseTrafficFilter=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool QueuedCancellation() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	bool Passed = OpenPairs(Sockets);
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250);
	GargantuanNativeEgressTicket *Executing = nullptr;
	GargantuanNativeEgressTicket *Queued = nullptr;
	if (Passed) Passed = QueueByte(Sockets[0], false, Executing);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	if (Passed) Passed = QueueByte(Sockets[1], false, Queued);
	if (Passed) Passed = GargantuanCancelUnstartedNativeEgress(Queued);
	GargantuanNativeEgressResult CanceledResult{};
	if (Passed) Passed = GargantuanReadNativeEgressResult(Queued, CanceledResult) &&
		!CanceledResult.Positive && CanceledResult.Error != 0 &&
		CanceledResult.CompletedAtMicroseconds == 0 &&
		CanceledResult.CompletedAtGnsMicroseconds == 0;
	if (Queued) GargantuanReleaseNativeEgress(Queued);
	GargantuanResumeNativeEgressForTest();
	if (Executing) {
		GargantuanNativeEgressResult ExecutedResult{};
		Passed = WaitResult(Executing, ExecutedResult) && Passed;
		Passed = ExecutedResult.Positive && Passed;
		GargantuanReleaseNativeEgress(Executing);
	}
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] QueuedCancellation=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool PauseAndSocketLease() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	bool Passed = OpenPairs(Sockets);
	GargantuanNativeEgressTicket *Ticket = nullptr;
	GargantuanNativeEgressResult Result{};
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::BeforeOsCall, 250);
	if (Passed) Passed = QueueByte(Sockets[0], false, Ticket);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	if (Sockets[0]) {
		Sockets[0]->Close();
		Sockets[0] = nullptr;
	}
	GargantuanResumeNativeEgressForTest();
	if (Ticket) {
		Passed = WaitResult(Ticket, Result) && Passed;
		Passed = !Result.Positive && Result.Bytes == 0 &&
			Result.CompletedAtMicroseconds == 0 && Result.CompletedAtGnsMicroseconds == 0 &&
			Result.DispatchSequence != 0 &&
			Result.SocketGeneration != 0 && Passed;
		GargantuanReleaseNativeEgress(Ticket);
	}
	ClosePairs(Sockets);

	Sockets = {};
	Passed = Passed && OpenPairs(Sockets);
	Ticket = nullptr;
	if (Passed) Passed = GargantuanArmNativeEgressPauseForTest(
		GargantuanNativeEgressTestPausePoint::AfterOsReturn, 250);
	if (Passed) Passed = QueueByte(Sockets[0], true, Ticket);
	if (Passed) Passed = GargantuanWaitNativeEgressPauseForTest(100);
	GargantuanNativeEgressResult NewResult{};
	if (Passed) Passed = !GargantuanReadNativeEgressResult(Ticket, NewResult);
	if (Sockets[0]) {
		Sockets[0]->Close();
		Sockets[0] = nullptr;
	}
	GargantuanResumeNativeEgressForTest();
	if (Ticket) {
		Passed = WaitResult(Ticket, NewResult) && Passed;
		Passed = NewResult.Positive && NewResult.Bytes == 1 &&
			NewResult.SocketGeneration > Result.SocketGeneration &&
			NewResult.CompletedAtGnsMicroseconds > 0 &&
			NewResult.CompletedAtMicroseconds >= NewResult.StartedAtMicroseconds && Passed;
		GargantuanReleaseNativeEgress(Ticket);
	}
	ClosePairs(Sockets);
	Sockets = {};
	Passed = Passed && OpenPairs(Sockets);
	Ticket = nullptr;
	if (Passed) Passed = QueueByte(Sockets[0], false, Ticket);
	if (Ticket) {
		GargantuanNativeEgressResult ReopenedResult{};
		Passed = WaitResult(Ticket, ReopenedResult) && Passed;
		Passed = ReopenedResult.Positive &&
			ReopenedResult.SocketGeneration > NewResult.SocketGeneration && Passed;
		GargantuanReleaseNativeEgress(Ticket);
	}
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] BeforeAfterOsAndCloseLease=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}

bool NegativeNativeResult() {
	std::array<IBoundUDPSocket *, SocketCount> Sockets{};
	bool Passed = OpenPairs(Sockets);
	const auto Before = GargantuanNativeEgressFailuresForTest();
	if (Passed) Passed = GargantuanFailNextNativeEgressForTest();
	GargantuanNativeEgressTicket *Ticket = nullptr;
	if (Passed) Passed = QueueByte(Sockets[0], false, Ticket);
	if (Ticket) {
		GargantuanNativeEgressResult Result{};
		Passed = WaitResult(Ticket, Result) && Passed;
		Passed = !Result.Positive && Result.Bytes == 0 && Result.Error != 0 &&
			Result.CompletedAtMicroseconds == 0 && Result.CompletedAtGnsMicroseconds == 0 &&
			GargantuanNativeEgressFailuresForTest() == Before + 1 && Passed;
		GargantuanReleaseNativeEgress(Ticket);
	}
	ClosePairs(Sockets);
	std::fprintf(stderr, "[Network:NativeEgress] NegativeNativeResult=%s\n",
		Passed ? "PASS" : "FAIL");
	return Passed;
}
}

int main() {
	SteamNetworkingSockets_SetManualPollMode(true);
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) {
		std::fprintf(stderr, "[Network:NativeEgress] initialization failed: %s\n", Error);
		return 1;
	}
	bool Passed;
	{
		SteamNetworkingGlobalLock Lock("GnsNativeEgressWorkerTests");
		Passed = GargantuanConfigureNativeEgress(true, 1);
		Passed = Passed && QueueReserveAndPriority();
		Passed = Passed && ProtectedBurstFairness();
		Passed = Passed && PauseTrafficFilter();
		Passed = Passed && QueuedCancellation();
		Passed = Passed && PauseAndSocketLease();
		Passed = Passed && NegativeNativeResult();
		const bool Stopped = GargantuanConfigureNativeEgress(false, 0);
		Passed = Passed && Stopped;
	}
	GameNetworkingSockets_Kill();
	SteamNetworkingSockets_SetManualPollMode(false);
	return Passed ? 0 : 1;
}
