// Included after the raw socket class and service-thread wake primitive.
// This is an opt-in Windows transport implementation, not a second protocol
// actor. Every operation here is bounded by the fixed slot and worker arrays.
namespace {

constexpr unsigned GargantuanEgressSlotCount = 64;
constexpr unsigned GargantuanEgressProtectedReserve = 8;
constexpr unsigned GargantuanEgressProtectedBurst = 8;
constexpr unsigned GargantuanEgressMaximumWorkers = 4;
enum class GargantuanTicketState : unsigned { Free, Queued, Executing, Ready };
#ifdef _WIN32
constexpr int GargantuanCancelledError = WSA_OPERATION_ABORTED;
#else
constexpr int GargantuanCancelledError = -1;
#endif

} // namespace

struct GargantuanNativeEgressTicket {
	std::atomic<GargantuanTicketState> State{GargantuanTicketState::Free};
	CRawUDPSocketImpl *Socket = nullptr;
	const IBoundUDPSocket *BoundIdentity = nullptr; // compared only; never dereferenced by workers
	std::uint64_t SocketGeneration = 0;
	std::uint64_t QueueSequence = 0;
	std::uint64_t QueuedAtMicroseconds = 0;
	std::uint8_t Datagram[k_cbSteamNetworkingSocketsMaxUDPMsgLen]{};
	int DatagramBytes = 0;
	sockaddr_storage Destination{};
	socklen_t DestinationBytes = 0;
	int Ecn = 0;
	bool Protected = false;
	bool OsStarted = false; // guarded by the worker domain mutex
	bool CancelRequested = false; // guarded by the worker domain mutex
	GargantuanNativeEgressResult Result{};
};

namespace {

struct GargantuanEgressDomain {
	std::mutex Mutex;
	std::condition_variable Changed;
	std::array<GargantuanNativeEgressTicket, GargantuanEgressSlotCount> Slots;
	std::array<std::thread, GargantuanEgressMaximumWorkers> Workers;
	unsigned WorkerCount = 0;
	bool Enabled = false;
	bool Stopping = false;
	std::uint64_t NextQueueSequence = 1;
	std::uint64_t NextDispatchSequence = 1;
	unsigned ProtectedDispatchStreak = 0;
	std::uint64_t NextSocketGeneration = 1;
	#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
		bool TestPauseArmed = false;
		bool TestPaused = false;
		bool TestReleased = false;
		GargantuanNativeEgressTestPausePoint TestPausePoint =
			GargantuanNativeEgressTestPausePoint::BeforeOsCall;
		GargantuanNativeEgressTestTraffic TestTraffic =
			GargantuanNativeEgressTestTraffic::Any;
		int TestMinimumDatagramBytes = 0;
		std::uint32_t TestMaximumMilliseconds = 0;
		bool TestFailNextArmed = false;
		std::uint64_t TestFailedCount = 0;
	#endif
};

GargantuanEgressDomain &GargantuanEgress() {
	// Process lifetime storage prevents a static destructor from racing a hung
	// OS call. Normal shutdown joins every worker after all tickets retire.
	static auto *Domain = new GargantuanEgressDomain;
	return *Domain;
}

bool GargantuanSimulationDisabled() {
	return GlobalConfig::FakeRateLimit_Send_Rate.Get() <= 0 &&
		GlobalConfig::FakePacketLoss_Send.Get() <= 0 &&
		GlobalConfig::FakePacketLag_Send.Get() <= 0 &&
		GlobalConfig::FakePacketReorder_Send.Get() <= 0 &&
		GlobalConfig::FakePacketJitter_Send_Avg.Get() <= 0 &&
		GlobalConfig::FakePacketDup_Send.Get() <= 0 &&
		GlobalConfig::PacketTraceMaxBytes.Get() < 0;
}

GargantuanNativeEgressTicket *GargantuanNextQueued(GargantuanEgressDomain &Domain) {
	GargantuanNativeEgressTicket *OldestOrdinary = nullptr;
	GargantuanNativeEgressTicket *OldestProtected = nullptr;
	for (auto &Slot : Domain.Slots) {
		if (Slot.State.load(std::memory_order_relaxed) != GargantuanTicketState::Queued)
			continue;
		auto *&Oldest = Slot.Protected ? OldestProtected : OldestOrdinary;
		if (!Oldest || Slot.QueueSequence < Oldest->QueueSequence) Oldest = &Slot;
	}
	// Protected traffic gets a prompt turn, but a sustained ACK/control stream
	// cannot leave already queued ordinary DATA without a finite dispatch turn.
	if (OldestProtected && (!OldestOrdinary ||
		Domain.ProtectedDispatchStreak < GargantuanEgressProtectedBurst))
		return OldestProtected;
	return OldestOrdinary;
}

#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
void GargantuanPauseAtTestPoint(GargantuanNativeEgressTestPausePoint Point,
	bool Protected, int DatagramBytes) {
	auto &Domain = GargantuanEgress();
	std::unique_lock Lock(Domain.Mutex);
	if (!Domain.TestPauseArmed || Domain.TestPausePoint != Point ||
		DatagramBytes < Domain.TestMinimumDatagramBytes ||
		(Domain.TestTraffic == GargantuanNativeEgressTestTraffic::Ordinary && Protected) ||
		(Domain.TestTraffic == GargantuanNativeEgressTestTraffic::Protected && !Protected)) return;
	Domain.TestPauseArmed = false;
	Domain.TestPaused = true;
	Domain.Changed.notify_all();
	Domain.Changed.wait_for(Lock, std::chrono::milliseconds(Domain.TestMaximumMilliseconds),
		[&] { return Domain.TestReleased || Domain.Stopping; });
	Domain.TestPaused = false;
	Domain.TestReleased = false;
	Domain.Changed.notify_all();
}
#endif

void GargantuanEgressWorkerMain() {
	auto &Domain = GargantuanEgress();
	for (;;) {
		GargantuanNativeEgressTicket *Ticket;
		std::uint64_t DispatchSequence;
		{
			std::unique_lock Lock(Domain.Mutex);
			Domain.Changed.wait(Lock, [&] { return Domain.Stopping || GargantuanNextQueued(Domain); });
			Ticket = GargantuanNextQueued(Domain);
			if (!Ticket) {
				if (Domain.Stopping) return;
				continue;
			}
			Ticket->State.store(GargantuanTicketState::Executing, std::memory_order_release);
			DispatchSequence = Domain.NextDispatchSequence++;
			Domain.ProtectedDispatchStreak = Ticket->Protected ?
				std::min(Domain.ProtectedDispatchStreak + 1, GargantuanEgressProtectedBurst) : 0;
		}

		GargantuanNativeEgressResult Result{};
		Result.DispatchSequence = DispatchSequence;
		Result.SocketGeneration = Ticket->SocketGeneration;
		Result.QueuedAtMicroseconds = Ticket->QueuedAtMicroseconds;
		#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
			GargantuanPauseAtTestPoint(GargantuanNativeEgressTestPausePoint::BeforeOsCall,
				Ticket->Protected, Ticket->DatagramBytes);
		#endif
		{
			std::lock_guard Lock(Domain.Mutex);
			if (Ticket->CancelRequested || !Domain.Enabled ||
				Ticket->Socket->m_GargantuanEgressClosing) {
				Result.Error = GargantuanCancelledError;
				Ticket->Result = Result;
				Ticket->State.store(GargantuanTicketState::Ready, std::memory_order_release);
				Domain.Changed.notify_all();
				WakeServiceThread();
				continue;
			}
			#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
				if (Domain.TestFailNextArmed) {
					Domain.TestFailNextArmed = false;
					++Domain.TestFailedCount;
					#ifdef _WIN32
						Result.Error = WSAENOBUFS;
					#else
						Result.Error = -2;
					#endif
					Ticket->Result = Result;
					Ticket->State.store(GargantuanTicketState::Ready, std::memory_order_release);
					Domain.Changed.notify_all();
					WakeServiceThread();
					continue;
				}
			#endif
			Ticket->OsStarted = true;
		}
		Result.StartedAtMicroseconds = GargantuanReliableServiceClock();
		#ifdef _WIN32
			WSABUF Buffer{static_cast<ULONG>(Ticket->DatagramBytes),
				reinterpret_cast<char *>(Ticket->Datagram)};
			WSAMSG Message{};
			Message.name = reinterpret_cast<sockaddr *>(&Ticket->Destination);
			Message.namelen = Ticket->DestinationBytes;
			Message.dwBufferCount = 1;
			Message.lpBuffers = &Buffer;
			CHAR Control[WSA_CMSG_SPACE(sizeof(INT))]{};
			if (Ticket->Ecn > 0) {
				Message.Control.len = sizeof(Control);
				Message.Control.buf = Control;
				CMSGHDR *Header = WSA_CMSG_FIRSTHDR(&Message);
				Header->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
				Header->cmsg_level = Ticket->Destination.ss_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
				Header->cmsg_type = Ticket->Destination.ss_family == AF_INET ? IP_ECN : IPV6_ECN;
				*reinterpret_cast<PINT>(WSA_CMSG_DATA(Header)) = Ticket->Ecn & 3;
			}
			DWORD BytesSent = 0;
			const int NativeReturn = WSASendMsg(Ticket->Socket->m_socket, &Message, 0,
				&BytesSent, nullptr, nullptr);
			Result.Error = NativeReturn == 0 ? 0 : GetLastSocketError();
			Result.Positive = NativeReturn == 0 && BytesSent == static_cast<DWORD>(Ticket->DatagramBytes);
			Result.Bytes = Result.Positive ? Ticket->DatagramBytes : 0;
		#else
			Result.Error = -1;
		#endif
		Result.CompletedAtMicroseconds = GargantuanReliableServiceClock();
		Result.CompletedAtGnsMicroseconds = SteamNetworkingSockets_GetLocalTimestamp();
		#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
			GargantuanPauseAtTestPoint(GargantuanNativeEgressTestPausePoint::AfterOsReturn,
				Ticket->Protected, Ticket->DatagramBytes);
		#endif
		Ticket->Result = Result;
		Ticket->State.store(GargantuanTicketState::Ready, std::memory_order_release);
		Domain.Changed.notify_all();
		WakeServiceThread();
	}
}

bool GargantuanIsOwnedRawSocket(const IRawUDPSocket *Socket) {
	for (const auto *Candidate : s_vecRawSockets)
		if (Candidate == Socket) return true;
	return false;
}

void GargantuanFenceSocketNativeEgress(CRawUDPSocketImpl *Socket) {
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	Socket->m_GargantuanEgressClosing = true;
	for (auto &Ticket : Domain.Slots) {
		if (Ticket.Socket != Socket) continue;
		const auto State = Ticket.State.load(std::memory_order_relaxed);
		if (State == GargantuanTicketState::Queued) {
			Ticket.Result = {false, 0, GargantuanCancelledError, 0, 0, Ticket.SocketGeneration};
			Ticket.State.store(GargantuanTicketState::Ready, std::memory_order_release);
		} else if (State == GargantuanTicketState::Executing && !Ticket.OsStarted) {
			Ticket.CancelRequested = true;
		}
	}
	Domain.Changed.notify_all();
}

} // namespace

bool GargantuanConfigureNativeEgress(bool Enabled, unsigned Workers) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	auto &Domain = GargantuanEgress();
	#ifdef _WIN32
		if (Enabled) {
			if (Workers == 0 || Workers > GargantuanEgressMaximumWorkers) return false;
			std::lock_guard Lock(Domain.Mutex);
			if (Domain.Enabled) return Domain.WorkerCount == Workers;
			if (Domain.WorkerCount || Domain.Stopping) return false;
			Domain.Enabled = true;
			Domain.ProtectedDispatchStreak = 0;
			try {
				for (unsigned Index = 0; Index < Workers; ++Index)
					Domain.Workers[Index] = std::thread(GargantuanEgressWorkerMain);
				Domain.WorkerCount = Workers;
				return true;
			} catch (...) {
				Domain.Enabled = false;
				Domain.Stopping = true;
				#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
					Domain.TestFailNextArmed = false;
				#endif
				Domain.Changed.notify_all();
			}
		} else {
			{
				std::lock_guard Lock(Domain.Mutex);
				for (const auto &Ticket : Domain.Slots)
					if (Ticket.State.load(std::memory_order_relaxed) != GargantuanTicketState::Free)
						return false;
				Domain.Enabled = false;
				Domain.Stopping = true;
				Domain.Changed.notify_all();
			}
		}
		// Workers touch no GNS or adapter lock, so joining under those locks is
		// safe after every ticket has been reconciled and released.
		for (auto &Worker : Domain.Workers)
			if (Worker.joinable()) Worker.join();
		Domain.WorkerCount = 0;
		Domain.Stopping = false;
		return !Enabled;
	#else
		(void)Enabled;
		(void)Workers;
		return !Enabled;
	#endif
}

bool GargantuanCanQueueNativeEgress() noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	#ifdef _WIN32
		auto &Domain = GargantuanEgress();
		std::lock_guard Lock(Domain.Mutex);
		if (!Domain.Enabled || !GargantuanSimulationDisabled()) return false;
		for (const auto &Ticket : Domain.Slots)
			if (Ticket.State.load(std::memory_order_relaxed) == GargantuanTicketState::Free)
				return true;
	#endif
	return false;
}

void GargantuanFenceNativeEgress() noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	Domain.Enabled = false;
	#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
		Domain.TestFailNextArmed = false;
	#endif
	for (auto &Ticket : Domain.Slots) {
		const auto State = Ticket.State.load(std::memory_order_relaxed);
		if (State == GargantuanTicketState::Queued) {
			Ticket.Result = {false, 0, GargantuanCancelledError, 0, 0, Ticket.SocketGeneration};
			Ticket.State.store(GargantuanTicketState::Ready, std::memory_order_release);
		} else if (State == GargantuanTicketState::Executing && !Ticket.OsStarted) {
			Ticket.CancelRequested = true;
		}
	}
	Domain.Changed.notify_all();
}

bool GargantuanWaitNativeEgressIdle(std::uint32_t TimeoutMilliseconds) noexcept {
	auto &Domain = GargantuanEgress();
	std::unique_lock Lock(Domain.Mutex);
	return Domain.Changed.wait_for(Lock, std::chrono::milliseconds(TimeoutMilliseconds), [&] {
		for (const auto &Ticket : Domain.Slots)
			if (Ticket.State.load(std::memory_order_relaxed) == GargantuanTicketState::Executing)
				return false;
		return true;
	});
}

GargantuanNativeEgressQueueStatus GargantuanQueueNativeEgress(
	IBoundUDPSocket *BoundSocket, int ChunkCount, const iovec *Chunks,
	int TotalBytes, bool Protected, GargantuanNativeEgressTicket *&Ticket
) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	Ticket = nullptr;
	#ifdef _WIN32
		auto &Domain = GargantuanEgress();
		if (!BoundSocket || !Chunks || ChunkCount <= 0 || ChunkCount > 2 ||
			TotalBytes <= 0 || TotalBytes > k_cbSteamNetworkingSocketsMaxUDPMsgLen)
			return GargantuanNativeEgressQueueStatus::Rejected;
		auto *Raw = BoundSocket->GetRawSock();
		if (!Raw || !GargantuanIsOwnedRawSocket(Raw))
			return GargantuanNativeEgressQueueStatus::Disabled;
		auto *Socket = static_cast<CRawUDPSocketImpl *>(Raw);
		if (Socket->m_GargantuanEgressClosing || Socket->m_socket == INVALID_SOCKET ||
			!GargantuanSimulationDisabled())
			return GargantuanNativeEgressQueueStatus::Rejected;
		std::lock_guard Lock(Domain.Mutex);
		if (!Domain.Enabled || Domain.NextQueueSequence == 0 ||
			Domain.NextSocketGeneration == 0)
			return GargantuanNativeEgressQueueStatus::Rejected;
		for (const auto &Existing : Domain.Slots)
			if (Existing.State.load(std::memory_order_relaxed) != GargantuanTicketState::Free &&
				Existing.BoundIdentity == BoundSocket)
				return GargantuanNativeEgressQueueStatus::Rejected;
		// DATA cannot consume the last protected admission slots. The slots are
		// pre-reserved result records, so ACK/control/retry never needs to wait
		// for a DATA result to be reconciled on another connection.
		if (!Protected) {
			unsigned Occupied = 0;
			for (const auto &Existing : Domain.Slots)
				Occupied += Existing.State.load(std::memory_order_relaxed) != GargantuanTicketState::Free;
			if (Occupied >= GargantuanEgressSlotCount - GargantuanEgressProtectedReserve)
				return GargantuanNativeEgressQueueStatus::Rejected;
		}
		for (auto &Candidate : Domain.Slots) {
			if (Candidate.State.load(std::memory_order_relaxed) == GargantuanTicketState::Free) {
				Ticket = &Candidate;
				break;
			}
		}
		if (!Ticket) return GargantuanNativeEgressQueueStatus::Rejected;
		int Copied = 0;
		for (int Index = 0; Index < ChunkCount; ++Index) {
			if (!Chunks[Index].iov_base || Chunks[Index].iov_len >
				static_cast<size_t>(TotalBytes - Copied)) {
				Ticket = nullptr;
				return GargantuanNativeEgressQueueStatus::Rejected;
			}
			memcpy(Ticket->Datagram + Copied, Chunks[Index].iov_base, Chunks[Index].iov_len);
			Copied += static_cast<int>(Chunks[Index].iov_len);
		}
		if (Copied != TotalBytes) {
			Ticket = nullptr;
			return GargantuanNativeEgressQueueStatus::Rejected;
		}
		const netadr_t &Remote = BoundSocket->GetRemoteHostAddr();
		if (Socket->m_nAddressFamilies & k_nAddressFamily_IPv6) {
			#ifndef PLATFORM_NO_IPV6
				Remote.ToSockadrIPV6(&Ticket->Destination);
				Ticket->DestinationBytes = sizeof(sockaddr_in6);
			#else
				Ticket = nullptr;
				return GargantuanNativeEgressQueueStatus::Rejected;
			#endif
		} else {
			Ticket->DestinationBytes = static_cast<socklen_t>(Remote.ToSockadr(&Ticket->Destination));
		}
		Ticket->Ecn = ResolveECNSendGlobal();
		Ticket->Socket = Socket;
		Ticket->BoundIdentity = BoundSocket;
		if (!Socket->m_GargantuanEgressGeneration)
			Socket->m_GargantuanEgressGeneration = Domain.NextSocketGeneration++;
		Ticket->SocketGeneration = Socket->m_GargantuanEgressGeneration;
		Ticket->QueueSequence = Domain.NextQueueSequence++;
		Ticket->QueuedAtMicroseconds = GargantuanReliableServiceClock();
		Ticket->DatagramBytes = TotalBytes;
		Ticket->Protected = Protected;
		Ticket->OsStarted = false;
		Ticket->CancelRequested = false;
		Ticket->Result = {};
		++Socket->m_GargantuanEgressLeases;
		Ticket->State.store(GargantuanTicketState::Queued, std::memory_order_release);
		Domain.Changed.notify_one();
		return GargantuanNativeEgressQueueStatus::Queued;
	#else
		(void)BoundSocket; (void)ChunkCount; (void)Chunks; (void)TotalBytes; (void)Protected;
		return GargantuanNativeEgressQueueStatus::Disabled;
	#endif
}

bool GargantuanReadNativeEgressResult(
	const GargantuanNativeEgressTicket *Ticket,
	GargantuanNativeEgressResult &Result
) noexcept {
	if (!Ticket || Ticket->State.load(std::memory_order_acquire) != GargantuanTicketState::Ready)
		return false;
	Result = Ticket->Result;
	return true;
}

bool GargantuanWaitNativeEgressResult(
	const GargantuanNativeEgressTicket *Ticket, std::uint32_t TimeoutMilliseconds,
	GargantuanNativeEgressResult &Result
) noexcept {
	if (!Ticket) return false;
	auto &Domain = GargantuanEgress();
	std::unique_lock Lock(Domain.Mutex);
	if (!Domain.Changed.wait_for(Lock, std::chrono::milliseconds(TimeoutMilliseconds), [&] {
		return Ticket->State.load(std::memory_order_acquire) == GargantuanTicketState::Ready;
	})) return false;
	Result = Ticket->Result;
	return true;
}

bool GargantuanCancelUnstartedNativeEgress(GargantuanNativeEgressTicket *Ticket) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	if (!Ticket) return false;
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	const auto State = Ticket->State.load(std::memory_order_relaxed);
	if (State == GargantuanTicketState::Queued) {
		Ticket->Result = {false, 0, GargantuanCancelledError, 0, 0, Ticket->SocketGeneration};
		Ticket->State.store(GargantuanTicketState::Ready, std::memory_order_release);
	} else if (State == GargantuanTicketState::Executing && !Ticket->OsStarted) {
		Ticket->CancelRequested = true;
	} else {
		return false;
	}
	Domain.Changed.notify_all();
	return true;
}

void GargantuanReleaseNativeEgress(GargantuanNativeEgressTicket *Ticket) noexcept {
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	if (!Ticket) return;
	auto &Domain = GargantuanEgress();
	CRawUDPSocketImpl *Socket;
	{
		std::lock_guard Lock(Domain.Mutex);
		if (Ticket->State.load(std::memory_order_acquire) != GargantuanTicketState::Ready)
			return;
		Socket = Ticket->Socket;
		if (Socket && Socket->m_GargantuanEgressGeneration != Ticket->SocketGeneration)
			GargantuanFailStopNativeEgress("socket generation changed before release");
		Ticket->Socket = nullptr;
		Ticket->BoundIdentity = nullptr;
		Ticket->State.store(GargantuanTicketState::Free, std::memory_order_release);
		Domain.Changed.notify_all();
	}
	if (Socket && Socket->m_GargantuanEgressLeases > 0) {
		--Socket->m_GargantuanEgressLeases;
		if (Socket->m_GargantuanEgressClosing)
			WakeServiceThread(); // GNS removes sockets only after its poll completes.
	}
}

[[noreturn]] void GargantuanFailStopNativeEgress(const char *Reason) noexcept {
	// A begun OS attempt with unknown outcome cannot release its socket or SNP
	// references. This path deliberately avoids CRT abort dialogs and boundedly
	// emits a fixed-size diagnostic before terminating this process.
	#ifdef _WIN32
		char Diagnostic[192] = "[Network:NativeEgress] fail-stop: ";
		size_t Offset = sizeof("[Network:NativeEgress] fail-stop: ") - 1;
		if (Reason) {
			while (*Reason && Offset < sizeof(Diagnostic) - 3)
				Diagnostic[Offset++] = *Reason++;
		}
		Diagnostic[Offset++] = '\n';
		Diagnostic[Offset] = '\0';
		OutputDebugStringA(Diagnostic);
		TerminateProcess(GetCurrentProcess(), 94);
	#endif
	std::_Exit(94);
}

#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
bool GargantuanArmNativeEgressPauseForTest(
	GargantuanNativeEgressTestPausePoint Point,
	std::uint32_t MaximumMilliseconds,
	GargantuanNativeEgressTestTraffic Traffic,
	int MinimumDatagramBytes
) noexcept {
	if (MaximumMilliseconds == 0 || MaximumMilliseconds > 250 ||
		MinimumDatagramBytes < 0 || MinimumDatagramBytes > k_cbSteamNetworkingSocketsMaxUDPMsgLen)
		return false;
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	if (Domain.TestPauseArmed || Domain.TestPaused || !Domain.Enabled) return false;
	Domain.TestPausePoint = Point;
	Domain.TestTraffic = Traffic;
	Domain.TestMinimumDatagramBytes = MinimumDatagramBytes;
	Domain.TestMaximumMilliseconds = MaximumMilliseconds;
	Domain.TestReleased = false;
	Domain.TestPauseArmed = true;
	return true;
}

bool GargantuanWaitNativeEgressPauseForTest(std::uint32_t TimeoutMilliseconds) noexcept {
	auto &Domain = GargantuanEgress();
	std::unique_lock Lock(Domain.Mutex);
	return Domain.Changed.wait_for(Lock, std::chrono::milliseconds(TimeoutMilliseconds),
		[&] { return Domain.TestPaused; });
}

void GargantuanResumeNativeEgressForTest() noexcept {
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	Domain.TestReleased = true;
	Domain.Changed.notify_all();
}

bool GargantuanFailNextNativeEgressForTest() noexcept {
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	if (!Domain.Enabled || Domain.TestFailNextArmed) return false;
	Domain.TestFailNextArmed = true;
	return true;
}

std::uint64_t GargantuanNativeEgressFailuresForTest() noexcept {
	auto &Domain = GargantuanEgress();
	std::lock_guard Lock(Domain.Mutex);
	return Domain.TestFailedCount;
}
#endif
